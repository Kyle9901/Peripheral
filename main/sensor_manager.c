#include "sensor_manager.h"

#include "bh1750.h"
#include "connectivity.h"
#include "dht_sensor.h"
#include "status.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "sensors";

#define SENSOR_POLL_INTERVAL_MS 250

static bh1750_t s_bh1750;
static bool s_bh1750_ready;
static SemaphoreHandle_t s_reading_mutex;
static sensor_reading_t s_latest;
static bool s_has_reading;
static uint32_t s_telemetry_sequence;

#if CONFIG_INSTACARE_DHT_MODEL_DHT11
#define INSTACARE_DHT_MODEL DHT_SENSOR_MODEL_DHT11
#define INSTACARE_DHT_MODEL_NAME "DHT11"
#define INSTACARE_DHT_MODEL_ID "dht11"
#else
#define INSTACARE_DHT_MODEL DHT_SENSOR_MODEL_DHT22
#define INSTACARE_DHT_MODEL_NAME "DHT22"
#define INSTACARE_DHT_MODEL_ID "dht22"
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

static void publish_reading(const sensor_reading_t *reading)
{
    char temperature[24];
    char humidity[24];
    char illuminance[24];
    number_or_null(temperature, sizeof(temperature), reading->dht_valid,
                   reading->temperature_c);
    number_or_null(humidity, sizeof(humidity), reading->dht_valid,
                   reading->humidity_percent);
    number_or_null(illuminance, sizeof(illuminance), reading->bh1750_valid,
                   reading->illuminance_lux);

    char json[512];
    int length = snprintf(
        json, sizeof(json),
        "{"
        "\"spec\":\"instacare.device/1.0\","
        "\"sequence\":%lu,"
        "\"uptime_ms\":%lu,"
        "\"dht_model\":\"%s\","
        "\"values\":{"
        "\"temperature_c\":%s,"
        "\"humidity_percent\":%s,"
        "\"illuminance_lux\":%s,"
        "\"motion\":%s,"
        "\"motion_detected\":%s"
        "},"
        "\"quality\":{\"dht\":\"%s\",\"bh1750\":\"%s\"}"
        "}",
        (unsigned long)s_telemetry_sequence,
        (unsigned long)reading->sampled_at_ms,
        INSTACARE_DHT_MODEL_ID,
        temperature, humidity, illuminance,
        reading->motion ? "true" : "false",
        reading->motion_detected ? "true" : "false",
        reading->dht_valid ? "ok" : "read_error",
        reading->bh1750_valid ? "ok" : "read_error");
    if (length <= 0 || length >= sizeof(json)) {
        ESP_LOGE(TAG, "遥测 JSON 超出缓冲区");
        return;
    }
    if (connectivity_send_telemetry(json) == 0) {
        ESP_LOGI(TAG, "遥测已发送，sequence=%lu",
                 (unsigned long)s_telemetry_sequence);
        s_telemetry_sequence++;
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

static void sample_sensors(bool motion_detected)
{
    sensor_reading_t reading = {
        .motion = gpio_get_level((gpio_num_t)CONFIG_INSTACARE_PIR_GPIO) != 0,
        .motion_detected = motion_detected,
        .sampled_at_ms = status_get_uptime_ms(),
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
             "温度=%s%s 湿度=%s%s 光照=%s%s 人体=%s 本周期触发=%s",
             temperature,
             reading.dht_valid ? " C" : "",
             humidity,
             reading.dht_valid ? " %" : "",
             illuminance,
             reading.bh1750_valid ? " lux" : "",
             reading.motion ? "有" : "无",
             reading.motion_detected ? "是" : "否");
    publish_reading(&reading);
}

static void sensor_task(void *argument)
{
    (void)argument;
    /* DHT11 上电后需要至少 1 秒稳定时间。 */
    vTaskDelay(pdMS_TO_TICKS(1000));
    TickType_t report_interval = pdMS_TO_TICKS(
        CONFIG_INSTACARE_SENSOR_REPORT_INTERVAL_MS);
    TickType_t next_report = xTaskGetTickCount();
    bool motion_detected = false;

    while (true) {
        if (gpio_get_level((gpio_num_t)CONFIG_INSTACARE_PIR_GPIO) != 0) {
            motion_detected = true;
        }
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_report) >= 0) {
            sample_sensors(motion_detected);
            motion_detected = false;
            next_report = now + report_interval;
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS));
    }
}

int sensor_manager_init(void)
{
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
             "传感器已启动：%s=GPIO%d, PIR=GPIO%d, BH1750 SDA=%d SCL=%d 地址=0x%02x",
             INSTACARE_DHT_MODEL_NAME, CONFIG_INSTACARE_DHT_GPIO,
             CONFIG_INSTACARE_PIR_GPIO,
             CONFIG_INSTACARE_BH1750_SDA_GPIO,
             CONFIG_INSTACARE_BH1750_SCL_GPIO,
             s_bh1750_ready ? s_bh1750.address
                            : CONFIG_INSTACARE_BH1750_ADDRESS);
    return 0;
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
