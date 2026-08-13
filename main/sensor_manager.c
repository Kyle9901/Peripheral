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
#include "nvs.h"

#include <stdio.h>
#include <stdatomic.h>

static const char *TAG = "sensors";

#define SENSOR_POLL_INTERVAL_MS 250
#define SENSOR_RESCAN_INTERVAL_MS 30000
#define PIR_CONFIRM_HIGH_SAMPLES 3
#define SENSOR_CAPS_NVS_NAMESPACE "sensor_caps"
#define SENSOR_CAPS_NVS_PIR_KEY "pir_seen"
#define DEFAULT_OCCUPANCY_HOLD_SECONDS \
    ((CONFIG_INSTACARE_OCCUPANCY_HOLD_MS + 999) / 1000)

static bh1750_t s_bh1750;
static bool s_configured;
static bool s_dht_ready;
static bool s_bh1750_ready;
static bool s_pir_confirmed;
static SemaphoreHandle_t s_reading_mutex;
static sensor_reading_t s_latest;
static bool s_has_reading;
static sensor_reading_callback_t s_reading_callback;
static sensor_occupancy_callback_t s_occupancy_callback;
static sensor_capability_callback_t s_capability_callback;
static void *s_callback_context;
static atomic_uint_fast16_t s_occupancy_hold_seconds =
    DEFAULT_OCCUPANCY_HOLD_SECONDS;

#if CONFIG_INSTACARE_DHT_MODEL_DHT11
#define INSTACARE_DHT_MODEL DHT_SENSOR_MODEL_DHT11
#define INSTACARE_DHT_MODEL_NAME "DHT11"
#else
#define INSTACARE_DHT_MODEL DHT_SENSOR_MODEL_DHT22
#define INSTACARE_DHT_MODEL_NAME "DHT22"
#endif

static esp_err_t init_pir_gpio(void)
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

static bool load_pir_confirmed(void)
{
    nvs_handle_t handle;
    if (nvs_open(SENSOR_CAPS_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t value = 0;
    esp_err_t error = nvs_get_u8(handle, SENSOR_CAPS_NVS_PIR_KEY, &value);
    nvs_close(handle);
    return error == ESP_OK && value == 1;
}

static esp_err_t persist_pir_confirmed(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(SENSOR_CAPS_NVS_NAMESPACE, NVS_READWRITE,
                               &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_u8(handle, SENSOR_CAPS_NVS_PIR_KEY, 1);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

static bool probe_dht(unsigned attempts)
{
    dht_sensor_reading_t reading;
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        esp_err_t error = dht_sensor_read(&reading);
        if (error == ESP_OK) {
            return true;
        }
        if (attempt + 1 < attempts) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    return false;
}

static bool probe_bh1750(void)
{
    esp_err_t error = bh1750_init(&s_bh1750,
                                  (gpio_num_t)CONFIG_INSTACARE_BH1750_SDA_GPIO,
                                  (gpio_num_t)CONFIG_INSTACARE_BH1750_SCL_GPIO,
                                  CONFIG_INSTACARE_BH1750_ADDRESS);
    if (error != ESP_OK) {
        ESP_LOGI(TAG, "未探测到 BH1750：%s", esp_err_to_name(error));
        return false;
    }
    return true;
}

static bool try_enable_bh1750(void)
{
    if (!probe_bh1750()) {
        return false;
    }
    s_bh1750_ready = true;
    return true;
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
        .motion = s_pir_confirmed &&
            gpio_get_level((gpio_num_t)CONFIG_INSTACARE_PIR_GPIO) != 0,
        .occupied = s_pir_confirmed && occupied,
        .sampled_at_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
    };

    if (s_dht_ready) {
        dht_sensor_reading_t dht_reading;
        esp_err_t error = dht_sensor_read(&dht_reading);
        if (error == ESP_OK) {
            reading.temperature_c = dht_reading.temperature_c;
            reading.humidity_percent = dht_reading.humidity_percent;
            reading.dht_valid = true;
        } else {
            ESP_LOGW(TAG, "%s 读取失败：%s", INSTACARE_DHT_MODEL_NAME,
                     esp_err_to_name(error));
        }
    }

    if (s_bh1750_ready) {
        esp_err_t error = bh1750_read_lux(&s_bh1750,
                                         &reading.illuminance_lux);
        reading.bh1750_valid = error == ESP_OK;
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "BH1750 读取失败：%s", esp_err_to_name(error));
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
             "温度=%s%s 湿度=%s%s 光照=%s%s PIR=%s 电平=%s Matter占用=%s",
             temperature, reading.dht_valid ? " C" : "",
             humidity, reading.dht_valid ? " %" : "",
             illuminance, reading.bh1750_valid ? " lux" : "",
             s_pir_confirmed ? "已识别" : "未识别",
             reading.motion ? "高" : "低",
             reading.occupied ? "是" : "否");
    if (s_reading_callback != NULL) {
        s_reading_callback(&reading, s_callback_context);
    }
}

static void rescan_unavailable_sensors(void)
{
    if (!s_dht_ready && probe_dht(1)) {
        s_dht_ready = true;
        ESP_LOGI(TAG, "运行中识别到 %s", INSTACARE_DHT_MODEL_NAME);
        if (s_capability_callback != NULL) {
            s_capability_callback(SENSOR_CAPABILITY_DHT, s_callback_context);
        }
    }
    if (!s_bh1750_ready && try_enable_bh1750()) {
        ESP_LOGI(TAG, "运行中识别到 BH1750（地址 0x%02x）", s_bh1750.address);
        if (s_capability_callback != NULL) {
            s_capability_callback(SENSOR_CAPABILITY_BH1750, s_callback_context);
        }
    }
}

static void sensor_task(void *argument)
{
    (void)argument;
    TickType_t now = xTaskGetTickCount();
    TickType_t next_report = now;
    TickType_t next_rescan = now + pdMS_TO_TICKS(SENSOR_RESCAN_INTERVAL_MS);
    TickType_t last_motion = 0;
    bool has_detected_motion = false;
    bool last_occupied = false;
    unsigned pir_high_samples = 0;

    while (true) {
        now = xTaskGetTickCount();
        bool pir_high = gpio_get_level(
            (gpio_num_t)CONFIG_INSTACARE_PIR_GPIO) != 0;
        if (!s_pir_confirmed) {
            pir_high_samples = pir_high ? pir_high_samples + 1 : 0;
            if (pir_high_samples >= PIR_CONFIRM_HIGH_SAMPLES) {
                s_pir_confirmed = true;
                esp_err_t error = persist_pir_confirmed();
                if (error != ESP_OK) {
                    ESP_LOGW(TAG, "保存 PIR 识别状态失败：%s",
                             esp_err_to_name(error));
                }
                ESP_LOGI(TAG, "GPIO%d 连续输出高电平，已识别 HC-SR501",
                         CONFIG_INSTACARE_PIR_GPIO);
                if (s_capability_callback != NULL) {
                    s_capability_callback(SENSOR_CAPABILITY_PIR,
                                          s_callback_context);
                }
            }
        }
        if (s_pir_confirmed && pir_high) {
            last_motion = now;
            has_detected_motion = true;
        }

        uint16_t hold_seconds = sensor_manager_get_occupancy_hold_seconds();
        TickType_t occupancy_hold = pdMS_TO_TICKS(
            (uint32_t)hold_seconds * 1000U);
        bool occupied = s_pir_confirmed && has_detected_motion &&
            (TickType_t)(now - last_motion) <= occupancy_hold;
        if (occupied != last_occupied) {
            last_occupied = occupied;
            if (s_occupancy_callback != NULL) {
                s_occupancy_callback(occupied, s_callback_context);
            }
        }

        if ((int32_t)(now - next_report) >= 0) {
            sample_sensors(occupied);
            next_report = now + pdMS_TO_TICKS(
                CONFIG_INSTACARE_SENSOR_REPORT_INTERVAL_MS);
        }
        if ((int32_t)(now - next_rescan) >= 0) {
            rescan_unavailable_sensors();
            next_rescan = now + pdMS_TO_TICKS(SENSOR_RESCAN_INTERVAL_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS));
    }
}

int sensor_manager_configure(sensor_capabilities_t *capabilities)
{
    if (capabilities == NULL) {
        return -1;
    }
    *capabilities = (sensor_capabilities_t){0};
    s_dht_ready = false;
    s_bh1750_ready = false;
    s_pir_confirmed = false;

    esp_err_t error = init_pir_gpio();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "PIR GPIO 初始化失败：%s", esp_err_to_name(error));
        return -1;
    }
    error = dht_sensor_init((gpio_num_t)CONFIG_INSTACARE_DHT_GPIO,
                            INSTACARE_DHT_MODEL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "%s GPIO 初始化失败：%s", INSTACARE_DHT_MODEL_NAME,
                 esp_err_to_name(error));
        return -1;
    }

    /* DHT 上电稳定后进行两次确认，避免一次时序错误造成误判。 */
    vTaskDelay(pdMS_TO_TICKS(1000));
    s_dht_ready = probe_dht(2);
    s_bh1750_ready = try_enable_bh1750();
    s_pir_confirmed = load_pir_confirmed();

    capabilities->dht = s_dht_ready;
    capabilities->bh1750 = s_bh1750_ready;
    capabilities->pir = s_pir_confirmed;
    s_configured = true;
    ESP_LOGI(TAG, "自动探测结果：温湿度=%s 光照=%s 人体红外=%s",
             capabilities->dht ? "有" : "无",
             capabilities->bh1750 ? "有" : "无",
             capabilities->pir ? "已登记" : "等待首次高电平");
    return 0;
}

int sensor_manager_start(sensor_reading_callback_t reading_callback,
                         sensor_occupancy_callback_t occupancy_callback,
                         sensor_capability_callback_t capability_callback,
                         void *context)
{
    if (!s_configured) {
        return -1;
    }
    s_reading_callback = reading_callback;
    s_occupancy_callback = occupancy_callback;
    s_capability_callback = capability_callback;
    s_callback_context = context;
    s_reading_mutex = xSemaphoreCreateMutex();
    if (s_reading_mutex == NULL) {
        return -1;
    }
    if (xTaskCreate(sensor_task, "instacare_sensors", 4096, NULL, 4, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "无法创建传感器任务");
        return -1;
    }
    ESP_LOGI(TAG, "传感器管理已启动：采样=%dms，重扫=%dms，占用保持=%u秒",
             CONFIG_INSTACARE_SENSOR_REPORT_INTERVAL_MS,
             SENSOR_RESCAN_INTERVAL_MS,
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
