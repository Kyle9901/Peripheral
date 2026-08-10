#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETWORK_CONFIG_SSID_MAX          32
#define NETWORK_CONFIG_PASSWORD_MAX      64
#define NETWORK_CONFIG_HOST_MAX          253
#define NETWORK_CONFIG_PROVISIONING_MAX  63

typedef enum {
    NETWORK_SECURITY_OPEN = 0,
    NETWORK_SECURITY_WPA2_PSK,
    NETWORK_SECURITY_WPA3_SAE,
} network_security_t;

typedef struct {
    uint64_t revision;
    char provisioning_id[NETWORK_CONFIG_PROVISIONING_MAX + 1];
    char ssid[NETWORK_CONFIG_SSID_MAX + 1];
    char password[NETWORK_CONFIG_PASSWORD_MAX + 1];
    network_security_t security;
    char central_address[NETWORK_CONFIG_HOST_MAX + 1];
    uint16_t central_port;
} network_config_t;

typedef enum {
    NETWORK_CONFIG_OK = 0,
    NETWORK_CONFIG_INVALID,
    NETWORK_CONFIG_STALE,
    NETWORK_CONFIG_CONFLICT,
    NETWORK_CONFIG_STORAGE_ERROR,
} network_config_result_t;

/** 从 NVS 加载最近一次接受的网络配置。 */
int network_config_init(void);

/** 如果存在已接受的配置，将其复制到输出参数。 */
bool network_config_get(network_config_t *out_config);

/**
 * 解析、校验并原子持久化 NetworkConfig JSON 文档。
 * 重复提交内容相同的配网事务时保持幂等。
 */
network_config_result_t network_config_submit_json(
    const char *json,
    size_t json_len,
    network_config_t *out_config,
    char *error_message,
    size_t error_message_size);

#ifdef __cplusplus
}
#endif
