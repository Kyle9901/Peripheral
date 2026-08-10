#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t device;
    uint8_t address;
} bh1750_t;

/** 创建 I2C 主机并初始化 BH1750 连续高分辨率测量模式。 */
esp_err_t bh1750_init(bh1750_t *sensor, gpio_num_t sda_gpio,
                      gpio_num_t scl_gpio, uint8_t address);

/** 读取当前光照强度，单位为 lux。 */
esp_err_t bh1750_read_lux(bh1750_t *sensor, float *lux);

#ifdef __cplusplus
}
#endif
