#include "dht_sensor.h"

#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>

static gpio_num_t s_gpio = GPIO_NUM_NC;
static dht_sensor_model_t s_model = DHT_SENSOR_MODEL_DHT11;
static portMUX_TYPE s_timing_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *TAG = "dht";

typedef enum {
    DHT_TIMEOUT_NONE,
    DHT_TIMEOUT_RESPONSE_LOW,
    DHT_TIMEOUT_RESPONSE_HIGH,
    DHT_TIMEOUT_DATA_START,
    DHT_TIMEOUT_BIT_HIGH,
    DHT_TIMEOUT_BIT_LOW,
} dht_timeout_stage_t;

typedef struct {
    dht_timeout_stage_t stage;
    int bit_index;
    int line_level;
} dht_diagnostic_t;

static bool wait_for_level(int level, uint32_t timeout_us)
{
    int64_t deadline = esp_timer_get_time() + timeout_us;
    while (gpio_get_level(s_gpio) != level) {
        if (esp_timer_get_time() >= deadline) {
            return false;
        }
    }
    return true;
}

static bool wait_and_record(int level, uint32_t timeout_us,
                            dht_timeout_stage_t stage, int bit_index,
                            dht_diagnostic_t *diagnostic)
{
    if (wait_for_level(level, timeout_us)) {
        return true;
    }
    diagnostic->stage = stage;
    diagnostic->bit_index = bit_index;
    diagnostic->line_level = gpio_get_level(s_gpio);
    return false;
}

static bool read_frame(uint8_t frame[5], dht_diagnostic_t *diagnostic)
{
    /* 两种 DHT 的应答均为约 80us 低、80us 高，随后传输 40 位数据。 */
    if (!wait_and_record(0, 160, DHT_TIMEOUT_RESPONSE_LOW, -1,
                         diagnostic) ||
        !wait_and_record(1, 160, DHT_TIMEOUT_RESPONSE_HIGH, -1,
                         diagnostic) ||
        !wait_and_record(0, 160, DHT_TIMEOUT_DATA_START, -1,
                         diagnostic)) {
        return false;
    }

    for (int bit_index = 0; bit_index < 40; bit_index++) {
        if (!wait_and_record(1, 100, DHT_TIMEOUT_BIT_HIGH, bit_index,
                             diagnostic)) {
            return false;
        }
        int64_t high_started = esp_timer_get_time();
        if (!wait_and_record(0, 130, DHT_TIMEOUT_BIT_LOW, bit_index,
                             diagnostic)) {
            return false;
        }
        uint32_t high_duration = (uint32_t)(esp_timer_get_time() - high_started);
        frame[bit_index / 8] <<= 1;
        if (high_duration > 50) {
            frame[bit_index / 8] |= 1;
        }
    }
    return true;
}

static void log_timeout(const dht_diagnostic_t *diagnostic, int idle_level)
{
    switch (diagnostic->stage) {
    case DHT_TIMEOUT_RESPONSE_LOW:
        ESP_LOGW(TAG,
                 "GPIO%d 等待传感器响应低电平超时（读取前=%d，当前=%d）："
                 "请检查 DATA 引脚、共地、供电和上拉电阻",
                 s_gpio, idle_level, diagnostic->line_level);
        break;
    case DHT_TIMEOUT_RESPONSE_HIGH:
        ESP_LOGW(TAG,
                 "GPIO%d 等待传感器响应高电平超时（当前=%d）："
                 "数据线可能被拉低或缺少上拉",
                 s_gpio, diagnostic->line_level);
        break;
    case DHT_TIMEOUT_DATA_START:
        ESP_LOGW(TAG, "GPIO%d 等待数据起始低电平超时（当前=%d）",
                 s_gpio, diagnostic->line_level);
        break;
    case DHT_TIMEOUT_BIT_HIGH:
        ESP_LOGW(TAG, "GPIO%d 等待第 %d 位高电平超时（当前=%d）",
                 s_gpio, diagnostic->bit_index, diagnostic->line_level);
        break;
    case DHT_TIMEOUT_BIT_LOW:
        ESP_LOGW(TAG, "GPIO%d 等待第 %d 位结束低电平超时（当前=%d）",
                 s_gpio, diagnostic->bit_index, diagnostic->line_level);
        break;
    case DHT_TIMEOUT_NONE:
    default:
        ESP_LOGW(TAG, "GPIO%d 读取超时", s_gpio);
        break;
    }
}

esp_err_t dht_sensor_init(gpio_num_t gpio_num, dht_sensor_model_t model)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio_num) ||
        (model != DHT_SENSOR_MODEL_DHT11 &&
         model != DHT_SENSOR_MODEL_DHT22)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_gpio = gpio_num;
    s_model = model;
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&config);
    if (error == ESP_OK) {
        gpio_set_level(s_gpio, 1);
    }
    return error;
}

static esp_err_t decode_dht11(const uint8_t frame[5],
                              dht_sensor_reading_t *reading)
{
    reading->humidity_percent = frame[0] + frame[1] / 10.0f;
    reading->temperature_c = frame[2] + (frame[3] & 0x7f) / 10.0f;
    if ((frame[3] & 0x80) != 0) {
        reading->temperature_c = -reading->temperature_c;
    }
    if (reading->humidity_percent > 100.0f ||
        reading->temperature_c < -20.0f || reading->temperature_c > 60.0f) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t decode_dht22(const uint8_t frame[5],
                              dht_sensor_reading_t *reading)
{
    uint16_t humidity_raw = ((uint16_t)frame[0] << 8) | frame[1];
    uint16_t temperature_raw = ((uint16_t)(frame[2] & 0x7f) << 8) | frame[3];
    reading->humidity_percent = humidity_raw / 10.0f;
    reading->temperature_c = temperature_raw / 10.0f;
    if ((frame[2] & 0x80) != 0) {
        reading->temperature_c = -reading->temperature_c;
    }
    if (reading->humidity_percent > 100.0f ||
        reading->temperature_c < -40.0f || reading->temperature_c > 80.0f) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t dht_sensor_read(dht_sensor_reading_t *reading)
{
    if (reading == NULL || s_gpio == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_ARG;
    }

    int idle_level = gpio_get_level(s_gpio);

    /* DHT11 要求至少 18ms；DHT22 使用 2ms 即可。 */
    gpio_set_direction(s_gpio, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(s_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(
        s_model == DHT_SENSOR_MODEL_DHT11 ? 20 : 2));

    uint8_t frame[5] = {0};
    dht_diagnostic_t diagnostic = {
        .stage = DHT_TIMEOUT_NONE,
        .bit_index = -1,
        .line_level = -1,
    };
    portENTER_CRITICAL(&s_timing_lock);
    gpio_set_level(s_gpio, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(s_gpio, GPIO_MODE_INPUT);
    bool received = read_frame(frame, &diagnostic);
    portEXIT_CRITICAL(&s_timing_lock);

    gpio_set_direction(s_gpio, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(s_gpio, 1);
    if (!received) {
        log_timeout(&diagnostic, idle_level);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t checksum = (uint8_t)(frame[0] + frame[1] + frame[2] + frame[3]);
    if (checksum != frame[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    return s_model == DHT_SENSOR_MODEL_DHT11
               ? decode_dht11(frame, reading)
               : decode_dht22(frame, reading);
}
