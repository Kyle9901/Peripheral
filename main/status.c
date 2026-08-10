#include "status.h"
#include "dev_info.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "status";

static status_state_t s_state = STATUS_IDLE;
static status_error_t s_error;
static bool            s_has_error = false;
static char            s_provisioning_id[64] = {0};
static uint32_t        s_sequence = 0;
static int64_t         s_start_time_us = 0;

static char s_status_json[512];

static const char *state_to_string(status_state_t state)
{
    switch (state) {
    case STATUS_IDLE:              return "idle";
    case STATUS_CONFIG_RECEIVED:   return "config_received";
    case STATUS_WIFI_CONNECTING:   return "wifi_connecting";
    case STATUS_WIFI_CONNECTED:    return "wifi_connected";
    case STATUS_CENTRAL_CONNECTING:return "central_connecting";
    case STATUS_REGISTERING:       return "registering";
    case STATUS_OPERATIONAL:       return "operational";
    case STATUS_ERROR:             return "error";
    default:                       return "unknown";
    }
}

void status_init(void)
{
    s_state = STATUS_IDLE;
    s_has_error = false;
    memset(&s_error, 0, sizeof(s_error));
    s_provisioning_id[0] = '\0';
    s_sequence = 0;
    s_start_time_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Status initialized: idle");
}

void status_set_state(status_state_t state)
{
    if (s_state == state) return;
    ESP_LOGI(TAG, "State: %s -> %s", state_to_string(s_state), state_to_string(state));
    s_state = state;
    s_sequence++;
    if (state != STATUS_ERROR) {
        s_has_error = false;
    }
}

status_state_t status_get_state(void)
{
    return s_state;
}

const char *status_get_json(void)
{
    const char *error_part = "null";
    if (s_has_error) {
        char err_buf[256];
        snprintf(err_buf, sizeof(err_buf),
            "{\"code\":\"%s\",\"message\":\"%s\",\"retryable\":%s,\"retry_after_ms\":%lu}",
            s_error.code,
            s_error.message,
            s_error.retryable ? "true" : "false",
            (unsigned long)s_error.retry_after_ms);
        // 使用静态缓冲区保存错误 JSON。
        static char error_json[256];
        strncpy(error_json, err_buf, sizeof(error_json) - 1);
        error_json[sizeof(error_json) - 1] = '\0';
        error_part = error_json;
    }

    snprintf(s_status_json, sizeof(s_status_json),
        "{"
        "\"spec\":\"instacare.provisioning/1.0\","
        "\"provisioning_id\":\"%s\","
        "\"config_revision\":%llu,"
        "\"sequence\":%lu,"
        "\"state\":\"%s\","
        "\"error\":%s,"
        "\"uptime_ms\":%llu"
        "}",
        s_provisioning_id[0] ? s_provisioning_id : "",
        (unsigned long long)dev_info_get_config_revision(),  // from dev_info.h
        (unsigned long)s_sequence,
        state_to_string(s_state),
        error_part,
        (unsigned long long)((esp_timer_get_time() - s_start_time_us) / 1000)
    );
    return s_status_json;
}

void status_set_error(const char *code, const char *message,
                      bool retryable, uint32_t retry_after_ms)
{
    s_has_error = true;
    strncpy(s_error.code, code, sizeof(s_error.code) - 1);
    s_error.code[sizeof(s_error.code) - 1] = '\0';
    strncpy(s_error.message, message, sizeof(s_error.message) - 1);
    s_error.message[sizeof(s_error.message) - 1] = '\0';
    s_error.retryable = retryable;
    s_error.retry_after_ms = retry_after_ms;
    s_state = STATUS_ERROR;
    s_sequence++;
    ESP_LOGW(TAG, "Error: %s — %s (retryable=%d, after=%lums)",
             code, message, retryable, (unsigned long)retry_after_ms);
}

void status_clear_error(void)
{
    s_has_error = false;
    memset(&s_error, 0, sizeof(s_error));
}

const status_error_t *status_get_error(void)
{
    return s_has_error ? &s_error : NULL;
}

const char *status_get_provisioning_id(void)
{
    return s_provisioning_id;
}

void status_set_provisioning_id(const char *id)
{
    strncpy(s_provisioning_id, id, sizeof(s_provisioning_id) - 1);
    s_provisioning_id[sizeof(s_provisioning_id) - 1] = '\0';
}

uint32_t status_get_sequence(void)
{
    return s_sequence;
}

uint32_t status_get_uptime_ms(void)
{
    return (uint32_t)((esp_timer_get_time() - s_start_time_us) / 1000);
}
