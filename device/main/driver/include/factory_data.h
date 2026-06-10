#ifndef DRIVER_FACTORY_DATA_H
#define DRIVER_FACTORY_DATA_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool present;
    uint32_t mode;
    uint32_t country;
    char serial_number[65];
    char lwm2m_host[65];
    uint32_t lwm2m_port;
    uint8_t device_private_key[32];
    size_t device_private_key_len;
    char ssid[33];
    char passphrase[65];
} factory_data_config_t;

/* Load and decode factory data protobuf from NVS namespace factory_data/key pb_blob. */
esp_err_t factory_data_load(factory_data_config_t *out);

/* Build CoAP URI (coap://host:port) from decoded factory config. */
esp_err_t factory_data_build_lwm2m_uri(const factory_data_config_t *cfg,
                                       char *out_uri,
                                       size_t out_uri_len);

/* Map factory country enum to 2-letter ISO code (e.g. US/CA/JP/AU/NZ/EU). */
esp_err_t factory_data_country_to_code(const factory_data_config_t *cfg,
                                       char out_code[3]);

/* True when factory data indicates normal Wi-Fi mode (mode WIFI). */
bool factory_data_is_wifi_only(const factory_data_config_t *cfg);

/* True when factory data indicates HaLow mesh mode (mode MESH). */
bool factory_data_is_mesh_mode(const factory_data_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_FACTORY_DATA_H */
