#include "connectivity.h"

#include "ble_prov.h"
#include "status.h"
#include "tlv_protocol.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "connectivity";

#define WIFI_GOT_IP_BIT       BIT0
#define WIFI_DISCONNECTED_BIT BIT1
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define TCP_CONNECT_TIMEOUT_MS  10000
#define RETRY_INITIAL_MS         1000
#define RETRY_MAX_MS            60000

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_config_mutex;
static SemaphoreHandle_t s_socket_mutex;
static TaskHandle_t s_worker_task;
static network_config_t s_config;
static bool s_has_config;
static int s_socket = -1;
static uint8_t s_disconnect_reason;

static void publish_state(status_state_t state)
{
    status_set_state(state);
    ble_prov_notify_status();
}

static void publish_error(const char *code, const char *message,
                          uint32_t retry_after_ms)
{
    status_set_error(code, message, true, retry_after_ms);
    ble_prov_notify_status();
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        s_disconnect_reason = event != NULL ? event->reason : 0;
        xEventGroupClearBits(s_wifi_events, WIFI_GOT_IP_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupClearBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_GOT_IP_BIT);
    }
}

static bool copy_current_config(network_config_t *config)
{
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    bool available = s_has_config;
    if (available) {
        *config = s_config;
    }
    xSemaphoreGive(s_config_mutex);
    return available;
}

static bool is_auth_failure(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_EXPIRE ||
           reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static int connect_wifi(const network_config_t *config,
                        const char **error_code, const char **error_message)
{
    wifi_config_t wifi_config = {0};
    size_t ssid_length = strlen(config->ssid);
    size_t password_length = strlen(config->password);
    memcpy(wifi_config.sta.ssid, config->ssid, ssid_length);
    memcpy(wifi_config.sta.password, config->password, password_length);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.rssi = -127;
    switch (config->security) {
    case NETWORK_SECURITY_OPEN:
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        break;
    case NETWORK_SECURITY_WPA2_PSK:
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        break;
    case NETWORK_SECURITY_WPA3_SAE:
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;
        break;
    default:
        *error_code = "INVALID_CONFIG";
        *error_message = "Unsupported Wi-Fi security type";
        return -1;
    }

    publish_state(STATUS_WIFI_CONNECTING);
    xEventGroupClearBits(s_wifi_events, WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT);
    esp_wifi_disconnect();
    esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (error == ESP_OK) {
        xEventGroupClearBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
        error = esp_wifi_connect();
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(error));
        *error_code = "INTERNAL_ERROR";
        *error_message = "Failed to start Wi-Fi connection";
        return -1;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t total = pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS);
    while (xTaskGetTickCount() - start < total) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events, WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT,
            pdFALSE, pdFALSE, total - elapsed);
        if ((bits & WIFI_GOT_IP_BIT) != 0) {
            publish_state(STATUS_WIFI_CONNECTED);
            return 0;
        }
        if ((bits & WIFI_DISCONNECTED_BIT) != 0) {
            xEventGroupClearBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
            if (s_disconnect_reason == WIFI_REASON_NO_AP_FOUND) {
                *error_code = "WIFI_NOT_FOUND";
                *error_message = "Configured Wi-Fi network was not found";
                return -1;
            }
            if (is_auth_failure(s_disconnect_reason)) {
                *error_code = "WIFI_AUTH_FAILED";
                *error_message = "Wi-Fi authentication failed";
                return -1;
            }
            /* 在 30 秒连接窗口内重试临时的 Wi-Fi 关联失败。 */
            esp_wifi_connect();
        }
    }

    *error_code = "DHCP_FAILED";
    *error_message = "Wi-Fi connection or DHCP timed out";
    return -1;
}

static int wait_for_connect(int socket_fd, uint32_t timeout_ms)
{
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(socket_fd, &write_set);
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int selected = select(socket_fd + 1, NULL, &write_set, NULL, &timeout);
    if (selected <= 0) {
        return -1;
    }
    int socket_error = 0;
    socklen_t error_length = sizeof(socket_error);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR,
                   &socket_error, &error_length) != 0 || socket_error != 0) {
        return -1;
    }
    return 0;
}

static int connect_central(const network_config_t *config, bool *dns_failed)
{
    char port[6];
    snprintf(port, sizeof(port), "%u", config->central_port);
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
    };
    struct addrinfo *addresses = NULL;
    int resolved = getaddrinfo(config->central_address, port, &hints, &addresses);
    if (resolved != 0 || addresses == NULL) {
        *dns_failed = true;
        ESP_LOGW(TAG, "Could not resolve central address: %d", resolved);
        return -1;
    }

    int connected_socket = -1;
    for (const struct addrinfo *address = addresses;
         address != NULL; address = address->ai_next) {
        int candidate = socket(address->ai_family, address->ai_socktype,
                               address->ai_protocol);
        if (candidate < 0) {
            continue;
        }
        int flags = fcntl(candidate, F_GETFL, 0);
        fcntl(candidate, F_SETFL, flags | O_NONBLOCK);
        int result = connect(candidate, address->ai_addr, address->ai_addrlen);
        if (result == 0 || (errno == EINPROGRESS &&
                            wait_for_connect(candidate, TCP_CONNECT_TIMEOUT_MS) == 0)) {
            fcntl(candidate, F_SETFL, flags);
            int enabled = 1;
            int idle_seconds = 30;
            int interval_seconds = 10;
            int probe_count = 3;
            setsockopt(candidate, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
            setsockopt(candidate, IPPROTO_TCP, TCP_KEEPIDLE,
                       &idle_seconds, sizeof(idle_seconds));
            setsockopt(candidate, IPPROTO_TCP, TCP_KEEPINTVL,
                       &interval_seconds, sizeof(interval_seconds));
            setsockopt(candidate, IPPROTO_TCP, TCP_KEEPCNT,
                       &probe_count, sizeof(probe_count));
            connected_socket = candidate;
            break;
        }
        close(candidate);
    }
    freeaddrinfo(addresses);
    return connected_socket;
}

static void set_active_socket(int socket_fd)
{
    xSemaphoreTake(s_socket_mutex, portMAX_DELAY);
    s_socket = socket_fd;
    xSemaphoreGive(s_socket_mutex);
}

static void close_active_socket(int expected_socket)
{
    xSemaphoreTake(s_socket_mutex, portMAX_DELAY);
    if (s_socket == expected_socket) {
        shutdown(s_socket, SHUT_RDWR);
        close(s_socket);
        s_socket = -1;
    }
    xSemaphoreGive(s_socket_mutex);
}

static bool delay_or_reconfigured(uint32_t delay_ms)
{
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms)) != 0;
}

static void connectivity_worker(void *argument)
{
    (void)argument;
    uint32_t retry_cap_ms = RETRY_INITIAL_MS;

    while (true) {
        network_config_t config;
        if (!copy_current_config(&config)) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const char *wifi_error_code = NULL;
        const char *wifi_error_message = NULL;
        if ((xEventGroupGetBits(s_wifi_events) & WIFI_GOT_IP_BIT) == 0 &&
            connect_wifi(&config, &wifi_error_code, &wifi_error_message) != 0) {
            uint32_t delay_ms = esp_random() % (retry_cap_ms + 1);
            publish_error(wifi_error_code, wifi_error_message, delay_ms);
            retry_cap_ms = retry_cap_ms < RETRY_MAX_MS / 2
                               ? retry_cap_ms * 2 : RETRY_MAX_MS;
            delay_or_reconfigured(delay_ms);
            continue;
        }

        publish_state(STATUS_CENTRAL_CONNECTING);
        bool dns_failed = false;
        int socket_fd = connect_central(&config, &dns_failed);
        if (socket_fd < 0) {
            uint32_t delay_ms = esp_random() % (retry_cap_ms + 1);
            publish_error(dns_failed ? "DNS_FAILED" : "CENTRAL_UNREACHABLE",
                          dns_failed ? "Central address could not be resolved"
                                     : "TCP connection failed",
                          delay_ms);
            retry_cap_ms = retry_cap_ms < RETRY_MAX_MS / 2
                               ? retry_cap_ms * 2 : RETRY_MAX_MS;
            delay_or_reconfigured(delay_ms);
            continue;
        }

        ESP_LOGI(TAG, "Connected to central %s:%u",
                 config.central_address, config.central_port);
        set_active_socket(socket_fd);
        tlv_session_result_t session_result = tlv_run_session(socket_fd);
        bool was_operational = status_get_state() == STATUS_OPERATIONAL;
        close_active_socket(socket_fd);

        if ((xEventGroupGetBits(s_wifi_events) & WIFI_GOT_IP_BIT) == 0 ||
            was_operational) {
            retry_cap_ms = RETRY_INITIAL_MS;
        } else {
            retry_cap_ms = retry_cap_ms < RETRY_MAX_MS / 2
                               ? retry_cap_ms * 2 : RETRY_MAX_MS;
        }
        uint32_t delay_ms = esp_random() % (retry_cap_ms + 1);
        if (session_result == TLV_SESSION_REJECTED) {
            publish_error("REGISTRATION_REJECTED",
                          "Central rejected the session or manifest", delay_ms);
        } else if (session_result == TLV_SESSION_PROTOCOL_ERROR) {
            publish_error("SESSION_FAILED", "Invalid or timed out TLV session",
                          delay_ms);
        } else {
            publish_error("CENTRAL_UNREACHABLE", "TCP session disconnected",
                          delay_ms);
        }
        delay_or_reconfigured(delay_ms);
    }
}

int connectivity_init(void)
{
    s_wifi_events = xEventGroupCreate();
    s_config_mutex = xSemaphoreCreateMutex();
    s_socket_mutex = xSemaphoreCreateMutex();
    if (s_wifi_events == NULL || s_config_mutex == NULL || s_socket_mutex == NULL ||
        tlv_protocol_init() != 0) {
        return -1;
    }

    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(error));
        return -1;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(error));
        return -1;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi STA netif");
        return -1;
    }

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wifi_init) != ESP_OK ||
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   wifi_event_handler, NULL) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   wifi_event_handler, NULL) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Wi-Fi STA");
        return -1;
    }

    if (xTaskCreate(connectivity_worker, "instacare_net", 12288, NULL, 5,
                    &s_worker_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create connectivity task");
        return -1;
    }
    ESP_LOGI(TAG, "Wi-Fi/TCP worker initialized");
    return 0;
}

int connectivity_apply_config(const network_config_t *config)
{
    if (config == NULL || s_config_mutex == NULL || s_worker_task == NULL) {
        return -1;
    }
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }
    s_config = *config;
    s_has_config = true;
    xSemaphoreGive(s_config_mutex);

    xSemaphoreTake(s_socket_mutex, portMAX_DELAY);
    if (s_socket >= 0) {
        shutdown(s_socket, SHUT_RDWR);
    }
    xSemaphoreGive(s_socket_mutex);
    xTaskNotifyGive(s_worker_task);
    return 0;
}

int connectivity_send_telemetry(const char *json)
{
    if (json == NULL || status_get_state() != STATUS_OPERATIONAL) {
        return -1;
    }
    if (xSemaphoreTake(s_socket_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }
    int result = s_socket >= 0 ? tlv_send_telemetry_json(s_socket, json) : -1;
    xSemaphoreGive(s_socket_mutex);
    return result;
}
