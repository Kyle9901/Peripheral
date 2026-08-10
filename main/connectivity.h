#pragma once

#include "network_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 Wi-Fi STA、TCP 工作任务和协议状态。 */
int connectivity_init(void);

/** 异步应用新接受或从 NVS 恢复的网络配置。 */
int connectivity_apply_config(const network_config_t *config);

/** 当已注册的 TCP 会话在线时，发送一条 JSON 遥测记录。 */
int connectivity_send_telemetry(const char *json);

#ifdef __cplusplus
}
#endif
