#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 配网状态枚举 */
typedef enum {
    STATUS_IDLE,
    STATUS_CONFIG_RECEIVED,
    STATUS_WIFI_CONNECTING,
    STATUS_WIFI_CONNECTED,
    STATUS_CENTRAL_CONNECTING,
    STATUS_REGISTERING,
    STATUS_OPERATIONAL,
    STATUS_ERROR,
} status_state_t;

/** 错误信息 */
typedef struct {
    char code[64];
    char message[128];
    bool retryable;
    uint32_t retry_after_ms;
} status_error_t;

void status_init(void);
void status_set_state(status_state_t state);
status_state_t status_get_state(void);
const char *status_get_json(void);
void status_set_error(const char *code, const char *message,
                      bool retryable, uint32_t retry_after_ms);
void status_clear_error(void);
const status_error_t *status_get_error(void);
const char *status_get_provisioning_id(void);
void status_set_provisioning_id(const char *id);
uint32_t status_get_sequence(void);
uint32_t status_get_uptime_ms(void);

#ifdef __cplusplus
}
#endif