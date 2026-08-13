#include "sensor_manager.h"

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <app/util/attribute-storage.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <nvs_flash.h>
#include <platform/CHIPDeviceEvent.h>
#include <platform/CHIPDeviceLayer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

static const char *TAG = "matter_sensor";

using namespace esp_matter;
using namespace chip::app::Clusters;

namespace {

struct matter_endpoints_t {
    endpoint_t *temperature = nullptr;
    endpoint_t *humidity = nullptr;
    endpoint_t *illuminance = nullptr;
    endpoint_t *occupancy = nullptr;
    bool dht_active = false;
    bool illuminance_active = false;
    bool occupancy_active = false;
};

matter_endpoints_t s_endpoints;

uint16_t lux_to_matter(float lux)
{
    if (lux <= 0.0f) {
        return 0;
    }

    const double encoded = 10000.0 * std::log10(static_cast<double>(lux)) + 1.0;
    return static_cast<uint16_t>(std::clamp(std::lround(encoded), 1L, 65534L));
}

void update_attribute(uint16_t endpoint_id, uint32_t cluster_id,
                      uint32_t attribute_id, esp_matter_attr_val_t value,
                      const char *name)
{
    esp_err_t error = attribute::update(endpoint_id, cluster_id, attribute_id, &value);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "更新 %s 失败：%s", name, esp_err_to_name(error));
    }
}

void apply_sensor_reading(sensor_reading_t reading)
{
    if (s_endpoints.dht_active && reading.dht_valid) {
        const int32_t temperature = std::clamp<int32_t>(
            static_cast<int32_t>(std::lround(reading.temperature_c * 100.0f)),
            static_cast<int32_t>(-27315), static_cast<int32_t>(32767));
        auto value = esp_matter_nullable_int16(
            nullable<int16_t>(static_cast<int16_t>(temperature)));
        update_attribute(endpoint::get_id(s_endpoints.temperature),
                         TemperatureMeasurement::Id,
                         TemperatureMeasurement::Attributes::MeasuredValue::Id,
                         value, "温度");

        const int32_t humidity = std::clamp<int32_t>(
            static_cast<int32_t>(std::lround(reading.humidity_percent * 100.0f)),
            static_cast<int32_t>(0), static_cast<int32_t>(10000));
        value = esp_matter_nullable_uint16(
            nullable<uint16_t>(static_cast<uint16_t>(humidity)));
        update_attribute(endpoint::get_id(s_endpoints.humidity),
                         RelativeHumidityMeasurement::Id,
                         RelativeHumidityMeasurement::Attributes::MeasuredValue::Id,
                         value, "湿度");
    }

    if (s_endpoints.illuminance_active && reading.bh1750_valid) {
        auto value = esp_matter_nullable_uint16(
            nullable<uint16_t>(lux_to_matter(reading.illuminance_lux)));
        update_attribute(endpoint::get_id(s_endpoints.illuminance),
                         IlluminanceMeasurement::Id,
                         IlluminanceMeasurement::Attributes::MeasuredValue::Id,
                         value, "光照");
    }

    if (s_endpoints.occupancy_active) {
        auto occupancy = esp_matter_bitmap8(reading.occupied ? 0x01 : 0x00);
        update_attribute(endpoint::get_id(s_endpoints.occupancy),
                         OccupancySensing::Id,
                         OccupancySensing::Attributes::Occupancy::Id,
                         occupancy, "人体占用");
    }

    ESP_LOGI(TAG,
             "Matter 属性已更新：温度=%s 湿度=%s 光照=%s 占用=%s",
             !s_endpoints.dht_active ? "未启用" :
                 (reading.dht_valid ? "有效" : "无效"),
             !s_endpoints.dht_active ? "未启用" :
                 (reading.dht_valid ? "有效" : "无效"),
             !s_endpoints.illuminance_active ? "未启用" :
                 (reading.bh1750_valid ? "有效" : "无效"),
             !s_endpoints.occupancy_active ? "未启用" :
                 (reading.occupied ? "是" : "否"));
}

void sensor_reading_callback(const sensor_reading_t *reading, void *context)
{
    (void)context;
    if (reading == nullptr) {
        return;
    }

    const sensor_reading_t copy = *reading;
    chip::DeviceLayer::SystemLayer().ScheduleLambda([copy]() {
        apply_sensor_reading(copy);
    });
}

void occupancy_callback(bool occupied, void *context)
{
    (void)context;
    chip::DeviceLayer::SystemLayer().ScheduleLambda([occupied]() {
        if (!s_endpoints.occupancy_active) {
            return;
        }
        auto value = esp_matter_bitmap8(occupied ? 0x01 : 0x00);
        update_attribute(endpoint::get_id(s_endpoints.occupancy),
                         OccupancySensing::Id,
                         OccupancySensing::Attributes::Occupancy::Id,
                         value, "人体占用");
        ESP_LOGI(TAG, "Matter 占用实时更新：%s", occupied ? "有人" : "无人");
    });
}

esp_err_t attribute_update_callback(attribute::callback_type_t type,
                                    uint16_t endpoint_id,
                                    uint32_t cluster_id,
                                    uint32_t attribute_id,
                                    esp_matter_attr_val_t *value,
                                    void *private_data)
{
    (void)private_data;

    if (s_endpoints.occupancy != nullptr &&
        endpoint_id == endpoint::get_id(s_endpoints.occupancy) &&
        cluster_id == OccupancySensing::Id &&
        attribute_id ==
            OccupancySensing::Attributes::PIROccupiedToUnoccupiedDelay::Id) {
        if (value == nullptr || value->type != ESP_MATTER_VAL_TYPE_UINT16 ||
            value->val.u16 < SENSOR_OCCUPANCY_HOLD_MIN_SECONDS ||
            value->val.u16 > SENSOR_OCCUPANCY_HOLD_MAX_SECONDS) {
            ESP_LOGW(TAG, "拒绝无效占用保持时间；允许范围为 %u–%u 秒",
                     SENSOR_OCCUPANCY_HOLD_MIN_SECONDS,
                     SENSOR_OCCUPANCY_HOLD_MAX_SECONDS);
            return ESP_ERR_INVALID_ARG;
        }
        if (type == attribute::POST_UPDATE) {
            sensor_manager_set_occupancy_hold_seconds(value->val.u16);
            ESP_LOGI(TAG, "Matter 已写入 PIR 占用保持时间：%u 秒",
                     value->val.u16);
        }
    }
    return ESP_OK;
}

esp_err_t identification_callback(identification::callback_type_t type,
                                  uint16_t endpoint_id,
                                  uint8_t effect_id,
                                  uint8_t effect_variant,
                                  void *private_data)
{
    (void)private_data;
    ESP_LOGI(TAG, "Identify：endpoint=%u type=%u effect=%u variant=%u",
             endpoint_id, static_cast<unsigned>(type), effect_id, effect_variant);
    return ESP_OK;
}

void open_commissioning_window_if_needed()
{
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() != 0) {
        return;
    }

    auto &manager = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (manager.IsCommissioningWindowOpen()) {
        return;
    }

    CHIP_ERROR error = manager.OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds16(300),
        chip::CommissioningWindowAdvertisement::kDnssdOnly);
    if (error != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "打开配网窗口失败：%" CHIP_ERROR_FORMAT, error.Format());
    }
}

void matter_event_callback(const ChipDeviceEvent *event, intptr_t argument)
{
    (void)argument;
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Matter 配网完成");
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "Matter 配网失败：Fail-safe 超时");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Matter Fabric 已移除，重新开放配网窗口");
        open_commissioning_window_if_needed();
        break;
    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "Matter BLE 已停用并释放内存");
        break;
    default:
        break;
    }
}

bool configure_occupancy_endpoint(endpoint_t *occupancy)
{
    cluster_t *occupancy_cluster = cluster::get(occupancy,
                                                OccupancySensing::Id);
    if (occupancy_cluster == nullptr) {
        ESP_LOGE(TAG, "获取 Occupancy Sensing Cluster 失败");
        return false;
    }

    const uint16_t default_hold_seconds = static_cast<uint16_t>(
        (CONFIG_INSTACARE_OCCUPANCY_HOLD_MS + 999) / 1000);
    attribute_t *hold_attribute =
        cluster::occupancy_sensing::attribute::
            create_pir_occupied_to_unoccupied_delay(
                occupancy_cluster, default_hold_seconds);
    if (hold_attribute == nullptr ||
        attribute::add_bounds(
            hold_attribute,
            esp_matter_uint16(SENSOR_OCCUPANCY_HOLD_MIN_SECONDS),
            esp_matter_uint16(SENSOR_OCCUPANCY_HOLD_MAX_SECONDS)) != ESP_OK) {
        ESP_LOGE(TAG, "创建 PIR 占用保持时间属性失败");
        return false;
    }

    esp_matter_attr_val_t persisted_hold;
    if (attribute::get_val(hold_attribute, &persisted_hold) != ESP_OK ||
        persisted_hold.type != ESP_MATTER_VAL_TYPE_UINT16 ||
        sensor_manager_set_occupancy_hold_seconds(persisted_hold.val.u16) != 0) {
        ESP_LOGE(TAG, "加载 PIR 占用保持时间失败");
        return false;
    }

    return true;
}

bool create_capability_endpoint(node_t *node, sensor_capability_t capability,
                                bool enable_now)
{
    if (node == nullptr) {
        return false;
    }
    esp_err_t error = ESP_OK;
    switch (capability) {
    case SENSOR_CAPABILITY_DHT:
        if (s_endpoints.dht_active) {
            return true;
        }
        {
            endpoint::temperature_sensor::config_t temperature_config;
            endpoint::humidity_sensor::config_t humidity_config;
            s_endpoints.temperature = endpoint::temperature_sensor::create(
                node, &temperature_config, ENDPOINT_FLAG_NONE, nullptr);
            s_endpoints.humidity = endpoint::humidity_sensor::create(
                node, &humidity_config, ENDPOINT_FLAG_NONE, nullptr);
        }
        if (s_endpoints.temperature == nullptr ||
            s_endpoints.humidity == nullptr) {
            return false;
        }
        if (enable_now) {
            error = endpoint::enable(s_endpoints.temperature);
            if (error == ESP_OK) {
                error = endpoint::enable(s_endpoints.humidity);
            }
        }
        s_endpoints.dht_active = error == ESP_OK;
        if (s_endpoints.dht_active) {
            ESP_LOGI(TAG, "Matter Endpoint：温度=%u，湿度=%u",
                     endpoint::get_id(s_endpoints.temperature),
                     endpoint::get_id(s_endpoints.humidity));
        }
        break;
    case SENSOR_CAPABILITY_BH1750:
        if (s_endpoints.illuminance_active) {
            return true;
        }
        {
            endpoint::light_sensor::config_t light_config;
            s_endpoints.illuminance = endpoint::light_sensor::create(
                node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
        }
        if (s_endpoints.illuminance == nullptr) {
            return false;
        }
        if (enable_now) {
            error = endpoint::enable(s_endpoints.illuminance);
        }
        s_endpoints.illuminance_active = error == ESP_OK;
        if (s_endpoints.illuminance_active) {
            ESP_LOGI(TAG, "Matter Endpoint：光照=%u",
                     endpoint::get_id(s_endpoints.illuminance));
        }
        break;
    case SENSOR_CAPABILITY_PIR:
        if (s_endpoints.occupancy_active) {
            return true;
        }
        {
            endpoint::occupancy_sensor::config_t occupancy_config;
            occupancy_config.occupancy_sensing.occupancy_sensor_type =
                chip::to_underlying(
                    OccupancySensing::OccupancySensorTypeEnum::kPir);
            occupancy_config.occupancy_sensing.occupancy_sensor_type_bitmap =
                chip::to_underlying(
                    OccupancySensing::OccupancySensorTypeBitmap::kPir);
            occupancy_config.occupancy_sensing.features =
                cluster::occupancy_sensing::feature::passive_infrared::get_id();
            s_endpoints.occupancy = endpoint::occupancy_sensor::create(
                node, &occupancy_config, ENDPOINT_FLAG_NONE, nullptr);
        }
        if (s_endpoints.occupancy == nullptr ||
            !configure_occupancy_endpoint(s_endpoints.occupancy)) {
            return false;
        }
        if (enable_now) {
            error = endpoint::enable(s_endpoints.occupancy);
        }
        s_endpoints.occupancy_active = error == ESP_OK;
        if (s_endpoints.occupancy_active) {
            ESP_LOGI(TAG, "Matter Endpoint：人体占用=%u",
                     endpoint::get_id(s_endpoints.occupancy));
        }
        break;
    default:
        return false;
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "启用传感器 Endpoint 失败：%s", esp_err_to_name(error));
        return false;
    }
    ESP_LOGI(TAG, "Matter 能力%s：%s", enable_now ? "已动态启用" : "已准备",
             capability == SENSOR_CAPABILITY_DHT ? "温度/湿度" :
             capability == SENSOR_CAPABILITY_BH1750 ? "光照" : "人体占用");
    return true;
}

void capability_callback(sensor_capability_t capability, void *context)
{
    (void)context;
    chip::DeviceLayer::SystemLayer().ScheduleLambda([capability]() {
        if (!create_capability_endpoint(node::get(), capability, true)) {
            return;
        }
        sensor_reading_t reading;
        if (sensor_manager_get_latest(&reading)) {
            apply_sensor_reading(reading);
        }
    });
}

} // namespace

extern "C" void app_main(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    ESP_LOGI(TAG, "=== InstaCare Matter Sensor ===");

    sensor_capabilities_t capabilities;
    if (sensor_manager_configure(&capabilities) != 0) {
        ESP_LOGE(TAG, "设备传感器配置初始化失败");
        return;
    }
    node::config_t node_config;
    std::strncpy(node_config.root_node.basic_information.node_label,
                 "InstaCare Sensor",
                 sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_config.root_node.basic_information.node_label[
        sizeof(node_config.root_node.basic_information.node_label) - 1] = '\0';
    node_t *node = node::create(&node_config, attribute_update_callback,
                                identification_callback);
    if (node == nullptr) {
        ESP_LOGE(TAG, "创建 Matter 节点失败");
        return;
    }

    /* 启动前只创建已探测到的 Endpoint，避免控制器生成不存在的实体。 */
    if ((capabilities.dht &&
         !create_capability_endpoint(node, SENSOR_CAPABILITY_DHT, false)) ||
        (capabilities.bh1750 &&
         !create_capability_endpoint(node, SENSOR_CAPABILITY_BH1750, false)) ||
        (capabilities.pir &&
         !create_capability_endpoint(node, SENSOR_CAPABILITY_PIR, false))) {
        ESP_LOGE(TAG, "创建启动时传感器 Endpoint 失败");
        return;
    }

    error = esp_matter::start(matter_event_callback);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "启动 Matter 协议栈失败：%s", esp_err_to_name(error));
        return;
    }

    if (sensor_manager_start(sensor_reading_callback, occupancy_callback,
                             capability_callback, nullptr) != 0) {
        ESP_LOGE(TAG, "传感器管理器初始化失败");
        return;
    }

    ESP_LOGI(TAG, "设备已启动；未配网时请在 Home Assistant 中扫描启动日志里的 Matter 二维码");
}
