#include "sensor_manager.h"

#include "bh1750.h"
#include "dht_sensor.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdatomic.h>

static const char *TAG = "sensors";

#define SENSOR_POLL_INTERVAL_MS 250
#define DEFAULT_OCCUPANCY_HOLD_SECONDS \
    ((CONFIG_INSTACARE_OCCUPANCY_HOLD_MS + 999) / 1000)

static bh1750_t s_bh1750;
static bool s_bh1750_ready;
static SemaphoreHandle_t s_reading_mutex;
static sensor_reading_t s_latest;
static bool s_has_reading;
static sensor_reading_callback_t s_reading_callback;
static void *s_reading_callback_context;
static atomic_uint_fast16_t s_occupancy_hold_seconds =
    DEFAULT_OCCUPANCY_HOLD_SECONDS;

#if CONFIG_INSTACARE_DHT_MODEL_DHT11
#define INSTACARE_DHT_MODEL DHT_SENSOR_MODEL_DHT11
#define INSTACARE_DHT_MODEL_NAME "DHT11"
#else
#define INSTACARE_DHT_MODEL DHT_SENSOR_MODEL_DHT22
#define INSTACARE_DHT_MODEL_NAME "DHT22"
#endif

static esp_err_t init_pir(void)
{
    gpio_num_t gpio = (gpio_num_t)CONFIG_INSTACARE_PIR_GPIO;
    if (!GPIO_IS_VALID_GPIO(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

static void number_or_null(char *buffer, size_t buffer_size,
                           bool valid, float value)
{
    if (valid) {
        snprintf(buffer, buffer_size, "%.1f", (double)value);
    } else {
        snprintf(buffer, buffer_size, "null");
    }
}

static void store_latest(const sensor_reading_t *reading)
{
    if (xSemaphoreTake(s_reading_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_latest = *reading;
        s_has_reading = true;
        xSemaphoreGive(s_reading_mutex);
    }
}

static void sample_sensors(bool occupied)
{
    sensor_reading_t reading = {
        .motion = gpio_get_level((gpio_num_t)CONFIG_INSTACARE_PIR_GPIO) != 0,
        .occupied = occupied,
        .sampled_at_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
    };

    dht_sensor_reading_t dht_reading;
    esp_err_t dht_error = dht_sensor_read(&dht_reading);
    if (dht_error == ESP_OK) {
        reading.temperature_c = dht_reading.temperature_c;
        reading.humidity_percent = dht_reading.humidity_percent;
        reading.dht_valid = true;
    } else {
        ESP_LOGW(TAG, "%s 读取失败：%s", INSTACARE_DHT_MODEL_NAME,
                 esp_err_to_name(dht_error));
    }

    if (s_bh1750_ready) {
        esp_err_t light_error = bh1750_read_lux(&s_bh1750,
                                                &reading.illuminance_lux);
        reading.bh1750_valid = light_error == ESP_OK;
        if (light_error != ESP_OK) {
            ESP_LOGW(TAG, "BH1750 读取失败：%s", esp_err_to_name(light_error));
        }
    }

    store_latest(&reading);
    char temperature[24];
    char humidity[24];
    char illuminance[24];
    number_or_null(temperature, sizeof(temperature), reading.dht_valid,
                   reading.temperature_c);
    number_or_null(humidity, sizeof(humidity), reading.dht_valid,
                   reading.humidity_percent);
    number_or_null(illuminance, sizeof(illuminance), reading.bh1750_valid,
                   reading.illuminance_lux);
    ESP_LOGI(TAG,
             "温度=%s%s 湿度=%s%s 光照=%s%s PIR电平=%s Matter占用=%s",
             temperature,
             reading.dht_valid ? " C" : "",
             humidity,
             reading.dht_valid ? " %" : "",
             illuminance,
             reading.bh1750_valid ? " lux" : "",
             reading.motion ? "高" : "低",
             reading.occupied ? "是" : "否");
    if (s_reading_callback != NULL) {
        s_reading_callback(&reading, s_reading_callback_context);
    }
}

static void sensor_task(void *argument)
{
    (void)argument;
    /* DHT11 上电后需要至少 1 秒稳定时间。 */
    vTaskDelay(pdMS_TO_TICKS(1000));
    TickType_t report_interval = pdMS_TO_TICKS(
        CONFIG_INSTACARE_SENSOR_REPORT_INTERVAL_MS);
    TickType_t next_report = xTaskGetTickCount();
    TickType_t last_motion = 0;
    bool has_detected_motion = false;
    while (true) {
        if (gpio_get_level((gpio_num_t)CONFIG_INSTACARE_PIR_GPIO) != 0) {
            last_motion = xTaskGetTickCount();
            has_detected_motion = true;
        }
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_report) >= 0) {
            uint16_t hold_seconds = sensor_manager_get_occupancy_hold_seconds();
            TickType_t occupancy_hold = pdMS_TO_TICKS(
                (uint32_t)hold_seconds * 1000U);
            bool occupied = has_detected_motion &&
                (TickType_t)(now - last_motion) <= occupancy_hold;
            sample_sensors(occupied);
            next_report = now + report_interval;
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS));
    }
}

int sensor_manager_init(sensor_reading_callback_t callback, void *context)
{
    s_reading_callback = callback;
    s_reading_callback_context = context;
    s_reading_mutex = xSemaphoreCreateMutex();
    if (s_reading_mutex == NULL) {
        return -1;
    }

    esp_err_t error = init_pir();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "HC-SR501 GPIO 初始化失败：%s", esp_err_to_name(error));
        return -1;
    }
    error = dht_sensor_init((gpio_num_t)CONFIG_INSTACARE_DHT_GPIO,
                            INSTACARE_DHT_MODEL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "%s GPIO 初始化失败：%s", INSTACARE_DHT_MODEL_NAME,
                 esp_err_to_name(error));
        return -1;
    }

    error = bh1750_init(&s_bh1750,
                        (gpio_num_t)CONFIG_INSTACARE_BH1750_SDA_GPIO,
                        (gpio_num_t)CONFIG_INSTACARE_BH1750_SCL_GPIO,
                        CONFIG_INSTACARE_BH1750_ADDRESS);
    s_bh1750_ready = error == ESP_OK;
    if (!s_bh1750_ready) {
        ESP_LOGW(TAG, "BH1750 初始化失败（地址 0x%02x）：%s",
                 CONFIG_INSTACARE_BH1750_ADDRESS, esp_err_to_name(error));
    }

    if (xTaskCreate(sensor_task, "instacare_sensors", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "无法创建传感器任务");
        return -1;
    }
    ESP_LOGI(TAG,
             "传感器已启动：%s=GPIO%d, PIR=GPIO%d, BH1750 SDA=%d SCL=%d 地址=0x%02x，占用保持=%u秒",
             INSTACARE_DHT_MODEL_NAME, CONFIG_INSTACARE_DHT_GPIO,
             CONFIG_INSTACARE_PIR_GPIO,
             CONFIG_INSTACARE_BH1750_SDA_GPIO,
             CONFIG_INSTACARE_BH1750_SCL_GPIO,
             s_bh1750_ready ? s_bh1750.address
                            : CONFIG_INSTACARE_BH1750_ADDRESS,
             sensor_manager_get_occupancy_hold_seconds());
    return 0;
}

int sensor_manager_set_occupancy_hold_seconds(uint16_t seconds)
{
    if (seconds < SENSOR_OCCUPANCY_HOLD_MIN_SECONDS ||
        seconds > SENSOR_OCCUPANCY_HOLD_MAX_SECONDS) {
        return -1;
    }
    atomic_store_explicit(&s_occupancy_hold_seconds, seconds,
                          memory_order_relaxed);
    ESP_LOGI(TAG, "占用保持时间已调整为 %u 秒", seconds);
    return 0;
}

uint16_t sensor_manager_get_occupancy_hold_seconds(void)
{
    return (uint16_t)atomic_load_explicit(&s_occupancy_hold_seconds,
                                          memory_order_relaxed);
}

bool sensor_manager_get_latest(sensor_reading_t *reading)
{
    if (reading == NULL || s_reading_mutex == NULL ||
        xSemaphoreTake(s_reading_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    bool available = s_has_reading;
    if (available) {
        *reading = s_latest;
    }
    xSemaphoreGive(s_reading_mutex);
    return available;
}
