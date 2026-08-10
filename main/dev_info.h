#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 获取 DeviceInfo JSON 字符串。
 * 调用方不得释放返回的指针。
 */
const char *dev_info_get_json(void);

/**
 * 获取 device_id 的 32 字符小写 hex 字符串。
 */
const char *dev_info_get_device_id_hex(void);

/**
 * 获取 device_id 的 16 字节原始值。
 */
const uint8_t *dev_info_get_device_id_raw(void);

/**
 * 获取当前 config_revision。
 */
uint64_t dev_info_get_config_revision(void);

/**
 * 更新 config_revision（收到新配置后）。
 */
void dev_info_set_config_revision(uint64_t revision);

/**
 * 初始化：加载或生成 device_id、读取 NVS。
 */
int dev_info_init(void);

#ifdef __cplusplus
}
#endif