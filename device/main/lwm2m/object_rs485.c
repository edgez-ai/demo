#include "object_interface_base.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "interface_bridge.h"

static iface_ctx_t s_rs485_ctx = {
    .tag = "lwm2m_rs485",
    .obj_id = LWM2M_OBJ_RS485,
    .fixed_type = IFACE_TYPE_MODBUS_UART,
    .res = {
        .type = RES_RS485_TYPE,
        .enabled = RES_RS485_ENABLED,
        .open_state = RES_RS485_OPEN_STATE,
        .tx_bytes = RES_RS485_TX_BYTES,
        .rx_bytes = RES_RS485_RX_BYTES,
        .error_count = RES_RS485_ERROR_COUNT,
        .last_error = RES_RS485_LAST_ERROR,
        .baudrate = RES_RS485_BAUDRATE,
        .i2c_address = 0,
        .modbus_unit_id = RES_RS485_MODBUS_UNIT_ID,
        .mode = RES_RS485_MODE,
        .stats_window_ms = RES_RS485_STATS_WINDOW_MS,
        .tx_rate = RES_RS485_TX_RATE,
        .rx_rate = RES_RS485_RX_RATE,
        .tx_payload = RES_RS485_TX_PAYLOAD,
        .rx_buffer_pos = RES_RS485_RX_BUFFER_POS,
        .rx_chunk = RES_RS485_RX_CHUNK,
        .rx_buffer_size = RES_RS485_RX_BUFFER_SIZE,
        .tx_pin = RES_RS485_TX_PIN,
        .rx_pin = RES_RS485_RX_PIN,
        .driver = RES_RS485_DRIVER,
        .object_ids = RES_RS485_OBJECT_IDS,
        .reset_counters = RES_RS485_RESET_COUNTERS,
    },
    .feat = {
        .baudrate = true,
        .i2c_address = false,
        .modbus_unit_id = true,
        .pins = true,
        .driver = true,
        .object_ids = true,
    },
};

lwm2m_object_t *get_rs485_object(void)
{
    return iface_get_object(&s_rs485_ctx);
}

esp_err_t rs485_object_set_instance(uint16_t instance_id, const rs485_instance_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!iface_get_object(&s_rs485_ctx)) return ESP_FAIL;

    iface_state_t *st = iface_alloc(&s_rs485_ctx, instance_id);
    if (!st) return ESP_ERR_NO_MEM;

    st->open_state = cfg->open_state;
    st->baudrate = cfg->baudrate;
    st->modbus_unit_id = cfg->modbus_unit_id;
    st->mode = cfg->mode;
    st->stats_window_ms = cfg->stats_window_ms;
    st->tx_rate = cfg->tx_rate;
    st->rx_rate = cfg->rx_rate;
    st->rx_buffer_size = cfg->rx_buffer_size;
    st->tx_pin = cfg->tx_pin;
    st->rx_pin = cfg->rx_pin;

    iface_rebuild_instance_list(&s_rs485_ctx);
    ESP_LOGI(s_rs485_ctx.tag, "RS485 instance set id=%u baud=%lu tx=%ld rx=%ld", (unsigned)instance_id,
             (unsigned long)st->baudrate, (long)st->tx_pin, (long)st->rx_pin);
    return ESP_OK;
}

esp_err_t rs485_object_update_counters(uint16_t instance_id, uint32_t tx_bytes, uint32_t rx_bytes,
                                       uint32_t error_count, uint32_t tx_rate, uint32_t rx_rate,
                                       const char *last_error)
{
    return iface_update_counters(&s_rs485_ctx, instance_id, tx_bytes, rx_bytes, error_count, tx_rate, rx_rate, last_error);
}

esp_err_t rs485_object_set_rx_cursor(uint16_t instance_id, int32_t pos)
{
    return iface_set_rx_cursor(&s_rs485_ctx, instance_id, pos);
}

esp_err_t rs485_object_set_rx_buffer_size(uint16_t instance_id, int32_t size)
{
    return iface_set_rx_buffer_size(&s_rs485_ctx, instance_id, size);
}

void rs485_object_set_handlers(interface_tx_handler_t tx_handler, interface_rx_handler_t rx_handler, interface_open_handler_t open_handler)
{
    iface_set_handlers(&s_rs485_ctx, tx_handler, rx_handler, open_handler);
}

esp_err_t rs485_object_invoke_open(uint16_t instance_id, bool open)
{
    if (!s_rs485_ctx.open_handler) {
        interface_bridge_register();
    }
    if (!s_rs485_ctx.open_handler) return ESP_ERR_INVALID_STATE;
    return s_rs485_ctx.open_handler(instance_id, open);
}

ssize_t rs485_object_invoke_tx(uint16_t instance_id, const uint8_t *data, size_t len)
{
    if (!s_rs485_ctx.tx_handler) return -1;
    return s_rs485_ctx.tx_handler(instance_id, data, len);
}

ssize_t rs485_object_invoke_rx(uint16_t instance_id, int32_t pos, uint8_t *out, size_t maxlen, int32_t *advanced)
{
    if (!s_rs485_ctx.rx_handler) return -1;
    return s_rs485_ctx.rx_handler(instance_id, pos, out, maxlen, advanced);
}

esp_err_t rs485_object_get_runtime(uint16_t instance_id, uint32_t *baudrate, uint32_t *modbus_unit_id,
                                   int32_t *tx_pin, int32_t *rx_pin,
                                   int32_t *rx_buffer_size)
{
    if (!iface_get_object(&s_rs485_ctx)) return ESP_FAIL;

    iface_state_t *st = NULL;
    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (s_rs485_ctx.slots[i].used && s_rs485_ctx.slots[i].id == instance_id) {
            st = &s_rs485_ctx.slots[i];
            break;
        }
    }
    if (!st) return ESP_ERR_INVALID_STATE;

    if (baudrate) *baudrate = st->baudrate;
    if (modbus_unit_id) *modbus_unit_id = st->modbus_unit_id;
    if (tx_pin) *tx_pin = st->tx_pin;
    if (rx_pin) *rx_pin = st->rx_pin;
    if (rx_buffer_size) *rx_buffer_size = st->rx_buffer_size;
    return ESP_OK;
}

esp_err_t rs485_object_update_runtime(uint16_t instance_id, uint32_t baudrate, uint32_t modbus_unit_id,
                                      int32_t tx_pin, int32_t rx_pin,
                                      int32_t rx_buffer_size)
{
    if (!iface_get_object(&s_rs485_ctx)) return ESP_FAIL;

    iface_state_t *st = NULL;
    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (s_rs485_ctx.slots[i].used && s_rs485_ctx.slots[i].id == instance_id) {
            st = &s_rs485_ctx.slots[i];
            break;
        }
    }
    if (!st) return ESP_ERR_INVALID_STATE;

    st->baudrate = baudrate;
    st->modbus_unit_id = modbus_unit_id;
    st->tx_pin = tx_pin;
    st->rx_pin = rx_pin;
    st->rx_buffer_size = rx_buffer_size;

    ESP_LOGI(s_rs485_ctx.tag, "RS485 runtime updated id=%u baud=%lu tx=%ld rx=%ld rx_buf=%ld",
             (unsigned)instance_id,
             (unsigned long)st->baudrate,
             (long)st->tx_pin,
             (long)st->rx_pin,
             (long)st->rx_buffer_size);
    return ESP_OK;
}

esp_err_t rs485_object_is_enabled(uint16_t instance_id, bool *enabled)
{
    if (!enabled) return ESP_ERR_INVALID_ARG;
    if (!iface_get_object(&s_rs485_ctx)) return ESP_FAIL;

    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (s_rs485_ctx.slots[i].used && s_rs485_ctx.slots[i].id == instance_id) {
            *enabled = s_rs485_ctx.slots[i].enabled;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}
