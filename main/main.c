/**
 * main.c — InstaCare Peripheral 入口
 *
 * 启动流程：
 *   1. 初始化 NVS（存储 device_id、config_revision 等）
 *   2. 初始化设备信息（加载或生成 device_id）
 *   3. 初始化状态机
 *   4. 启动 Wi-Fi/TCP worker 和 BLE Provisioning Service
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "ble_prov.h"
#include "dev_info.h"
#include "status.h"
#include "network_config.h"
#include "connectivity.h"
#include "sensor_manager.h"

static const char *TAG = "main";

void app_main(void)
{
    // ================================================================
    // 1. 初始化 NVS
    // ================================================================
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "=== InstaCare Peripheral ===");
    ESP_LOGI(TAG, "Spec: instacare.provisioning/1.0");

    // ================================================================
    // 2. 初始化设备信息
    // ================================================================
    if (dev_info_init() != 0) {
        ESP_LOGE(TAG, "Failed to initialize device info");
        return;
    }
    ESP_LOGI(TAG, "Device ID: %s", dev_info_get_device_id_hex());

    // ================================================================
    // 3. 初始化状态机
    // ================================================================
    status_init();

    // 启动网络连接前先加载已保存的配置。
    if (network_config_init() != 0) {
        ESP_LOGE(TAG, "Failed to initialize network configuration");
        return;
    }

    if (connectivity_init() != 0) {
        ESP_LOGE(TAG, "Failed to initialize Wi-Fi/TCP connectivity");
        return;
    }

    // ================================================================
    // 4. 启动 BLE Provisioning
    // ================================================================
    if (ble_prov_init() != 0) {
        ESP_LOGE(TAG, "Failed to initialize BLE provisioning");
        return;
    }

    ESP_LOGI(TAG, "Peripheral ready, advertising...");

    // 启动传感器采样；单个传感器缺失不会阻止网络和 BLE 工作。
    if (sensor_manager_init() != 0) {
        ESP_LOGE(TAG, "传感器管理器初始化失败");
    }

    // 重启后恢复已接受的配置，无需再次通过 BLE 写入。
    network_config_t saved_config;
    if (network_config_get(&saved_config)) {
        status_set_provisioning_id(saved_config.provisioning_id);
        status_set_state(STATUS_CONFIG_RECEIVED);
        ble_prov_notify_status();
        connectivity_apply_config(&saved_config);
    }

    // ================================================================
    // 主循环
    // ================================================================
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        // 定期打印状态
        ESP_LOGD(TAG, "State: %d, connected: %s, uptime: %lu ms",
                 status_get_state(),
                 "see BLE events",
                 (unsigned long)status_get_uptime_ms());
    }
}
