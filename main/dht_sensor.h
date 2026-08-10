#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float temperature_c;
    float humidity_percent;
} dht_sensor_reading_t;

typedef enum {
    DHT_SENSOR_MODEL_DHT11,
    DHT_SENSOR_MODEL_DHT22,
} dht_sensor_model_t;

/** 初始化 DHT11/DHT22 单总线数据引脚，并选择对应的数据格式。 */
esp_err_t dht_sensor_init(gpio_num_t gpio_num, dht_sensor_model_t model);

/** 读取一次温湿度；相邻两次调用应至少间隔 2 秒。 */
esp_err_t dht_sensor_read(dht_sensor_reading_t *reading);

#ifdef __cplusplus
}
#endif
