#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 BLE Provisioning Service。
 * 完成后开始 Extended Advertising。
 */
int ble_prov_init(void);

/**
 * 发送 Status 特征 Notify。
 * 当状态变化时调用，通知已连接的 Central。
 */
void ble_prov_notify_status(void);

#ifdef __cplusplus
}
#endif