#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "esp_err.h"
#include "liblwm2m.h"
#include "lwm2m_client.h"
#include "object_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_IFACE_INSTANCES 4
#define LAST_ERROR_MAX 32
#define RX_CHUNK_MAX 2048

typedef struct {
    bool used;
    uint16_t id;
    interface_type_t type;
    bool enabled;
    bool open_state;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t error_count;
    char last_error[LAST_ERROR_MAX];
    uint32_t baudrate;
    uint32_t i2c_address;
    uint32_t modbus_unit_id;
    uint32_t mode;
    uint32_t stats_window_ms;
    uint32_t tx_rate;
    uint32_t rx_rate;
    int32_t rx_buffer_size;
    int32_t rx_buffer_pos;
    int32_t tx_pin;
    int32_t rx_pin;
    char *driver;
    char *object_ids;
} iface_state_t;

typedef struct {
    uint16_t type;
    uint16_t enabled;
    uint16_t open_state;
    uint16_t tx_bytes;
    uint16_t rx_bytes;
    uint16_t error_count;
    uint16_t last_error;
    uint16_t baudrate;
    uint16_t i2c_address;
    uint16_t modbus_unit_id;
    uint16_t mode;
    uint16_t stats_window_ms;
    uint16_t tx_rate;
    uint16_t rx_rate;
    uint16_t tx_payload;
    uint16_t rx_buffer_pos;
    uint16_t rx_chunk;
    uint16_t rx_buffer_size;
    uint16_t tx_pin;
    uint16_t rx_pin;
    uint16_t driver;
    uint16_t object_ids;
    uint16_t reset_counters;
} iface_resmap_t;

typedef struct {
    bool baudrate;
    bool i2c_address;
    bool modbus_unit_id;
    bool pins;
    bool driver;
    bool object_ids;
} iface_features_t;

typedef struct {
    const char *tag;
    uint16_t obj_id;
    interface_type_t fixed_type;
    iface_resmap_t res;
    iface_features_t feat;
    lwm2m_object_t *obj;
    iface_state_t slots[MAX_IFACE_INSTANCES];
    interface_tx_handler_t tx_handler;
    interface_rx_handler_t rx_handler;
    interface_open_handler_t open_handler;
} iface_ctx_t;

lwm2m_object_t *iface_get_object(iface_ctx_t *ctx);
iface_state_t *iface_alloc(iface_ctx_t *ctx, uint16_t id);
void iface_rebuild_instance_list(iface_ctx_t *ctx);

esp_err_t iface_update_counters(iface_ctx_t *ctx, uint16_t instance_id, uint32_t tx_bytes, uint32_t rx_bytes,
                                uint32_t error_count, uint32_t tx_rate, uint32_t rx_rate, const char *last_error);
esp_err_t iface_set_rx_cursor(iface_ctx_t *ctx, uint16_t instance_id, int32_t pos);
esp_err_t iface_set_rx_buffer_size(iface_ctx_t *ctx, uint16_t instance_id, int32_t size);
void iface_set_handlers(iface_ctx_t *ctx, interface_tx_handler_t tx_handler, interface_rx_handler_t rx_handler,
                       interface_open_handler_t open_handler);
void iface_handle_enabled_change(iface_ctx_t *ctx, bool enabled);

#ifdef __cplusplus
}
#endif
