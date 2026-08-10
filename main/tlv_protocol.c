#include "tlv_protocol.h"

#include "ble_prov.h"
#include "dev_info.h"
#include "status.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "tlv";

#define SESSION_STEP_TIMEOUT_MS  10000
#define HEARTBEAT_INTERVAL_MS    20000
#define PEER_TIMEOUT_MS          60000

static SemaphoreHandle_t s_send_mutex;

static uint16_t read_be16(const uint8_t *bytes)
{
    return ((uint16_t)bytes[0] << 8) | bytes[1];
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           bytes[3];
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static int send_all(int socket_fd, const uint8_t *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        int result = send(socket_fd, data + sent, length - sent, 0);
        if (result > 0) {
            sent += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int receive_exact(int socket_fd, uint8_t *buffer, size_t length,
                         uint32_t timeout_ms)
{
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (received < length) {
        int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) {
            return 0;
        }
        struct timeval timeout = {
            .tv_sec = (long)(remaining_us / 1000000),
            .tv_usec = (long)(remaining_us % 1000000),
        };
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket_fd, &read_set);
        int selected = select(socket_fd + 1, &read_set, NULL, NULL, &timeout);
        if (selected == 0) {
            return 0;
        }
        if (selected < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        int result = recv(socket_fd, buffer + received, length - received, 0);
        if (result > 0) {
            received += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 1;
}

static int receive_frame(int socket_fd, uint16_t *tag, uint8_t *value,
                         uint32_t value_capacity, uint32_t *value_length,
                         uint32_t timeout_ms)
{
    uint8_t header[TLV_HEADER_SIZE];
    int result = receive_exact(socket_fd, header, sizeof(header), timeout_ms);
    if (result <= 0) {
        return result;
    }

    *tag = read_be16(header);
    *value_length = read_be32(header + 2);
    if (*value_length > value_capacity || *value_length > TLV_MAX_VALUE_SIZE) {
        ESP_LOGE(TAG, "Peer frame is too large: %lu", (unsigned long)*value_length);
        return -2;
    }
    if (*value_length == 0) {
        return 1;
    }
    return receive_exact(socket_fd, value, *value_length, timeout_ms);
}

int tlv_protocol_init(void)
{
    if (s_send_mutex == NULL) {
        s_send_mutex = xSemaphoreCreateMutex();
    }
    return s_send_mutex != NULL ? 0 : -1;
}

int tlv_send_frame(int socket_fd, uint16_t tag,
                   const void *value, uint32_t value_length)
{
    if (socket_fd < 0 || value_length > TLV_MAX_VALUE_SIZE ||
        (value_length != 0 && value == NULL) || s_send_mutex == NULL) {
        return -1;
    }

    uint8_t header[TLV_HEADER_SIZE];
    write_be16(header, tag);
    write_be32(header + 2, value_length);

    if (xSemaphoreTake(s_send_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return -1;
    }
    int result = send_all(socket_fd, header, sizeof(header));
    if (result == 0 && value_length != 0) {
        result = send_all(socket_fd, value, value_length);
    }
    xSemaphoreGive(s_send_mutex);
    return result;
}

static int send_session_open(int socket_fd, const uint8_t *challenge,
                             uint32_t challenge_length)
{
    /*
     * 当前仓库缺少能力协议，无法实现其确切的签名信封格式。暂时在 open
     * 消息中保留不透明 challenge，使帧格式和会话生命周期可测试；取得正式
     * 协议后只需替换该载荷，不必修改传输层。
     */
    uint8_t value[16 + 2 + 256];
    if (challenge_length > 256) {
        return -1;
    }
    memcpy(value, dev_info_get_device_id_raw(), 16);
    write_be16(value + 16, (uint16_t)challenge_length);
    memcpy(value + 18, challenge, challenge_length);
    return tlv_send_frame(socket_fd, TLV_SESSION_OPEN,
                          value, 18 + challenge_length);
}

static int send_manifest(int socket_fd)
{
    const char *manifest =
        "{"
        "\"spec\":\"instacare.device/1.0\","
        "\"manifest_id\":\"instacare-peripheral-base\","
        "\"revision\":2,"
        "\"actions\":[],"
        "\"telemetry\":["
        "{\"name\":\"temperature_c\",\"type\":\"number\",\"unit\":\"celsius\"},"
        "{\"name\":\"humidity_percent\",\"type\":\"number\",\"unit\":\"percent\"},"
        "{\"name\":\"illuminance_lux\",\"type\":\"number\",\"unit\":\"lux\"},"
        "{\"name\":\"motion\",\"type\":\"boolean\"},"
        "{\"name\":\"motion_detected\",\"type\":\"boolean\"}"
        "]"
        "}";
    return tlv_send_frame(socket_fd, TLV_ACTION_REGISTER,
                          manifest, (uint32_t)strlen(manifest));
}

static tlv_session_result_t wait_for_tag(int socket_fd, uint16_t expected_tag,
                                         uint8_t *value, uint32_t capacity,
                                         uint32_t *length)
{
    uint16_t tag = 0;
    int result = receive_frame(socket_fd, &tag, value, capacity, length,
                               SESSION_STEP_TIMEOUT_MS);
    if (result <= 0) {
        return result == 0 ? TLV_SESSION_PROTOCOL_ERROR : TLV_SESSION_DISCONNECTED;
    }
    if (tag == TLV_ERROR) {
        ESP_LOGW(TAG, "Central rejected session step");
        return TLV_SESSION_REJECTED;
    }
    if (tag != expected_tag) {
        ESP_LOGW(TAG, "Expected tag=0x%04x, received=0x%04x", expected_tag, tag);
        return TLV_SESSION_PROTOCOL_ERROR;
    }
    return TLV_SESSION_OK;
}

tlv_session_result_t tlv_run_session(int socket_fd)
{
    uint8_t value[TLV_MAX_VALUE_SIZE];
    uint32_t value_length = 0;

    tlv_session_result_t result = wait_for_tag(
        socket_fd, TLV_SESSION_CHALLENGE, value, 256, &value_length);
    if (result != TLV_SESSION_OK || value_length < 16) {
        return result == TLV_SESSION_OK ? TLV_SESSION_PROTOCOL_ERROR : result;
    }
    if (send_session_open(socket_fd, value, value_length) != 0) {
        return TLV_SESSION_DISCONNECTED;
    }

    result = wait_for_tag(socket_fd, TLV_SESSION_READY,
                          value, sizeof(value), &value_length);
    if (result != TLV_SESSION_OK) {
        return result;
    }

    status_set_state(STATUS_REGISTERING);
    ble_prov_notify_status();
    if (send_manifest(socket_fd) != 0) {
        return TLV_SESSION_DISCONNECTED;
    }
    result = wait_for_tag(socket_fd, TLV_ACTION_REGISTERED,
                          value, sizeof(value), &value_length);
    if (result != TLV_SESSION_OK) {
        return result;
    }

    status_set_state(STATUS_OPERATIONAL);
    ble_prov_notify_status();
    ESP_LOGI(TAG, "Session active and manifest accepted");

    int64_t last_received_us = esp_timer_get_time();
    int64_t last_ping_us = last_received_us;
    while (true) {
        uint16_t tag = 0;
        int frame_result = receive_frame(socket_fd, &tag, value,
                                         sizeof(value), &value_length, 1000);
        int64_t now_us = esp_timer_get_time();
        if (frame_result < 0) {
            return frame_result == -2 ? TLV_SESSION_PROTOCOL_ERROR
                                      : TLV_SESSION_DISCONNECTED;
        }
        if (frame_result > 0) {
            last_received_us = now_us;
            if (tag == TLV_PING) {
                if (tlv_send_frame(socket_fd, TLV_PONG,
                                   value, value_length) != 0) {
                    return TLV_SESSION_DISCONNECTED;
                }
            } else if (tag == TLV_ERROR) {
                ESP_LOGW(TAG, "Central sent a protocol error");
            } else if (tag == TLV_ACTION_INVOKE) {
                const char *not_supported =
                    "{\"ok\":false,\"error\":\"ACTION_NOT_SUPPORTED\"}";
                if (tlv_send_frame(socket_fd, TLV_ACTION_RESULT,
                                   not_supported, strlen(not_supported)) != 0) {
                    return TLV_SESSION_DISCONNECTED;
                }
            }
        }

        if ((now_us - last_ping_us) / 1000 >= HEARTBEAT_INTERVAL_MS) {
            uint64_t uptime = status_get_uptime_ms();
            uint8_t ping_value[8];
            for (int i = 0; i < 8; i++) {
                ping_value[7 - i] = (uint8_t)(uptime >> (i * 8));
            }
            if (tlv_send_frame(socket_fd, TLV_PING,
                               ping_value, sizeof(ping_value)) != 0) {
                return TLV_SESSION_DISCONNECTED;
            }
            last_ping_us = now_us;
        }
        if ((now_us - last_received_us) / 1000 >= PEER_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Central heartbeat timed out");
            return TLV_SESSION_DISCONNECTED;
        }
    }
}

int tlv_send_telemetry_json(int socket_fd, const char *json)
{
    if (json == NULL) {
        return -1;
    }
    size_t length = strlen(json);
    if (length == 0 || length > TLV_MAX_VALUE_SIZE) {
        return -1;
    }
    return tlv_send_frame(socket_fd, TLV_TELEMETRY, json, (uint32_t)length);
}
