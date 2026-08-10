#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float temperature_c;
    float humidity_percent;
    float illuminance_lux;
    bool motion;
    bool motion_detected;
    bool dht_valid;
    bool bh1750_valid;
    uint32_t sampled_at_ms;
} sensor_reading_t;

/** 初始化传感器并启动周期采样与遥测上报任务。 */
int sensor_manager_init(void);

/** 获取最近一次采样结果。 */
bool sensor_manager_get_latest(sensor_reading_t *reading);

#ifdef __cplusplus
}
#endif
