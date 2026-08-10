#include "dev_info.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "dev_info";

/* ========== 可配置的设备信息 ========== */
#define DEV_MANUFACTURER    "Example Devices"
#define DEV_MODEL           "AC-100"
#define DEV_SERIAL_NUMBER   "AC100-000042"
#define DEV_FIRMWARE_VERSION "2.3.1"
#define DEV_SPEC_VERSION    "instacare.provisioning/1.0"
#define DEV_CAP_SPEC_VERSIONS "[\"instacare.device/1.0\"]"

/* ========== NVS 存储 ========== */
#define NVS_NAMESPACE       "instacare"
#define NVS_KEY_DEVICE_ID   "device_id"
#define NVS_KEY_CONFIG_REV  "config_rev"

static uint8_t  s_device_id[16];
static char     s_device_id_hex[33];
static uint64_t s_config_revision = 0;

/* ========== JSON 缓冲区 ========== */
static char s_device_info_json[512];

int dev_info_init(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    // 读取 device_id
    size_t len = sizeof(s_device_id);
    err = nvs_get_blob(handle, NVS_KEY_DEVICE_ID, s_device_id, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // 首次启动：生成随机 device_id
        ESP_LOGW(TAG, "No device_id in NVS, generating new one...");
        for (int i = 0; i < 16; i++) {
            s_device_id[i] = (uint8_t)(esp_random() & 0xFF);
        }
        err = nvs_set_blob(handle, NVS_KEY_DEVICE_ID, s_device_id, 16);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to store device_id: %s", esp_err_to_name(err));
            nvs_close(handle);
            return -1;
        }
        nvs_commit(handle);
        ESP_LOGI(TAG, "New device_id generated and stored");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob device_id failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return -1;
    }

    // 读取 config_revision
    err = nvs_get_u64(handle, NVS_KEY_CONFIG_REV, &s_config_revision);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_config_revision = 0;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_u64 failed: %s", esp_err_to_name(err));
        s_config_revision = 0;
    }

    nvs_close(handle);

    // 生成 hex 字符串
    for (int i = 0; i < 16; i++) {
        snprintf(&s_device_id_hex[i * 2], 3, "%02x", s_device_id[i]);
    }
    s_device_id_hex[32] = '\0';

    ESP_LOGI(TAG, "Device ID: %s, config_revision: %llu",
             s_device_id_hex, s_config_revision);
    return 0;
}

const char *dev_info_get_json(void)
{
    snprintf(s_device_info_json, sizeof(s_device_info_json),
        "{"
        "\"spec\":\"%s\","
        "\"device_id\":\"%s\","
        "\"manufacturer\":\"%s\","
        "\"model\":\"%s\","
        "\"serial_number\":\"%s\","
        "\"firmware_version\":\"%s\","
        "\"capability_spec_versions\":%s,"
        "\"config_revision\":%llu"
        "}",
        DEV_SPEC_VERSION,
        s_device_id_hex,
        DEV_MANUFACTURER,
        DEV_MODEL,
        DEV_SERIAL_NUMBER,
        DEV_FIRMWARE_VERSION,
        DEV_CAP_SPEC_VERSIONS,
        s_config_revision
    );
    return s_device_info_json;
}

const char *dev_info_get_device_id_hex(void)
{
    return s_device_id_hex;
}

const uint8_t *dev_info_get_device_id_raw(void)
{
    return s_device_id;
}

uint64_t dev_info_get_config_revision(void)
{
    return s_config_revision;
}

void dev_info_set_config_revision(uint64_t revision)
{
    s_config_revision = revision;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_set_u64(handle, NVS_KEY_CONFIG_REV, revision);
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "config_revision updated to %llu", revision);
}