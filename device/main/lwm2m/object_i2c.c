#include "object_interface_base.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "interface_bridge.h"

static iface_ctx_t s_i2c_ctx = {
    .tag = "lwm2m_i2c",
    .obj_id = LWM2M_OBJ_I2C,
    .fixed_type = IFACE_TYPE_I2C,
    .res = {
        .type = RES_I2C_TYPE,
        .enabled = RES_I2C_ENABLED,
        .open_state = RES_I2C_OPEN_STATE,
        .tx_bytes = RES_I2C_TX_BYTES,
        .rx_bytes = RES_I2C_RX_BYTES,
        .error_count = RES_I2C_ERROR_COUNT,
        .last_error = RES_I2C_LAST_ERROR,
        .baudrate = 0,
        .i2c_address = RES_I2C_ADDRESS,
        .modbus_unit_id = 0,
        .mode = RES_I2C_MODE,
        .stats_window_ms = RES_I2C_STATS_WINDOW_MS,
        .tx_rate = RES_I2C_TX_RATE,
        .rx_rate = RES_I2C_RX_RATE,
        .tx_payload = RES_I2C_TX_PAYLOAD,
        .rx_buffer_pos = RES_I2C_RX_BUFFER_POS,
        .rx_chunk = RES_I2C_RX_CHUNK,
        .rx_buffer_size = RES_I2C_RX_BUFFER_SIZE,
        .tx_pin = RES_I2C_TX_PIN,
        .rx_pin = RES_I2C_RX_PIN,
        .driver = RES_I2C_DRIVER,
        .object_ids = RES_I2C_OBJECT_IDS,
        .reset_counters = RES_I2C_RESET_COUNTERS,
    },
    .feat = {
        .baudrate = false,
        .i2c_address = true,
        .modbus_unit_id = false,
        .pins = true,
        .driver = true,
        .object_ids = true,
    },
};

lwm2m_object_t *get_i2c_object(void)
{
    return iface_get_object(&s_i2c_ctx);
}

esp_err_t i2c_object_set_instance(uint16_t instance_id, const i2c_instance_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!iface_get_object(&s_i2c_ctx)) return ESP_FAIL;

    iface_state_t *st = iface_alloc(&s_i2c_ctx, instance_id);
    if (!st) return ESP_ERR_NO_MEM;

    st->open_state = cfg->open_state;
    st->i2c_address = cfg->i2c_address;
    st->mode = cfg->mode;
    st->stats_window_ms = cfg->stats_window_ms;
    st->tx_rate = cfg->tx_rate;
    st->rx_rate = cfg->rx_rate;
    st->rx_buffer_size = cfg->rx_buffer_size;
    st->tx_pin = cfg->tx_pin;
    st->rx_pin = cfg->rx_pin;


    iface_rebuild_instance_list(&s_i2c_ctx);
    ESP_LOGI(s_i2c_ctx.tag, "I2C instance set id=%u addr=0x%lx", (unsigned)instance_id, (unsigned long)st->i2c_address);
    return ESP_OK;
}

esp_err_t i2c_object_update_counters(uint16_t instance_id, uint32_t tx_bytes, uint32_t rx_bytes,
                                     uint32_t error_count, uint32_t tx_rate, uint32_t rx_rate,
                                     const char *last_error)
{
    return iface_update_counters(&s_i2c_ctx, instance_id, tx_bytes, rx_bytes, error_count, tx_rate, rx_rate, last_error);
}

esp_err_t i2c_object_set_rx_cursor(uint16_t instance_id, int32_t pos)
{
    return iface_set_rx_cursor(&s_i2c_ctx, instance_id, pos);
}

esp_err_t i2c_object_set_rx_buffer_size(uint16_t instance_id, int32_t size)
{
    return iface_set_rx_buffer_size(&s_i2c_ctx, instance_id, size);
}

void i2c_object_set_handlers(interface_tx_handler_t tx_handler, interface_rx_handler_t rx_handler, interface_open_handler_t open_handler)
{
    iface_set_handlers(&s_i2c_ctx, tx_handler, rx_handler, open_handler);
}

esp_err_t i2c_object_invoke_open(uint16_t instance_id, bool open)
{
    if (!s_i2c_ctx.open_handler) {
        interface_bridge_register();
    }
    if (!s_i2c_ctx.open_handler) return ESP_ERR_INVALID_STATE;
    return s_i2c_ctx.open_handler(instance_id, open);
}

ssize_t i2c_object_invoke_tx(uint16_t instance_id, const uint8_t *data, size_t len)
{
    if (!s_i2c_ctx.tx_handler) return -1;
    return s_i2c_ctx.tx_handler(instance_id, data, len);
}

ssize_t i2c_object_invoke_rx(uint16_t instance_id, int32_t pos, uint8_t *out, size_t maxlen, int32_t *advanced)
{
    if (!s_i2c_ctx.rx_handler) return -1;
    return s_i2c_ctx.rx_handler(instance_id, pos, out, maxlen, advanced);
}

esp_err_t i2c_object_get_runtime(uint16_t instance_id, uint32_t *i2c_addr,
                                 int32_t *sda_pin, int32_t *scl_pin,
                                 int32_t *rx_buffer_size)
{
    if (!iface_get_object(&s_i2c_ctx)) return ESP_FAIL;

    iface_state_t *st = NULL;
    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (s_i2c_ctx.slots[i].used && s_i2c_ctx.slots[i].id == instance_id) {
            st = &s_i2c_ctx.slots[i];
            break;
        }
    }
    if (!st) return ESP_ERR_INVALID_STATE;

    if (i2c_addr) *i2c_addr = st->i2c_address;
    if (sda_pin) *sda_pin = st->tx_pin;
    if (scl_pin) *scl_pin = st->rx_pin;
    if (rx_buffer_size) *rx_buffer_size = st->rx_buffer_size;
    return ESP_OK;
}

esp_err_t i2c_object_update_runtime(uint16_t instance_id, uint32_t i2c_addr,
                                    int32_t sda_pin, int32_t scl_pin,
                                    int32_t rx_buffer_size)
{
    if (!iface_get_object(&s_i2c_ctx)) return ESP_FAIL;

    iface_state_t *st = NULL;
    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (s_i2c_ctx.slots[i].used && s_i2c_ctx.slots[i].id == instance_id) {
            st = &s_i2c_ctx.slots[i];
            break;
        }
    }
    if (!st) return ESP_ERR_INVALID_STATE;

    st->i2c_address = i2c_addr;
    st->tx_pin = sda_pin;
    st->rx_pin = scl_pin;
    st->rx_buffer_size = rx_buffer_size;

    ESP_LOGI(s_i2c_ctx.tag, "I2C runtime updated id=%u addr=0x%lx sda=%ld scl=%ld rx_buf=%ld",
             (unsigned)instance_id,
             (unsigned long)st->i2c_address,
             (long)st->tx_pin,
             (long)st->rx_pin,
             (long)st->rx_buffer_size);
    return ESP_OK;
}

esp_err_t i2c_object_is_enabled(uint16_t instance_id, bool *enabled)
{
    if (!enabled) return ESP_ERR_INVALID_ARG;
    if (!iface_get_object(&s_i2c_ctx)) return ESP_FAIL;

    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (s_i2c_ctx.slots[i].used && s_i2c_ctx.slots[i].id == instance_id) {
            *enabled = s_i2c_ctx.slots[i].enabled;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}
