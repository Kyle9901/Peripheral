#include "bh1750.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <string.h>

#define BH1750_POWER_ON_COMMAND       0x01
#define BH1750_RESET_COMMAND          0x07
#define BH1750_CONTINUOUS_HIGH_RES    0x10
#define BH1750_I2C_TIMEOUT_MS         100

static const char *TAG = "bh1750";

static esp_err_t send_command(bh1750_t *sensor, uint8_t command)
{
    return i2c_master_transmit(sensor->device, &command, 1,
                               BH1750_I2C_TIMEOUT_MS);
}

esp_err_t bh1750_init(bh1750_t *sensor, gpio_num_t sda_gpio,
                      gpio_num_t scl_gpio, uint8_t address)
{
    if (sensor == NULL || !GPIO_IS_VALID_OUTPUT_GPIO(sda_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(scl_gpio) || sda_gpio == scl_gpio ||
        address < 0x08 || address > 0x77) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(sensor, 0, sizeof(*sensor));
    sensor->address = address;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t error = i2c_new_master_bus(&bus_config, &sensor->bus);
    if (error != ESP_OK) {
        return error;
    }
    uint8_t candidate_addresses[2] = {address, address};
    size_t candidate_count = 1;
    if (address == 0x23 || address == 0x5c) {
        candidate_addresses[1] = address == 0x23 ? 0x5c : 0x23;
        candidate_count = 2;
    }

    for (size_t index = 0; index < candidate_count; index++) {
        uint8_t candidate = candidate_addresses[index];
        error = i2c_master_probe(sensor->bus, candidate,
                                 BH1750_I2C_TIMEOUT_MS);
        ESP_LOGI(TAG, "探测地址 0x%02x：%s（SDA=%d，SCL=%d）",
                 candidate, esp_err_to_name(error),
                 gpio_get_level(sda_gpio), gpio_get_level(scl_gpio));
        if (error == ESP_OK) {
            address = candidate;
            sensor->address = candidate;
            break;
        }
    }
    if (error != ESP_OK) {
        i2c_del_master_bus(sensor->bus);
        sensor->bus = NULL;
        return error;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
    };
    error = i2c_master_bus_add_device(sensor->bus, &device_config,
                                      &sensor->device);
    if (error != ESP_OK) {
        i2c_del_master_bus(sensor->bus);
        sensor->bus = NULL;
        return error;
    }
    if ((error = send_command(sensor, BH1750_POWER_ON_COMMAND)) != ESP_OK ||
        (error = send_command(sensor, BH1750_RESET_COMMAND)) != ESP_OK ||
        (error = send_command(sensor, BH1750_CONTINUOUS_HIGH_RES)) != ESP_OK) {
        i2c_master_bus_rm_device(sensor->device);
        i2c_del_master_bus(sensor->bus);
        sensor->device = NULL;
        sensor->bus = NULL;
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(180));
    return ESP_OK;
}

esp_err_t bh1750_read_lux(bh1750_t *sensor, float *lux)
{
    if (sensor == NULL || sensor->device == NULL || lux == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t data[2];
    esp_err_t error = i2c_master_receive(sensor->device, data, sizeof(data),
                                         BH1750_I2C_TIMEOUT_MS);
    if (error != ESP_OK) {
        return error;
    }
    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    *lux = raw / 1.2f;
    return ESP_OK;
}
