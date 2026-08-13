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

typedef struct {
    bool dht;
    bool bh1750;
    bool pir;
} sensor_capabilities_t;

typedef enum {
    SENSOR_CAPABILITY_DHT,
    SENSOR_CAPABILITY_BH1750,
    SENSOR_CAPABILITY_PIR,
} sensor_capability_t;

/** 运行中首次确认某个传感器存在时触发。 */
typedef void (*sensor_capability_callback_t)(sensor_capability_t capability,
                                             void *context);

/** PIR 占用状态变化时触发，不受环境数据采样周期限制。 */
typedef void (*sensor_occupancy_callback_t)(bool occupied, void *context);

#define SENSOR_OCCUPANCY_HOLD_MIN_SECONDS 1
#define SENSOR_OCCUPANCY_HOLD_MAX_SECONDS 600

/**
 * 探测当前接入的传感器，并返回启动时应发布的 Matter 能力。
 * DHT 和 BH1750 通过协议应答识别；PIR 从 NVS 中读取已经确认的状态。
 * 必须在创建 Matter Endpoint 前调用。
 */
int sensor_manager_configure(sensor_capabilities_t *capabilities);

/** 启动周期采样、热插入探测和 PIR 实时监听。 */
int sensor_manager_start(sensor_reading_callback_t reading_callback,
                         sensor_occupancy_callback_t occupancy_callback,
                         sensor_capability_callback_t capability_callback,
                         void *context);

/**
 * 动态设置 PIR 最近一次检测到移动后，“有人”状态的保持时间。
 * 该接口可在 sensor_manager_start() 前调用。
 */
int sensor_manager_set_occupancy_hold_seconds(uint16_t seconds);

/** 获取当前 PIR 占用保持时间，单位为秒。 */
uint16_t sensor_manager_get_occupancy_hold_seconds(void);

/** 获取最近一次采样结果。 */
bool sensor_manager_get_latest(sensor_reading_t *reading);

#ifdef __cplusplus
}
#endif
