/**
 * ble_prov.c — InstaCare BLE Provisioning Service
 *
 * 实现协议定义的：
 * - BLE 5 Extended Advertising（128-bit Service UUID + Service Data）
 * - GATT Provisioning Service（4 个特征）
 * - 长读写 / Prepare Write / Execute Write
 * - Status Notify
 */

#include "ble_prov.h"
#include "dev_info.h"
#include "cert_chain.h"
#include "status.h"
#include "network_config.h"
#include "connectivity.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_mac.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "ble_prov";

/* ================================================================
 * UUID 定义（协议 3.1 节）
 * ================================================================ */

// Provisioning Service: 7f510000-6e7a-4c4f-9d2b-2f6f5a01c001
#define PROV_SERVICE_UUID \
    {0x01, 0xC0, 0x01, 0x5A, 0x6F, 0x2F, 0x2B, 0x9D, \
     0x4F, 0x4C, 0x7A, 0x6E, 0x00, 0x00, 0x51, 0x7F}

// DeviceInfo:       7f510001-...
#define DEV_INFO_UUID \
    {0x01, 0xC0, 0x01, 0x5A, 0x6F, 0x2F, 0x2B, 0x9D, \
     0x4F, 0x4C, 0x7A, 0x6E, 0x01, 0x00, 0x51, 0x7F}

// CertificateChain: 7f510002-...
#define CERT_CHAIN_UUID \
    {0x02, 0xC0, 0x01, 0x5A, 0x6F, 0x2F, 0x2B, 0x9D, \
     0x4F, 0x4C, 0x7A, 0x6E, 0x02, 0x00, 0x51, 0x7F}

// NetworkConfig:    7f510003-...
#define NET_CONFIG_UUID \
    {0x03, 0xC0, 0x01, 0x5A, 0x6F, 0x2F, 0x2B, 0x9D, \
     0x4F, 0x4C, 0x7A, 0x6E, 0x03, 0x00, 0x51, 0x7F}

// Status:           7f510004-...
#define STATUS_UUID \
    {0x04, 0xC0, 0x01, 0x5A, 0x6F, 0x2F, 0x2B, 0x9D, \
     0x4F, 0x4C, 0x7A, 0x6E, 0x04, 0x00, 0x51, 0x7F}

/* ================================================================
 * Magic: "ICAR" (协议 3.2 节)
 * ================================================================ */
#define MAGIC_ICAR  {0x49, 0x43, 0x41, 0x52}

/* ================================================================
 * GATT 句柄数量
 * Service + 4 characteristics + 1 CCCD + 1 extra = 12 handles
 * ================================================================ */
#define NUM_HANDLES 12

/* ================================================================
 * 全局状态
 * ================================================================ */
static esp_gatt_if_t    s_gatts_if      = ESP_GATT_IF_NONE;
static uint16_t         s_conn_id       = 0;
static bool             s_connected     = false;
static bool             s_cccd_enabled  = false;

// GATT 属性句柄
static uint16_t s_handle_dev_info      = 0;  // value handle
static uint16_t s_handle_cert_chain    = 0;
static uint16_t s_handle_net_config    = 0;
static uint16_t s_handle_status        = 0;
static uint16_t s_handle_status_cccd   = 0;
// 声明句柄 (iOS nRF Connect 有时用声明句柄读)
static uint16_t s_handle_dev_info_decl = 0;
static uint16_t s_handle_cert_chain_decl = 0;
static uint16_t s_handle_net_config_decl = 0;
static uint16_t s_handle_status_decl   = 0;

// 蓝牙地址(预留)
// static uint8_t s_ble_addr[6];

// Prepare Write 缓冲区
#define WRITE_BUF_MAX 4096
static uint8_t *s_write_buf     = NULL;
static size_t   s_write_offset  = 0;

/* ================================================================
 * 前向声明
 * ================================================================ */
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param);
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param);
static void start_advertising(void);
static void handle_read_event(esp_gatt_if_t gatts_if,
                              esp_ble_gatts_cb_param_t *param);
static void handle_write_event(esp_gatt_if_t gatts_if,
                               esp_ble_gatts_cb_param_t *param);
static void handle_exec_write_event(esp_gatt_if_t gatts_if,
                                    esp_ble_gatts_cb_param_t *param);
static void process_network_config(void);

/* ================================================================
 * 发送 GATT 响应
 * ================================================================ */
static void send_read_response(esp_gatt_if_t gatts_if, uint16_t conn_id,
                               uint32_t trans_id, uint16_t handle,
                               const uint8_t *data, size_t data_len,
                               uint16_t offset)
{
    esp_gatt_rsp_t rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.attr_value.handle = handle;
    rsp.attr_value.offset = offset;

    size_t remaining = data_len - offset;
    if (remaining > 0) {
        // MTU-1 留给 ATT 头部，保守起见取较小的值
        size_t mtu = 512; // BLE 5 可协商更大的 MTU
        size_t chunk = (remaining > mtu - 1) ? (mtu - 1) : remaining;
        rsp.attr_value.len = chunk;
        memcpy(rsp.attr_value.value, data + offset, chunk);
    } else {
        rsp.attr_value.len = 0;
    }

    esp_err_t err = esp_ble_gatts_send_response(gatts_if, conn_id, trans_id,
                                                 ESP_GATT_OK, &rsp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send_response failed: %s", esp_err_to_name(err));
    }
}

/* ================================================================
 * 初始化
 * ================================================================ */
int ble_prov_init(void)
{
    esp_err_t ret;

    // 分配 Prepare Write 缓冲区
    s_write_buf = (uint8_t *)malloc(WRITE_BUF_MAX);
    if (!s_write_buf) {
        ESP_LOGE(TAG, "Failed to allocate write buffer");
        return -1;
    }

    // 释放 Classic BT 内存（如果未使用）
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    // 初始化 BT 控制器
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bt_controller_init failed: %s", esp_err_to_name(ret));
        return -1;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bt_controller_enable failed: %s", esp_err_to_name(ret));
        return -1;
    }

    // 初始化 Bluedroid
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid_init failed: %s", esp_err_to_name(ret));
        return -1;
    }
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid_enable failed: %s", esp_err_to_name(ret));
        return -1;
    }

    // 注册 GAP/GATT 回调
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gap_register_callback failed: %s", esp_err_to_name(ret));
        return -1;
    }
    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gatts_register_callback failed: %s", esp_err_to_name(ret));
        return -1;
    }

    // 注册 GATT 应用
    ret = esp_ble_gatts_app_register(0); // app_id = 0
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gatts_app_register failed: %s", esp_err_to_name(ret));
        return -1;
    }

    // 设置 BLE 设备名
    esp_ble_gap_set_device_name("InstaCare-Peri");

    ESP_LOGI(TAG, "BLE provisioning initialized");
    return 0;
}

/* ================================================================
 * 创建 GATT 服务
 * ================================================================ */
static void create_gatt_service(esp_gatt_if_t gatts_if)
{
    uint8_t prov_uuid[16]  = PROV_SERVICE_UUID;

    esp_gatt_srvc_id_t service_id = {
        .is_primary = true,
        .id = {
            .inst_id = 0,
            .uuid = {
                .len = ESP_UUID_LEN_128,
            },
        },
    };
    memcpy(service_id.id.uuid.uuid.uuid128, prov_uuid, 16);

    esp_err_t ret = esp_ble_gatts_create_service(gatts_if, &service_id, NUM_HANDLES);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create_service failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Creating GATT service...");
}

/* ================================================================
 * 添加特征
 * ================================================================ */
static void add_characteristics(esp_gatt_if_t gatts_if, uint16_t svc_handle)
{
    esp_err_t ret;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t prop;
    esp_attr_value_t attr_val;
    esp_attr_control_t control;

    ESP_LOGI(TAG, "Adding characteristics to service handle %d", svc_handle);

    /* ---------- DeviceInfo (Read) ---------- */
    char_uuid.len = ESP_UUID_LEN_128;
    uint8_t dev_uuid[16] = DEV_INFO_UUID;
    memcpy(char_uuid.uuid.uuid128, dev_uuid, 16);
    perm = ESP_GATT_PERM_READ;
    prop = ESP_GATT_CHAR_PROP_BIT_READ;
    attr_val.attr_max_len = 512;
    attr_val.attr_len     = 0;
    attr_val.attr_value   = NULL;
    control.auto_rsp = ESP_GATT_RSP_BY_APP;

    ret = esp_ble_gatts_add_char(svc_handle, &char_uuid, perm, prop,
                                  &attr_val, &control);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add_char DeviceInfo failed: %s", esp_err_to_name(ret));
    }

    /* ---------- CertificateChain (Read) ---------- */
    uint8_t cert_uuid[16] = CERT_CHAIN_UUID;
    memcpy(char_uuid.uuid.uuid128, cert_uuid, 16);
    perm = ESP_GATT_PERM_READ;
    prop = ESP_GATT_CHAR_PROP_BIT_READ;
    attr_val.attr_max_len = 65535; // 协议限制最大 64 KiB
    attr_val.attr_len     = 0;
    attr_val.attr_value   = NULL;
    control.auto_rsp = ESP_GATT_RSP_BY_APP;

    ret = esp_ble_gatts_add_char(svc_handle, &char_uuid, perm, prop,
                                  &attr_val, &control);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add_char CertChain failed: %s", esp_err_to_name(ret));
    }

    /* ---------- NetworkConfig (Write) ---------- */
    uint8_t net_uuid[16] = NET_CONFIG_UUID;
    memcpy(char_uuid.uuid.uuid128, net_uuid, 16);
    perm = ESP_GATT_PERM_WRITE;
    prop = ESP_GATT_CHAR_PROP_BIT_WRITE;
    attr_val.attr_max_len = 4096;
    attr_val.attr_len     = 0;
    attr_val.attr_value   = NULL;

    ret = esp_ble_gatts_add_char(svc_handle, &char_uuid, perm, prop,
                                  &attr_val, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add_char NetworkConfig failed: %s", esp_err_to_name(ret));
    }

    /* ---------- Status (Read + Notify) ---------- */
    uint8_t stat_uuid[16] = STATUS_UUID;
    memcpy(char_uuid.uuid.uuid128, stat_uuid, 16);
    perm = ESP_GATT_PERM_READ;
    prop = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
    attr_val.attr_max_len = 512;
    attr_val.attr_len     = 0;
    attr_val.attr_value   = NULL;
    control.auto_rsp = ESP_GATT_RSP_BY_APP;

    ret = esp_ble_gatts_add_char(svc_handle, &char_uuid, perm, prop,
                                  &attr_val, &control);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add_char Status failed: %s", esp_err_to_name(ret));
    }

    /* ---------- Status CCCD ---------- */
    // CCCD 由 BLE 栈自动添加，我们只需在 start_service 后记录句柄
    // 句柄号 = 上一个特征句柄 + 2（特征声明 + 特征值 + CCCD）

    /* ---------- 启动服务 ---------- */
    ret = esp_ble_gatts_start_service(svc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start_service failed: %s", esp_err_to_name(ret));
    }
}

/* ================================================================
 * 广告数据构建
 *
 * 为兼容 iPhone，使用 Legacy Advertising（非 Extended）：
 *   - Advertising packet:  Flags + 128-bit Service UUID = 21 bytes
 *   - Scan response:       Manufacturer Specific Data (magic + device_id) = 24 bytes
 *   都在 31 字节 Legacy 限制内。
 * ================================================================ */
static void start_advertising(void)
{
    uint8_t adv_data[32];
    uint8_t scan_rsp[32];
    uint8_t adv_len = 0, scan_len = 0;

    uint8_t prov_uuid[16] = PROV_SERVICE_UUID;
    uint8_t magic[4]      = MAGIC_ICAR;
    const uint8_t *dev_id = dev_info_get_device_id_raw();

    /*
     * Advertising packet:
     *   AD 1: Flags — LE General Discoverable + BR/EDR Not Supported
     *   AD 2: Complete List of 128-bit Service UUIDs
     */
    adv_data[adv_len++] = 0x02;       // length (type + data)
    adv_data[adv_len++] = 0x01;       // Flags
    adv_data[adv_len++] = 0x06;       // LE General Discoverable, BR/EDR not supported
    adv_data[adv_len++] = 17;         // length (type + UUID)
    adv_data[adv_len++] = 0x07;       // Complete List of 128-bit Service UUIDs
    memcpy(&adv_data[adv_len], prov_uuid, 16);
    adv_len += 16;                    // total = 21 bytes

    /*
     * Scan response:
     *   Manufacturer Specific Data (0xFF):
     *     Company ID: 0xFFFF (测试用)
     *     Magic (4) + device_id (16) = 20 bytes payload
     *   Total: 1(len) + 1(type) + 2(company) + 20(payload) = 24 bytes
     */
    scan_rsp[scan_len++] = 23;        // length = type(1) + company(2) + data(20)
    scan_rsp[scan_len++] = 0xFF;      // Manufacturer Specific Data
    scan_rsp[scan_len++] = 0xFF;      // Company ID low
    scan_rsp[scan_len++] = 0xFF;      // Company ID high
    memcpy(&scan_rsp[scan_len], magic, 4);
    scan_len += 4;
    memcpy(&scan_rsp[scan_len], dev_id, 16);
    scan_len += 16;                   // total = 24 bytes

    ESP_LOGI(TAG, "Advertising: %d bytes, ScanRsp: %d bytes", adv_len, scan_len);

    // 设置广播数据
    esp_err_t ret = esp_ble_gap_config_adv_data_raw(adv_data, adv_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "config_adv_data failed: %s", esp_err_to_name(ret));
        return;
    }

    // 设置扫描响应数据
    ret = esp_ble_gap_config_scan_rsp_data_raw(scan_rsp, scan_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "config_scan_rsp_data failed: %s", esp_err_to_name(ret));
        return;
    }

    // 启动 Legacy 广播
    esp_ble_adv_params_t adv_params = {
        .adv_int_min        = 0x20,   // 20 ms
        .adv_int_max        = 0x40,   // 40 ms
        .adv_type           = ADV_TYPE_IND,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .channel_map        = ADV_CHNL_ALL,
        .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    ret = esp_ble_gap_start_advertising(&adv_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start_advertising failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Legacy advertising started (iPhone compatible)");
}

/* ================================================================
 * Status Notify
 * ================================================================ */
void ble_prov_notify_status(void)
{
    if (!s_connected || !s_cccd_enabled || s_handle_status == 0) {
        return;
    }

    const char *json = status_get_json();
    esp_err_t ret = esp_ble_gatts_send_indicate(
        s_gatts_if,
        s_conn_id,
        s_handle_status,
        strlen(json),
        (uint8_t *)json,
        false  // false = notification (无需确认)
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "notify_status failed: %s", esp_err_to_name(ret));
    }
}

/* ================================================================
 * GATT 事件处理
 * ================================================================ */
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    switch (event) {

    case ESP_GATTS_REG_EVT:
        ESP_LOGI(TAG, "GATT registered, app_id=%d", param->reg.app_id);
        s_gatts_if = gatts_if;
        create_gatt_service(gatts_if);
        break;

    case ESP_GATTS_CREATE_EVT:
        ESP_LOGI(TAG, "Service created, handle=%d", param->create.service_handle);
        add_characteristics(gatts_if, param->create.service_handle);
        break;

    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG, "Service started, handle=%d", param->start.service_handle);
        start_advertising();
        break;

    case ESP_GATTS_ADD_CHAR_EVT: {
        uint16_t char_handle = param->add_char.attr_handle;
        // 根据添加顺序分配句柄
        // 顺序: DeviceInfo, CertificateChain, NetworkConfig, Status
        if (s_handle_dev_info == 0) {
            s_handle_dev_info_decl = char_handle;
            s_handle_dev_info = char_handle + 1; // +1 跳过声明句柄
            ESP_LOGI(TAG, "DeviceInfo decl=%d val=%d", s_handle_dev_info_decl, s_handle_dev_info);
        } else if (s_handle_cert_chain == 0) {
            s_handle_cert_chain_decl = char_handle;
            s_handle_cert_chain = char_handle + 1;
            ESP_LOGI(TAG, "CertificateChain decl=%d val=%d", s_handle_cert_chain_decl, s_handle_cert_chain);
        } else if (s_handle_net_config == 0) {
            s_handle_net_config_decl = char_handle;
            s_handle_net_config = char_handle + 1;
            ESP_LOGI(TAG, "NetworkConfig decl=%d val=%d", s_handle_net_config_decl, s_handle_net_config);
        } else if (s_handle_status == 0) {
            s_handle_status_decl = char_handle;
            s_handle_status = char_handle + 1;
            s_handle_status_cccd = char_handle + 2; // CCCD 在特征值之后
            ESP_LOGI(TAG, "Status decl=%d val=%d CCCD=%d",
                     s_handle_status_decl, s_handle_status, s_handle_status_cccd);
        }
        break;
    }

    case ESP_GATTS_CONNECT_EVT:
        s_connected = true;
        s_conn_id = param->connect.conn_id;
        ESP_LOGI(TAG, "Client connected, conn_id=%d", s_conn_id);
        // 更新 BLE 连接参数（iOS 兼容：min >= 15ms）
        esp_ble_conn_update_params_t conn_params = {0};
        memcpy(conn_params.bda, param->connect.remote_bda, 6);
        conn_params.latency = 0;
        conn_params.max_int = 0x30;  // 60ms
        conn_params.min_int = 0x18;  // 30ms (iOS 要求 >= 15ms)
        conn_params.timeout = 500;   // 500ms
        esp_ble_gap_update_conn_params(&conn_params);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_connected = false;
        s_cccd_enabled = false;
        s_conn_id = 0;
        s_write_offset = 0;
        ESP_LOGI(TAG, "Client disconnected, reason=0x%x",
                 param->disconnect.reason);
        // 重新开始广播
        start_advertising();
        break;

    case ESP_GATTS_READ_EVT:
        handle_read_event(gatts_if, param);
        break;

    case ESP_GATTS_WRITE_EVT:
        handle_write_event(gatts_if, param);
        break;

    case ESP_GATTS_EXEC_WRITE_EVT:
        handle_exec_write_event(gatts_if, param);
        break;

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(TAG, "MTU updated: %d", param->mtu.mtu);
        break;

    default:
        break;
    }
}

/* ================================================================
 * 读事件处理
 * ================================================================ */
static void handle_read_event(esp_gatt_if_t gatts_if,
                              esp_ble_gatts_cb_param_t *param)
{
    uint16_t handle    = param->read.handle;
    uint16_t conn_id   = param->read.conn_id;
    uint32_t trans_id  = param->read.trans_id;
    uint16_t offset    = param->read.offset;
    bool     is_long   = param->read.is_long;

    ESP_LOGD(TAG, "Read request: handle=%d, offset=%d, is_long=%d",
             handle, offset, is_long);

    if (handle == s_handle_dev_info || handle == s_handle_dev_info_decl) {
        const char *json = dev_info_get_json();
        send_read_response(gatts_if, conn_id, trans_id, handle,
                           (const uint8_t *)json, strlen(json), offset);

    } else if (handle == s_handle_cert_chain || handle == s_handle_cert_chain_decl) {
        size_t chain_len = 0;
        const uint8_t *chain_data = cert_chain_get_data(&chain_len);
        send_read_response(gatts_if, conn_id, trans_id, handle,
                           chain_data, chain_len, offset);

    } else if (handle == s_handle_status || handle == s_handle_status_decl) {
        const char *json = status_get_json();
        send_read_response(gatts_if, conn_id, trans_id, handle,
                           (const uint8_t *)json, strlen(json), offset);

    } else {
        ESP_LOGW(TAG, "Read on unknown handle %d", handle);
        esp_ble_gatts_send_response(gatts_if, conn_id, trans_id,
                                     ESP_GATT_READ_NOT_PERMIT, NULL);
    }
}

/* ================================================================
 * 写事件处理
 * ================================================================ */
static void handle_write_event(esp_gatt_if_t gatts_if,
                               esp_ble_gatts_cb_param_t *param)
{
    uint16_t handle = param->write.handle;

    if (handle == s_handle_net_config || handle == s_handle_net_config_decl) {
        if (param->write.is_prep) {
            // Prepare Write: 缓冲数据
            uint16_t offset = param->write.offset;
            uint16_t len    = param->write.len;
            uint8_t *value  = param->write.value;

            ESP_LOGD(TAG, "Prepare write: offset=%d, len=%d", offset, len);

            if (offset + len > WRITE_BUF_MAX) {
                ESP_LOGE(TAG, "Write buffer overflow: %d + %d > %d",
                         offset, len, WRITE_BUF_MAX);
                // 发送错误响应
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                             param->write.trans_id,
                                             ESP_GATT_PREP_WRITE_CANCEL, NULL);
                return;
            }

            memcpy(s_write_buf + offset, value, len);
            s_write_offset = offset + len;

            // 确认 Prepare Write
            esp_gatt_rsp_t rsp;
            memset(&rsp, 0, sizeof(rsp));
            rsp.attr_value.handle = handle;
            rsp.attr_value.offset = offset;
            rsp.attr_value.len    = len;
            memcpy(rsp.attr_value.value, value, len);

            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                         param->write.trans_id,
                                         ESP_GATT_OK, &rsp);
        } else {
            // 直接 Write（短数据）
            ESP_LOGD(TAG, "Direct write: len=%d", param->write.len);
            if (param->write.len >= WRITE_BUF_MAX) {
                ESP_LOGE(TAG, "NetworkConfig is too large");
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                             param->write.trans_id,
                                             ESP_GATT_INVALID_ATTR_LEN, NULL);
                return;
            }
            memcpy(s_write_buf, param->write.value, param->write.len);
            s_write_offset = param->write.len;

            // 确认写入
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                         param->write.trans_id,
                                         ESP_GATT_OK, NULL);

            // 处理配置
            process_network_config();
        }
    } else if (handle == s_handle_status_cccd) {
        // CCCD 写入
        uint8_t *value = param->write.value;
        if (param->write.len == 2) {
            uint16_t cccd_value = value[0] | (value[1] << 8);
            s_cccd_enabled = (cccd_value & 0x0001) != 0; // Notify enabled
            ESP_LOGI(TAG, "Status CCCD: notify=%s",
                     s_cccd_enabled ? "enabled" : "disabled");
        }
    } else {
        ESP_LOGW(TAG, "Write on unknown handle %d", handle);
    }
}

/* ================================================================
 * Execute Write 事件处理
 * ================================================================ */
static void handle_exec_write_event(esp_gatt_if_t gatts_if,
                                    esp_ble_gatts_cb_param_t *param)
{
    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC) {
        ESP_LOGI(TAG, "Execute write: %d bytes", s_write_offset);
        process_network_config();
    } else {
        // Cancel: 丢弃缓冲区
        ESP_LOGI(TAG, "Write cancelled");
        s_write_offset = 0;
    }
}

/* ================================================================
 * 处理 NetworkConfig
 * ================================================================ */
static void process_network_config(void)
{
    if (s_write_offset == 0) {
        ESP_LOGW(TAG, "Empty NetworkConfig");
        return;
    }

    // 确保以 null 结尾
    if (s_write_offset >= WRITE_BUF_MAX) {
        s_write_offset = WRITE_BUF_MAX - 1;
    }
    s_write_buf[s_write_offset] = '\0';

    ESP_LOGI(TAG, "Received NetworkConfig (%zu bytes)", s_write_offset);

    network_config_t config;
    char error_message[128];
    network_config_result_t result = network_config_submit_json(
        (const char *)s_write_buf, s_write_offset, &config,
        error_message, sizeof(error_message));
    if (result != NETWORK_CONFIG_OK) {
        const char *error_code = "INVALID_CONFIG";
        if (result == NETWORK_CONFIG_STALE) {
            error_code = "STALE_CONFIG";
        } else if (result == NETWORK_CONFIG_CONFLICT) {
            error_code = "CONFIG_CONFLICT";
        } else if (result == NETWORK_CONFIG_STORAGE_ERROR) {
            error_code = "INTERNAL_ERROR";
        }
        status_set_error(error_code, error_message, false, 0);
        ble_prov_notify_status();
        s_write_offset = 0;
        return;
    }

    status_set_provisioning_id(config.provisioning_id);
    status_set_state(STATUS_CONFIG_RECEIVED);
    ble_prov_notify_status();
    if (connectivity_apply_config(&config) != 0) {
        status_set_error("INTERNAL_ERROR", "Failed to apply NetworkConfig",
                         true, 1000);
        ble_prov_notify_status();
    }

    ESP_LOGI(TAG, "Config accepted: revision=%llu, prov_id=%s",
             (unsigned long long)config.revision, config.provisioning_id);

    s_write_offset = 0;
}

/* ================================================================
 * GAP 事件处理
 * ================================================================ */
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Advertising data set");
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Scan response data set");
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Advertising started successfully");
        } else {
            ESP_LOGE(TAG, "Advertising start failed: 0x%x",
                     param->adv_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "Advertising stopped");
        break;

    default:
        break;
    }
}
