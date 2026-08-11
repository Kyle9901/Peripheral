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
    bool occupied;
    bool dht_valid;
    bool bh1750_valid;
    uint32_t sampled_at_ms;
} sensor_reading_t;

typedef void (*sensor_reading_callback_t)(const sensor_reading_t *reading,
                                          void *context);

#define SENSOR_OCCUPANCY_HOLD_MIN_SECONDS 1
#define SENSOR_OCCUPANCY_HOLD_MAX_SECONDS 600

/** 初始化传感器并启动周期采样；每次新读数通过回调交给 Matter 层。 */
int sensor_manager_init(sensor_reading_callback_t callback, void *context);

/**
 * 动态设置 PIR 最近一次检测到移动后，“有人”状态的保持时间。
 * 该接口可在 sensor_manager_init() 前调用。
 */
int sensor_manager_set_occupancy_hold_seconds(uint16_t seconds);

/** 获取当前 PIR 占用保持时间，单位为秒。 */
uint16_t sensor_manager_get_occupancy_hold_seconds(void);

/** 获取最近一次采样结果。 */
bool sensor_manager_get_latest(sensor_reading_t *reading);

#ifdef __cplusplus
}
#endif
