#include "factory_data.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "pb_decode.h"
#include "factory_data.pb.h"

static const char *TAG = "factory_data";

#define FACTORY_DATA_NVS_NAMESPACE "factory_data"
#define FACTORY_DATA_NVS_BLOB_KEY "pb_blob"
#define FACTORY_DATA_PARTITION_LABEL "factory_data"
#define DEFAULT_LWM2M_PORT 5683
#define FACTORY_DATA_READ_MAX 512

static esp_err_t decode_factory_data_blob(const uint8_t *blob,
                                          size_t blob_len,
                                          factory_data_config_t *out)
{
    if (!blob || blob_len == 0 || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    edge_device_factory_FactoryData pb = edge_device_factory_FactoryData_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(blob, blob_len);
    if (!pb_decode(&stream, edge_device_factory_FactoryData_fields, &pb)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->present = true;
    out->mode = (uint32_t)pb.mode;
    if (out->mode == edge_device_factory_WifiMode_WIFI_MODE_MESH) {
        out->mode = edge_device_factory_WifiMode_WIFI_MODE_STA;
        ESP_LOGI(TAG, "Factory data mode normalized: MESH -> STA");
    }
    out->country = (uint32_t)pb.country;
    if (pb.serial_number[0] != '\0') {
        strncpy(out->serial_number, pb.serial_number, sizeof(out->serial_number) - 1);
        out->serial_number[sizeof(out->serial_number) - 1] = '\0';
    }
    if (pb.lwm2m_server.host[0] != '\0') {
        strncpy(out->lwm2m_host, pb.lwm2m_server.host, sizeof(out->lwm2m_host) - 1);
        out->lwm2m_host[sizeof(out->lwm2m_host) - 1] = '\0';
    }
    out->lwm2m_port = (pb.lwm2m_server.port == 0) ? DEFAULT_LWM2M_PORT : pb.lwm2m_server.port;

    if (pb.device_private_key.size > sizeof(out->device_private_key)) {
        return ESP_ERR_INVALID_SIZE;
    }
    out->device_private_key_len = pb.device_private_key.size;
    if (out->device_private_key_len > 0) {
        memcpy(out->device_private_key,
               pb.device_private_key.bytes,
               out->device_private_key_len);
    }

    if (pb.ssid[0] != '\0') {
        strncpy(out->ssid, pb.ssid, sizeof(out->ssid) - 1);
        out->ssid[sizeof(out->ssid) - 1] = '\0';
    }

    if (pb.passphrase[0] != '\0') {
        strncpy(out->passphrase, pb.passphrase, sizeof(out->passphrase) - 1);
        out->passphrase[sizeof(out->passphrase) - 1] = '\0';
    }

    return ESP_OK;
}

static esp_err_t load_factory_data_from_partition(factory_data_config_t *out)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        FACTORY_DATA_PARTITION_LABEL);
    if (!partition) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t read_len = partition->size;
    if (read_len > FACTORY_DATA_READ_MAX) {
        read_len = FACTORY_DATA_READ_MAX;
    }

    uint8_t raw[FACTORY_DATA_READ_MAX] = {0};
    esp_err_t err = esp_partition_read(partition, 0, raw, read_len);
    if (err != ESP_OK) {
        return err;
    }

    ssize_t last_non_ff = -1;
    for (size_t i = 0; i < read_len; i++) {
        if (raw[i] != 0xFF) {
            last_non_ff = (ssize_t)i;
        }
    }

    if (last_non_ff < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t blob_len = (size_t)last_non_ff + 1;
    return decode_factory_data_blob(raw, blob_len, out);
}

static esp_err_t load_factory_data_from_nvs(factory_data_config_t *out)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(FACTORY_DATA_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t blob_len = 0;
    err = nvs_get_blob(nvs_handle, FACTORY_DATA_NVS_BLOB_KEY, NULL, &blob_len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    if (blob_len == 0 || blob_len > FACTORY_DATA_READ_MAX) {
        nvs_close(nvs_handle);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t blob[FACTORY_DATA_READ_MAX] = {0};
    err = nvs_get_blob(nvs_handle, FACTORY_DATA_NVS_BLOB_KEY, blob, &blob_len);
    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    return decode_factory_data_blob(blob, blob_len, out);
}

esp_err_t factory_data_load(factory_data_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    esp_err_t err = load_factory_data_from_partition(out);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Loaded factory data from partition (mode=%u country=%u host=%s port=%u)",
                 (unsigned)out->mode,
                 (unsigned)out->country,
                 out->lwm2m_host,
                 (unsigned)out->lwm2m_port);
        return ESP_OK;
    }

    // Backward-compatible fallback for older provisioning paths that persisted in NVS.
    err = load_factory_data_from_nvs(out);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "Factory data not found in partition or NVS");
        } else {
            ESP_LOGW(TAG, "Failed to load factory data: %s", esp_err_to_name(err));
        }
        return err;
    }

    ESP_LOGI(TAG,
             "Loaded factory data from NVS (mode=%u country=%u host=%s port=%u)",
             (unsigned)out->mode,
             (unsigned)out->country,
             out->lwm2m_host,
             (unsigned)out->lwm2m_port);

    return ESP_OK;
}

esp_err_t factory_data_build_lwm2m_uri(const factory_data_config_t *cfg,
                                       char *out_uri,
                                       size_t out_uri_len)
{
    if (!cfg || !out_uri || out_uri_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!cfg->present || cfg->lwm2m_host[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    int written = snprintf(out_uri, out_uri_len, "coap://%s:%u", cfg->lwm2m_host, (unsigned)cfg->lwm2m_port);
    if (written <= 0 || (size_t)written >= out_uri_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t factory_data_country_to_code(const factory_data_config_t *cfg,
                                       char out_code[3])
{
    if (!cfg || !cfg->present || !out_code) {
        return ESP_ERR_INVALID_ARG;
    }

    switch ((edge_device_factory_CountryCode)cfg->country) {
    case edge_device_factory_CountryCode_COUNTRY_US:
        out_code[0] = 'U'; out_code[1] = 'S'; out_code[2] = '\0';
        return ESP_OK;
    case edge_device_factory_CountryCode_COUNTRY_CA:
        out_code[0] = 'C'; out_code[1] = 'A'; out_code[2] = '\0';
        return ESP_OK;
    case edge_device_factory_CountryCode_COUNTRY_JP:
        out_code[0] = 'J'; out_code[1] = 'P'; out_code[2] = '\0';
        return ESP_OK;
    case edge_device_factory_CountryCode_COUNTRY_AU:
        out_code[0] = 'A'; out_code[1] = 'U'; out_code[2] = '\0';
        return ESP_OK;
    case edge_device_factory_CountryCode_COUNTRY_NZ:
        out_code[0] = 'N'; out_code[1] = 'Z'; out_code[2] = '\0';
        return ESP_OK;
    case edge_device_factory_CountryCode_COUNTRY_EU:
        out_code[0] = 'E'; out_code[1] = 'U'; out_code[2] = '\0';
        return ESP_OK;
    case edge_device_factory_CountryCode_COUNTRY_UNSPECIFIED:
    default:
        return ESP_ERR_NOT_FOUND;
    }
}

bool factory_data_is_wifi_only(const factory_data_config_t *cfg)
{
    if (!cfg || !cfg->present) {
        return false;
    }

    return (cfg->mode == edge_device_factory_WifiMode_WIFI_MODE_WIFI);
}

bool factory_data_is_mesh_mode(const factory_data_config_t *cfg)
{
    if (!cfg || !cfg->present) {
        return false;
    }

    return (cfg->mode == edge_device_factory_WifiMode_WIFI_MODE_MESH);
}
