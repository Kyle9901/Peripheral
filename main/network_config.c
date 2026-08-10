#include "network_config.h"

#include "dev_info.h"
#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "network_config";

#define NVS_NAMESPACE       "instacare"
#define NVS_KEY_NET_CONFIG  "net_config"
#define CONFIG_BLOB_MAGIC   0x49434e46UL /* ICNF */
#define CONFIG_BLOB_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    network_config_t config;
} config_blob_t;

static network_config_t s_config;
static bool s_has_config;

static void set_error(char *buffer, size_t size, const char *message)
{
    if (buffer == NULL || size == 0) {
        return;
    }
    snprintf(buffer, size, "%s", message);
}

static bool utf8_character_count(const char *value, size_t *character_count)
{
    const unsigned char *p = (const unsigned char *)value;
    size_t count = 0;

    while (*p != '\0') {
        size_t continuation_count;
        uint32_t codepoint;

        if (*p < 0x80) {
            continuation_count = 0;
            codepoint = *p;
        } else if ((*p & 0xe0) == 0xc0) {
            continuation_count = 1;
            codepoint = *p & 0x1f;
        } else if ((*p & 0xf0) == 0xe0) {
            continuation_count = 2;
            codepoint = *p & 0x0f;
        } else if ((*p & 0xf8) == 0xf0) {
            continuation_count = 3;
            codepoint = *p & 0x07;
        } else {
            return false;
        }

        p++;
        for (size_t i = 0; i < continuation_count; i++, p++) {
            if ((*p & 0xc0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (*p & 0x3f);
        }
        if ((continuation_count == 1 && codepoint < 0x80) ||
            (continuation_count == 2 && codepoint < 0x800) ||
            (continuation_count == 3 && codepoint < 0x10000) ||
            codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
        count++;
    }

    *character_count = count;
    return true;
}

static bool is_hex_string(const char *value, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (!isxdigit((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

static bool copy_json_string(const cJSON *object, const char *name,
                             char *destination, size_t destination_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    size_t length = strlen(item->valuestring);
    if (length >= destination_size) {
        return false;
    }
    memcpy(destination, item->valuestring, length + 1);
    return true;
}

/* cJSON 使用 double 保存数值；从原始文本解析以完整保留 uint64。 */
static bool parse_revision(const char *json, uint64_t *revision)
{
    const char *key = strstr(json, "\"config_revision\"");
    if (key == NULL) {
        return false;
    }
    const char *colon = strchr(key + strlen("\"config_revision\""), ':');
    if (colon == NULL) {
        return false;
    }
    const char *p = colon + 1;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (!isdigit((unsigned char)*p)) {
        return false;
    }

    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(p, &end, 10);
    if (errno == ERANGE || end == p) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != ',' && *end != '}') {
        return false;
    }
    *revision = (uint64_t)value;
    return true;
}

static bool configs_equal(const network_config_t *left,
                          const network_config_t *right)
{
    return left->revision == right->revision &&
           left->security == right->security &&
           left->central_port == right->central_port &&
           strcmp(left->provisioning_id, right->provisioning_id) == 0 &&
           strcmp(left->ssid, right->ssid) == 0 &&
           strcmp(left->password, right->password) == 0 &&
           strcmp(left->central_address, right->central_address) == 0;
}

static esp_err_t save_config(const network_config_t *config)
{
    config_blob_t blob = {
        .magic = CONFIG_BLOB_MAGIC,
        .version = CONFIG_BLOB_VERSION,
        .size = sizeof(network_config_t),
        .config = *config,
    };
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle, NVS_KEY_NET_CONFIG, &blob, sizeof(blob));
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

int network_config_init(void)
{
    s_has_config = false;
    memset(&s_config, 0, sizeof(s_config));

    nvs_handle_t handle;
    esp_err_t error = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return 0;
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(error));
        return -1;
    }

    config_blob_t blob;
    size_t blob_size = sizeof(blob);
    error = nvs_get_blob(handle, NVS_KEY_NET_CONFIG, &blob, &blob_size);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return 0;
    }
    if (error != ESP_OK || blob_size != sizeof(blob) ||
        blob.magic != CONFIG_BLOB_MAGIC ||
        blob.version != CONFIG_BLOB_VERSION ||
        blob.size != sizeof(network_config_t)) {
        ESP_LOGW(TAG, "Ignoring invalid saved network configuration");
        return 0;
    }

    s_config = blob.config;
    s_has_config = true;
    ESP_LOGI(TAG, "Loaded network config revision=%llu, central=%s:%u",
             (unsigned long long)s_config.revision,
             s_config.central_address, s_config.central_port);
    return 0;
}

bool network_config_get(network_config_t *out_config)
{
    if (!s_has_config || out_config == NULL) {
        return false;
    }
    *out_config = s_config;
    return true;
}

network_config_result_t network_config_submit_json(
    const char *json, size_t json_len, network_config_t *out_config,
    char *error_message, size_t error_message_size)
{
    if (json == NULL || json_len == 0 || json_len > 4095) {
        set_error(error_message, error_message_size, "Invalid JSON length");
        return NETWORK_CONFIG_INVALID;
    }

    char *document = malloc(json_len + 1);
    if (document == NULL) {
        set_error(error_message, error_message_size, "Out of memory");
        return NETWORK_CONFIG_STORAGE_ERROR;
    }
    memcpy(document, json, json_len);
    document[json_len] = '\0';

    cJSON *root = cJSON_ParseWithLength(document, json_len);
    network_config_t candidate = {0};
    network_config_result_t result = NETWORK_CONFIG_INVALID;
    if (!cJSON_IsObject(root)) {
        set_error(error_message, error_message_size, "NetworkConfig must be a JSON object");
        goto done;
    }

    const cJSON *spec = cJSON_GetObjectItemCaseSensitive(root, "spec");
    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    const cJSON *central = cJSON_GetObjectItemCaseSensitive(root, "central");
    const cJSON *revision_item = cJSON_GetObjectItemCaseSensitive(root, "config_revision");
    if (!cJSON_IsString(spec) || strcmp(spec->valuestring, "instacare.provisioning/1.0") != 0 ||
        !cJSON_IsObject(wifi) || !cJSON_IsObject(central) ||
        !cJSON_IsNumber(revision_item) ||
        !parse_revision(document, &candidate.revision) || candidate.revision == 0 ||
        !copy_json_string(root, "provisioning_id", candidate.provisioning_id,
                          sizeof(candidate.provisioning_id)) ||
        candidate.provisioning_id[0] == '\0') {
        set_error(error_message, error_message_size, "Missing or invalid provisioning fields");
        goto done;
    }

    char security[16];
    if (!copy_json_string(wifi, "ssid", candidate.ssid, sizeof(candidate.ssid)) ||
        !copy_json_string(wifi, "password", candidate.password, sizeof(candidate.password)) ||
        !copy_json_string(wifi, "security", security, sizeof(security)) ||
        !copy_json_string(central, "address", candidate.central_address,
                          sizeof(candidate.central_address))) {
        set_error(error_message, error_message_size, "Missing or oversized network field");
        goto done;
    }

    const cJSON *port = cJSON_GetObjectItemCaseSensitive(central, "port");
    if (!cJSON_IsNumber(port) || port->valuedouble < 1 || port->valuedouble > 65535 ||
        floor(port->valuedouble) != port->valuedouble ||
        strlen(candidate.ssid) < 1 || strlen(candidate.ssid) > NETWORK_CONFIG_SSID_MAX ||
        candidate.central_address[0] == '\0') {
        set_error(error_message, error_message_size, "Invalid SSID, address or port");
        goto done;
    }
    candidate.central_port = (uint16_t)port->valueint;

    size_t password_characters = 0;
    if (!utf8_character_count(candidate.ssid, &password_characters) ||
        !utf8_character_count(candidate.password, &password_characters)) {
        set_error(error_message, error_message_size, "SSID or password is not valid UTF-8");
        goto done;
    }

    size_t password_bytes = strlen(candidate.password);
    if (strcmp(security, "open") == 0) {
        candidate.security = NETWORK_SECURITY_OPEN;
        if (password_bytes != 0) {
            set_error(error_message, error_message_size, "Open Wi-Fi requires an empty password");
            goto done;
        }
    } else if (strcmp(security, "wpa2_psk") == 0) {
        candidate.security = NETWORK_SECURITY_WPA2_PSK;
        bool raw_psk = password_bytes == 64 && is_hex_string(candidate.password, password_bytes);
        if (!raw_psk && (password_characters < 8 || password_characters > 63)) {
            set_error(error_message, error_message_size, "Invalid WPA2 password length");
            goto done;
        }
    } else if (strcmp(security, "wpa3_sae") == 0) {
        candidate.security = NETWORK_SECURITY_WPA3_SAE;
        if (password_characters < 1 || password_characters > 63) {
            set_error(error_message, error_message_size, "Invalid WPA3 password length");
            goto done;
        }
    } else {
        set_error(error_message, error_message_size, "Unsupported Wi-Fi security type");
        goto done;
    }

    if (s_has_config &&
        strcmp(candidate.provisioning_id, s_config.provisioning_id) == 0) {
        if (!configs_equal(&candidate, &s_config)) {
            set_error(error_message, error_message_size,
                      "Provisioning transaction contains different content");
            result = NETWORK_CONFIG_CONFLICT;
            goto done;
        }
        if (out_config != NULL) {
            *out_config = s_config;
        }
        result = NETWORK_CONFIG_OK;
        goto done;
    }

    if (candidate.revision <= dev_info_get_config_revision()) {
        set_error(error_message, error_message_size, "Config revision not incremented");
        result = NETWORK_CONFIG_STALE;
        goto done;
    }

    esp_err_t save_error = save_config(&candidate);
    if (save_error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save network config: %s", esp_err_to_name(save_error));
        set_error(error_message, error_message_size, "Failed to persist NetworkConfig");
        result = NETWORK_CONFIG_STORAGE_ERROR;
        goto done;
    }

    s_config = candidate;
    s_has_config = true;
    dev_info_set_config_revision(candidate.revision);
    if (out_config != NULL) {
        *out_config = candidate;
    }
    ESP_LOGI(TAG, "Accepted config revision=%llu, SSID length=%u, central=%s:%u",
             (unsigned long long)candidate.revision,
             (unsigned)strlen(candidate.ssid),
             candidate.central_address, candidate.central_port);
    result = NETWORK_CONFIG_OK;

done:
    cJSON_Delete(root);
    free(document);
    return result;
}
