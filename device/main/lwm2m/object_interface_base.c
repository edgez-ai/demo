#include "object_interface_base.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "interface_bridge.h"

#define IFACE_NVS_NAMESPACE "iface"
#define IFACE_DRIVER_NVS_PARTITION "driver_nvs"
#define IFACE_DRIVER_MAX_LEN (256 * 1024)
#define IFACE_OBJECT_IDS_MAX_LEN 1024

#define IFACE_GPIO_RS485 GPIO_NUM_13
#define IFACE_GPIO_I2C_UART GPIO_NUM_14

static iface_ctx_t *s_i2c_ctx = NULL;
static iface_ctx_t *s_uart_ctx = NULL;
static iface_ctx_t *s_rs485_ctx = NULL;
static bool s_iface_driver_nvs_ready = false;

static void notify_change(iface_ctx_t *ctx, uint16_t instance_id, uint16_t res_id);

static esp_err_t iface_state_set_driver(iface_state_t *st, const char *driver)
{
    if (!st || !driver) return ESP_ERR_INVALID_ARG;

    size_t len = strlen(driver);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return ESP_ERR_NO_MEM;

    memcpy(copy, driver, len + 1);
    if (st->driver) free(st->driver);
    st->driver = copy;
    return ESP_OK;
}

static esp_err_t iface_state_set_object_ids(iface_state_t *st, const char *object_ids)
{
    if (!st || !object_ids) return ESP_ERR_INVALID_ARG;

    size_t len = strlen(object_ids);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return ESP_ERR_NO_MEM;

    memcpy(copy, object_ids, len + 1);
    if (st->object_ids) free(st->object_ids);
    st->object_ids = copy;
    return ESP_OK;
}

static bool iface_validate_object_ids_string(const char *s)
{
    if (!s) return false;
    if (*s == '\0') return true;

    const char *p = s;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == '\0') return true;

        unsigned value = 0;
        int digits = 0;
        while (isdigit((unsigned char)*p)) {
            value = value * 10u + (unsigned)(*p - '0');
            if (value > 65535u) return false;
            p++;
            digits++;
        }

        if (digits == 0) return false;

        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') return true;
        if (*p != ',') return false;
        p++;
    }

    return true;
}

static esp_err_t iface_driver_nvs_open(nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    if (!s_iface_driver_nvs_ready) {
        esp_err_t init_err = nvs_flash_init_partition(IFACE_DRIVER_NVS_PARTITION);
        if (init_err != ESP_OK && init_err != ESP_ERR_NVS_NO_FREE_PAGES && init_err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
            return init_err;
        }
        if (init_err == ESP_ERR_NVS_NO_FREE_PAGES || init_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            init_err = nvs_flash_erase_partition(IFACE_DRIVER_NVS_PARTITION);
            if (init_err != ESP_OK) return init_err;
            init_err = nvs_flash_init_partition(IFACE_DRIVER_NVS_PARTITION);
            if (init_err != ESP_OK) return init_err;
        }
        s_iface_driver_nvs_ready = true;
    }

    return nvs_open_from_partition(IFACE_DRIVER_NVS_PARTITION, IFACE_NVS_NAMESPACE, mode, out);
}

static void iface_register_ctx(iface_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->obj_id == LWM2M_OBJ_I2C) {
        s_i2c_ctx = ctx;
    } else if (ctx->obj_id == LWM2M_OBJ_UART) {
        s_uart_ctx = ctx;
    } else if (ctx->obj_id == LWM2M_OBJ_RS485) {
        s_rs485_ctx = ctx;
    }
}

static bool iface_ctx_any_enabled(const iface_ctx_t *ctx)
{
    if (!ctx) return false;
    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (ctx->slots[i].used && ctx->slots[i].enabled) return true;
    }
    return false;
}

static void iface_ctx_disable_all(iface_ctx_t *ctx)
{
    if (!ctx) return;
    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (ctx->slots[i].used && ctx->slots[i].enabled) {
            ctx->slots[i].enabled = false;
            notify_change(ctx, ctx->slots[i].id, ctx->res.enabled);
        }
    }
}

void iface_handle_enabled_change(iface_ctx_t *ctx, bool enabled)
{
    if (!ctx) return;

    bool is_i2c = (ctx->obj_id == LWM2M_OBJ_I2C);
    bool is_uart = (ctx->obj_id == LWM2M_OBJ_UART);
    bool is_rs485 = (ctx->obj_id == LWM2M_OBJ_RS485);

    if (enabled) {
        if (is_i2c) {
            iface_ctx_disable_all(s_uart_ctx);
        } else if (is_uart) {
            iface_ctx_disable_all(s_i2c_ctx);
        }
    }

    if (is_rs485) {
        bool rs485_on = iface_ctx_any_enabled(s_rs485_ctx);
        esp_err_t err = gpio_set_level(IFACE_GPIO_RS485, rs485_on ? 1 : 0);
        if (err != ESP_OK) {
            ESP_LOGW("iface_power", "Failed setting RS485 power GPIO13 to %d: %s", rs485_on ? 1 : 0, esp_err_to_name(err));
        }
    }

    if (is_i2c || is_uart) {
        bool i2c_on = iface_ctx_any_enabled(s_i2c_ctx);
        bool uart_on = iface_ctx_any_enabled(s_uart_ctx);
        bool i2c_uart_on = (i2c_on || uart_on);
        esp_err_t err = gpio_set_level(IFACE_GPIO_I2C_UART, i2c_uart_on ? 1 : 0);
        if (err != ESP_OK) {
            ESP_LOGW("iface_power", "Failed setting I2C/UART power GPIO14 to %d: %s", i2c_uart_on ? 1 : 0, esp_err_to_name(err));
        }
    }
}

static void iface_nvs_key(char *out, size_t out_len, const iface_ctx_t *ctx, uint16_t instance_id)
{
    if (!out || out_len == 0 || !ctx) return;
    snprintf(out, out_len, "t%04x%02x", (unsigned)ctx->obj_id, (unsigned)(instance_id & 0xFF));
}

static void iface_nvs_driver_key(char *out, size_t out_len, const iface_ctx_t *ctx, uint16_t instance_id)
{
    if (!out || out_len == 0 || !ctx) return;
    snprintf(out, out_len, "d%04x%02x", (unsigned)ctx->obj_id, (unsigned)(instance_id & 0xFF));
}

static void iface_nvs_object_ids_key(char *out, size_t out_len, const iface_ctx_t *ctx, uint16_t instance_id)
{
    if (!out || out_len == 0 || !ctx) return;
    snprintf(out, out_len, "o%04x%02x", (unsigned)ctx->obj_id, (unsigned)(instance_id & 0xFF));
}

static esp_err_t iface_nvs_save_type(const iface_ctx_t *ctx, uint16_t instance_id, interface_type_t type)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(IFACE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    char key[16] = {0};
    iface_nvs_key(key, sizeof(key), ctx, instance_id);
    err = nvs_set_u16(nvs, key, (uint16_t)type);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        ESP_LOGI(ctx->tag, "Persisted iface type to NVS key=%s inst=%u type=%u", key, (unsigned)instance_id, (unsigned)type);
    } else {
        ESP_LOGW(ctx->tag, "Failed to persist iface type key=%s inst=%u err=%s", key, (unsigned)instance_id, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t iface_nvs_load_type(const iface_ctx_t *ctx, uint16_t instance_id, interface_type_t *out_type)
{
    if (!ctx || !out_type) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(IFACE_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    char key[16] = {0};
    iface_nvs_key(key, sizeof(key), ctx, instance_id);

    uint16_t val = 0;
    err = nvs_get_u16(nvs, key, &val);
    nvs_close(nvs);
    if (err != ESP_OK) return err;

    *out_type = (interface_type_t)val;
    ESP_LOGI(ctx->tag, "Loaded iface type from NVS key=%s inst=%u type=%u", key, (unsigned)instance_id, (unsigned)val);
    return ESP_OK;
}

static esp_err_t iface_nvs_save_driver(const iface_ctx_t *ctx, uint16_t instance_id, const char *driver)
{
    if (!ctx || !driver) return ESP_ERR_INVALID_ARG;

    size_t len = strlen(driver);
    if (len > IFACE_DRIVER_MAX_LEN) return ESP_ERR_INVALID_SIZE;

    nvs_handle_t nvs = 0;
    esp_err_t err = iface_driver_nvs_open(NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    char key[16] = {0};
    iface_nvs_driver_key(key, sizeof(key), ctx, instance_id);
    err = nvs_set_str(nvs, key, driver);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        ESP_LOGI(ctx->tag, "Persisted iface driver to NVS key=%s inst=%u len=%u", key, (unsigned)instance_id, (unsigned)len);
    } else {
        ESP_LOGW(ctx->tag, "Failed to persist iface driver key=%s inst=%u err=%s", key, (unsigned)instance_id, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t iface_nvs_load_driver(const iface_ctx_t *ctx, uint16_t instance_id, iface_state_t *st)
{
    if (!ctx || !st) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs = 0;
    esp_err_t err = iface_driver_nvs_open(NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    char key[16] = {0};
    iface_nvs_driver_key(key, sizeof(key), ctx, instance_id);

    size_t needed = 0;
    err = nvs_get_str(nvs, key, NULL, &needed);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    if (needed == 0 || needed > IFACE_DRIVER_MAX_LEN + 1) {
        nvs_close(nvs);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = (char *)malloc(needed);
    if (!buf) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(nvs, key, buf, &needed);
    nvs_close(nvs);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    if (st->driver) free(st->driver);
    st->driver = buf;

    ESP_LOGI(ctx->tag, "Loaded iface driver from NVS key=%s inst=%u len=%u", key, (unsigned)instance_id, (unsigned)(needed - 1));
    return ESP_OK;
}

static esp_err_t iface_nvs_save_object_ids(const iface_ctx_t *ctx, uint16_t instance_id, const char *object_ids)
{
    if (!ctx || !object_ids) return ESP_ERR_INVALID_ARG;

    size_t len = strlen(object_ids);
    if (len > IFACE_OBJECT_IDS_MAX_LEN) return ESP_ERR_INVALID_SIZE;
    if (!iface_validate_object_ids_string(object_ids)) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs = 0;
    esp_err_t err = iface_driver_nvs_open(NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    char key[16] = {0};
    iface_nvs_object_ids_key(key, sizeof(key), ctx, instance_id);
    err = nvs_set_str(nvs, key, object_ids);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        ESP_LOGI(ctx->tag, "Persisted iface object_ids to NVS key=%s inst=%u len=%u", key, (unsigned)instance_id, (unsigned)len);
    } else {
        ESP_LOGW(ctx->tag, "Failed to persist iface object_ids key=%s inst=%u err=%s", key, (unsigned)instance_id, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t iface_nvs_load_object_ids(const iface_ctx_t *ctx, uint16_t instance_id, iface_state_t *st)
{
    if (!ctx || !st) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs = 0;
    esp_err_t err = iface_driver_nvs_open(NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    char key[16] = {0};
    iface_nvs_object_ids_key(key, sizeof(key), ctx, instance_id);

    size_t needed = 0;
    err = nvs_get_str(nvs, key, NULL, &needed);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    if (needed == 0 || needed > IFACE_OBJECT_IDS_MAX_LEN + 1) {
        nvs_close(nvs);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = (char *)malloc(needed);
    if (!buf) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(nvs, key, buf, &needed);
    nvs_close(nvs);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    if (!iface_validate_object_ids_string(buf)) {
        free(buf);
        return ESP_ERR_INVALID_ARG;
    }

    if (st->object_ids) free(st->object_ids);
    st->object_ids = buf;

    ESP_LOGI(ctx->tag, "Loaded iface object_ids from NVS key=%s inst=%u len=%u", key, (unsigned)instance_id, (unsigned)(needed - 1));
    return ESP_OK;
}

static iface_ctx_t *ctx_from_object(lwm2m_object_t *objectP)
{
    return objectP ? (iface_ctx_t *)objectP->userData : NULL;
}

iface_state_t *iface_alloc(iface_ctx_t *ctx, uint16_t id)
{
    if (!ctx || id >= UINT16_MAX) return NULL;

    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (ctx->slots[i].used && ctx->slots[i].id == id) return &ctx->slots[i];
    }

    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (!ctx->slots[i].used) {
            memset(&ctx->slots[i], 0, sizeof(iface_state_t));
            ctx->slots[i].used = true;
            ctx->slots[i].id = id;
            interface_type_t saved_type = ctx->fixed_type;
            if (iface_nvs_load_type(ctx, id, &saved_type) == ESP_OK) {
                ctx->slots[i].type = saved_type;
            } else {
                ctx->slots[i].type = ctx->fixed_type;
            }
            if (ctx->feat.driver) {
                (void)iface_nvs_load_driver(ctx, id, &ctx->slots[i]);
            }
            if (ctx->feat.object_ids) {
                (void)iface_nvs_load_object_ids(ctx, id, &ctx->slots[i]);
            }
            return &ctx->slots[i];
        }
    }
    return NULL;
}

void iface_rebuild_instance_list(iface_ctx_t *ctx)
{
    if (!ctx || !ctx->obj) return;

    lwm2m_list_free(ctx->obj->instanceList);
    ctx->obj->instanceList = NULL;

    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (!ctx->slots[i].used) continue;

        lwm2m_list_t *node = (lwm2m_list_t *)lwm2m_malloc(sizeof(lwm2m_list_t));
        if (!node) {
            ESP_LOGW(ctx->tag, "Failed to alloc list node for iface %u", (unsigned)ctx->slots[i].id);
            continue;
        }
        memset(node, 0, sizeof(lwm2m_list_t));
        node->id = ctx->slots[i].id;
        ctx->obj->instanceList = lwm2m_list_add(ctx->obj->instanceList, node);
        ESP_LOGI(ctx->tag, "Obj list add id=%u", (unsigned)node->id);
    }
}

static void notify_change(iface_ctx_t *ctx, uint16_t instance_id, uint16_t res_id)
{
    lwm2m_context_t *lwm2m = get_lwm2m_context();
    if (!ctx || !lwm2m || lwm2m->state != STATE_READY) return;

    lwm2m_uri_t uri = {
        .objectId = ctx->obj_id,
        .instanceId = instance_id,
        .resourceId = res_id,
    };
    lwm2m_resource_value_changed(lwm2m, &uri);
}

static void add_res(uint16_t *out, size_t max, size_t *idx, uint16_t res)
{
    if (!out || !idx || *idx >= max) return;
    out[(*idx)++] = res;
}

static size_t build_resource_list(const iface_ctx_t *ctx, uint16_t *out, size_t max)
{
    size_t idx = 0;
    if (!ctx) return 0;

    add_res(out, max, &idx, ctx->res.type);
    add_res(out, max, &idx, ctx->res.enabled);
    add_res(out, max, &idx, ctx->res.open_state);
    add_res(out, max, &idx, ctx->res.tx_bytes);
    add_res(out, max, &idx, ctx->res.rx_bytes);
    add_res(out, max, &idx, ctx->res.error_count);
    add_res(out, max, &idx, ctx->res.last_error);
    if (ctx->feat.baudrate) add_res(out, max, &idx, ctx->res.baudrate);
    if (ctx->feat.i2c_address) add_res(out, max, &idx, ctx->res.i2c_address);
    if (ctx->feat.modbus_unit_id) add_res(out, max, &idx, ctx->res.modbus_unit_id);
    add_res(out, max, &idx, ctx->res.mode);
    add_res(out, max, &idx, ctx->res.stats_window_ms);
    add_res(out, max, &idx, ctx->res.tx_rate);
    add_res(out, max, &idx, ctx->res.rx_rate);
    add_res(out, max, &idx, ctx->res.rx_buffer_pos);
    add_res(out, max, &idx, ctx->res.rx_buffer_size);
    if (ctx->feat.pins) {
        add_res(out, max, &idx, ctx->res.tx_pin);
        add_res(out, max, &idx, ctx->res.rx_pin);
    }
    if (ctx->feat.driver) add_res(out, max, &idx, ctx->res.driver);
    if (ctx->feat.object_ids) add_res(out, max, &idx, ctx->res.object_ids);
    add_res(out, max, &idx, ctx->res.rx_chunk);

    return idx;
}

static uint8_t read_common(const iface_ctx_t *ctx, iface_state_t *st, lwm2m_data_t *data)
{
    if (!ctx || !st || !data) return COAP_500_INTERNAL_SERVER_ERROR;

    if (data->id == ctx->res.type) {
        lwm2m_data_encode_int(st->type, data);
        return COAP_205_CONTENT;
    }
    if (data->id == ctx->res.enabled) {
        lwm2m_data_encode_bool(st->enabled, data);
        return COAP_205_CONTENT;
    }
    if (data->id == ctx->res.open_state) {
        lwm2m_data_encode_bool(st->open_state, data);
        return COAP_205_CONTENT;
    }
    if (data->id == ctx->res.tx_bytes) {
        lwm2m_data_encode_int(st->tx_bytes, data);
        return COAP_205_CONTENT;
    }
    if (data->id == ctx->res.rx_bytes) {
        lwm2m_data_encode_int(st->rx_bytes, data);
        return COAP_205_CONTENT;
    }
    if (data->id == ctx->res.error_count) {
        lwm2m_data_encode_int(st->error_count, data);
        return COAP_205_CONTENT;
    }
    if (data->id == ctx->res.last_error) {
        if (st->last_error[0] == '\0') return COAP_404_NOT_FOUND;
        lwm2m_data_encode_string(st->last_error, data);
        return COAP_205_CONTENT;
    }
    if (ctx->feat.baudrate && data->id == ctx->res.baudrate) {
        lwm2m_data_encode_int(st->baudrate, data);
        return COAP_205_CONTENT;
    }
    if (ctx->feat.i2c_address && data->id == ctx->res.i2c_address) {
        lwm2m_data_encode_int(st->i2c_address, data);
        return COAP_205_CONTENT;
    }
    if (ctx->feat.modbus_unit_id && data->id == ctx->res.modbus_unit_id) {
        lwm2m_data_encode_int(st->modbus_unit_id, data);
        return COAP_205_CONTENT;
    }
    if (data->id == ctx->res.mode) {
        lwm2m_data_encode_int(st->mode, data);
        return COAP_205_CONTENT;
    }
    if (data->id == ctx->res.stats_window_ms) {
        lwm2m_data_encode_int(st->stats_window_ms, data);
        return COAP_205_CONTENT;
    }
    if (data->id == ctx->res.tx_rate) {
        lwm2m_data_encode_int(st->tx_rate, data);
        return COAP_205_CONTENT;
    }
    if (data->id == ctx->res.rx_rate) {
        lwm2m_data_encode_int(st->rx_rate, data);
        return COAP_205_CONTENT;
    }
    if (data->id == ctx->res.rx_buffer_pos) {
        lwm2m_data_encode_int(st->rx_buffer_pos, data);
        return COAP_205_CONTENT;
    }
    if (data->id == ctx->res.rx_buffer_size) {
        lwm2m_data_encode_int(st->rx_buffer_size, data);
        return COAP_205_CONTENT;
    }
    if (ctx->feat.pins && data->id == ctx->res.tx_pin) {
        lwm2m_data_encode_int(st->tx_pin, data);
        return COAP_205_CONTENT;
    }
    if (ctx->feat.pins && data->id == ctx->res.rx_pin) {
        lwm2m_data_encode_int(st->rx_pin, data);
        return COAP_205_CONTENT;
    }
    if (ctx->feat.driver && data->id == ctx->res.driver) {
        lwm2m_data_encode_string(st->driver ? st->driver : "", data);
        return COAP_205_CONTENT;
    }
    if (ctx->feat.object_ids && data->id == ctx->res.object_ids) {
        lwm2m_data_encode_string(st->object_ids ? st->object_ids : "", data);
        return COAP_205_CONTENT;
    }
    return COAP_404_NOT_FOUND;
}

static uint8_t prv_read(lwm2m_context_t *contextP, uint16_t instanceId, int *numDataP, lwm2m_data_t **dataArrayP, lwm2m_object_t *objectP)
{
    (void)contextP;
    iface_ctx_t *ctx = ctx_from_object(objectP);
    if (!ctx) return COAP_500_INTERNAL_SERVER_ERROR;

    iface_state_t *st = NULL;
    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (ctx->slots[i].used && ctx->slots[i].id == instanceId) {
            st = &ctx->slots[i];
            break;
        }
    }
    if (!st) return COAP_404_NOT_FOUND;

    if (*numDataP == 0) {
        uint16_t resources[24];
        size_t count = build_resource_list(ctx, resources, sizeof(resources) / sizeof(resources[0]));
        *dataArrayP = lwm2m_data_new((int)count);
        if (!*dataArrayP) return COAP_500_INTERNAL_SERVER_ERROR;
        *numDataP = (int)count;
        for (size_t i = 0; i < count; i++) (*dataArrayP)[i].id = resources[i];
    }

    for (int i = 0; i < *numDataP; i++) {
        lwm2m_data_t *data = (*dataArrayP) + i;
        if (data->id == ctx->res.rx_chunk) {
            if (!ctx->rx_handler) return COAP_503_SERVICE_UNAVAILABLE;
            uint8_t buf[RX_CHUNK_MAX];
            size_t chunk_max = sizeof(buf);
            if (st->rx_buffer_size > 0) {
                int32_t remaining = st->rx_buffer_size - st->rx_buffer_pos;
                if (remaining <= 0) {
                    lwm2m_data_encode_opaque(buf, 0, data);
                    return COAP_205_CONTENT;
                }
                if (remaining < (int32_t)chunk_max) chunk_max = (size_t)remaining;
            }
            int32_t advanced = -1;
            ESP_LOGI(ctx->tag, "rx_chunk inst=%u pos=%ld max=%u rx_size=%ld", (unsigned)instanceId,
                     (long)st->rx_buffer_pos, (unsigned)chunk_max, (long)st->rx_buffer_size);
            ssize_t got = ctx->rx_handler(instanceId, st->rx_buffer_pos, buf, chunk_max, &advanced);
            if (got < 0) return COAP_500_INTERNAL_SERVER_ERROR;
            if (got == 0) {
                lwm2m_data_encode_opaque(buf, 0, data);
                return COAP_205_CONTENT;
            }
            if ((size_t)got > sizeof(buf)) got = sizeof(buf);
            lwm2m_data_encode_opaque(buf, (size_t)got, data);
            st->rx_buffer_pos = (advanced >= 0) ? advanced : (st->rx_buffer_pos + (int32_t)got);
            notify_change(ctx, instanceId, ctx->res.rx_buffer_pos);
            return COAP_205_CONTENT;
        }

        uint8_t code = read_common(ctx, st, data);
        if (code != COAP_205_CONTENT) return code;
    }

    return COAP_205_CONTENT;
}

static uint8_t prv_write(lwm2m_context_t *contextP, uint16_t instanceId, int numData, lwm2m_data_t *dataArray, lwm2m_object_t *objectP, lwm2m_write_type_t writeType)
{
    (void)contextP;
    (void)writeType;
    iface_ctx_t *ctx = ctx_from_object(objectP);
    if (!ctx) return COAP_500_INTERNAL_SERVER_ERROR;

    iface_state_t *st = NULL;
    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (ctx->slots[i].used && ctx->slots[i].id == instanceId) {
            st = &ctx->slots[i];
            break;
        }
    }
    if (!st) return COAP_404_NOT_FOUND;

    for (int i = 0; i < numData; i++) {
        lwm2m_data_t *data = &dataArray[i];
        ESP_LOGI(ctx->tag, "write inst=%u res=%u type=%d len=%d", (unsigned)instanceId, data->id,
                 data->type, (int)data->value.asBuffer.length);
        if (data->id == ctx->res.type) {
            int64_t val = 0;
            if (1 != lwm2m_data_decode_int(data, &val)) return COAP_400_BAD_REQUEST;
            if (val < IFACE_TYPE_UART || val > IFACE_TYPE_MODBUS_UART) return COAP_400_BAD_REQUEST;
            st->type = (interface_type_t)val;
            esp_err_t err = iface_nvs_save_type(ctx, instanceId, st->type);
            if (err != ESP_OK) {
                ESP_LOGW(ctx->tag, "Failed to persist iface type inst=%u err=%s", (unsigned)instanceId, esp_err_to_name(err));
            }
            notify_change(ctx, instanceId, ctx->res.type);
            continue;
        }
        if (data->id == ctx->res.enabled) {
            bool val = false;
            if (1 != lwm2m_data_decode_bool(data, &val)) return COAP_400_BAD_REQUEST;

            bool is_bus_iface = (ctx->obj_id == LWM2M_OBJ_I2C || ctx->obj_id == LWM2M_OBJ_UART || ctx->obj_id == LWM2M_OBJ_RS485);
            if (!val && is_bus_iface && st->open_state && ctx->open_handler) {
                esp_err_t err = ctx->open_handler(instanceId, false);
                if (err != ESP_OK) {
                    snprintf(st->last_error, sizeof(st->last_error), "%s", esp_err_to_name(err));
                    return COAP_503_SERVICE_UNAVAILABLE;
                }
                st->open_state = false;
                notify_change(ctx, instanceId, ctx->res.open_state);
            }

            st->enabled = val;
            notify_change(ctx, instanceId, ctx->res.enabled);
            iface_handle_enabled_change(ctx, val);
            continue;
        }
        if (data->id == ctx->res.open_state) {
            bool val = false;
            if (1 != lwm2m_data_decode_bool(data, &val)) return COAP_400_BAD_REQUEST;

            bool is_bus_iface = (ctx->obj_id == LWM2M_OBJ_I2C || ctx->obj_id == LWM2M_OBJ_UART || ctx->obj_id == LWM2M_OBJ_RS485);
            if (is_bus_iface && val && !st->enabled) {
                snprintf(st->last_error, sizeof(st->last_error), "iface_disabled");
                return COAP_400_BAD_REQUEST;
            }

            ESP_LOGI(ctx->tag, "open_state inst=%u -> %s", (unsigned)instanceId, val ? "open" : "close");
            if (ctx->open_handler) {
                esp_err_t err = ctx->open_handler(instanceId, val);
                if (err != ESP_OK) {
                    snprintf(st->last_error, sizeof(st->last_error), "%s", esp_err_to_name(err));
                    return COAP_503_SERVICE_UNAVAILABLE;
                }
            }
            st->open_state = val;
            notify_change(ctx, instanceId, ctx->res.open_state);
            continue;
        }
        if (ctx->feat.baudrate && data->id == ctx->res.baudrate) {
            int64_t val = 0;
            if (1 != lwm2m_data_decode_int(data, &val) || val < 0) return COAP_400_BAD_REQUEST;
            st->baudrate = (uint32_t)val;
            ESP_LOGI(ctx->tag, "baudrate inst=%u -> %lu", (unsigned)instanceId, (unsigned long)st->baudrate);
            notify_change(ctx, instanceId, ctx->res.baudrate);
            continue;
        }
        if (ctx->feat.i2c_address && data->id == ctx->res.i2c_address) {
            int64_t val = 0;
            if (1 != lwm2m_data_decode_int(data, &val) || val < 0) return COAP_400_BAD_REQUEST;
            st->i2c_address = (uint32_t)val;
            notify_change(ctx, instanceId, ctx->res.i2c_address);
            continue;
        }
        if (ctx->feat.modbus_unit_id && data->id == ctx->res.modbus_unit_id) {
            int64_t val = 0;
            if (1 != lwm2m_data_decode_int(data, &val) || val < 0) return COAP_400_BAD_REQUEST;
            st->modbus_unit_id = (uint32_t)val;
            notify_change(ctx, instanceId, ctx->res.modbus_unit_id);
            continue;
        }
        if (data->id == ctx->res.mode) {
            int64_t val = 0;
            if (1 != lwm2m_data_decode_int(data, &val) || val < 0) return COAP_400_BAD_REQUEST;
            st->mode = (uint32_t)val;
            notify_change(ctx, instanceId, ctx->res.mode);
            continue;
        }
        if (data->id == ctx->res.stats_window_ms) {
            int64_t val = 0;
            if (1 != lwm2m_data_decode_int(data, &val) || val < 0) return COAP_400_BAD_REQUEST;
            st->stats_window_ms = (uint32_t)val;
            notify_change(ctx, instanceId, ctx->res.stats_window_ms);
            continue;
        }
        if (data->id == ctx->res.tx_payload) {
            if (!ctx->tx_handler) return COAP_503_SERVICE_UNAVAILABLE;
            if (data->type != LWM2M_TYPE_STRING && data->type != LWM2M_TYPE_OPAQUE) return COAP_400_BAD_REQUEST;
            ESP_LOGI(ctx->tag, "tx_payload inst=%u len=%d", (unsigned)instanceId, data->value.asBuffer.length);
            ssize_t sent = ctx->tx_handler(instanceId, data->value.asBuffer.buffer, data->value.asBuffer.length);
            if (sent < 0) {
                snprintf(st->last_error, sizeof(st->last_error), "tx_fail");
                return COAP_503_SERVICE_UNAVAILABLE;
            }
            st->tx_bytes += (uint32_t)sent;
            notify_change(ctx, instanceId, ctx->res.tx_bytes);
            continue;
        }
        if (data->id == ctx->res.rx_buffer_pos) {
            int64_t val = 0;
            if (1 != lwm2m_data_decode_int(data, &val)) return COAP_400_BAD_REQUEST;
            if (val < 0) val = 0;
            st->rx_buffer_pos = (int32_t)val;
            if (ctx->obj_id == LWM2M_OBJ_I2C && st->rx_buffer_pos == 0) {
                i2c_bridge_clear_pending_tx(instanceId);
            }
            notify_change(ctx, instanceId, ctx->res.rx_buffer_pos);
            continue;
        }
        if (data->id == ctx->res.rx_buffer_size) {
            int64_t val = 0;
            if (1 != lwm2m_data_decode_int(data, &val)) return COAP_400_BAD_REQUEST;
            ESP_LOGI(ctx->tag, "rx_buffer_size inst=%u -> %ld", (unsigned)instanceId, (long)val);
            st->rx_buffer_size = (int32_t)val;
            notify_change(ctx, instanceId, ctx->res.rx_buffer_size);
            continue;
        }
        if (ctx->feat.pins && data->id == ctx->res.tx_pin) {
            int64_t val = 0;
            if (1 != lwm2m_data_decode_int(data, &val)) return COAP_400_BAD_REQUEST;
            st->tx_pin = (int32_t)val;
            ESP_LOGI(ctx->tag, "tx_pin inst=%u -> %ld", (unsigned)instanceId, (long)st->tx_pin);
            notify_change(ctx, instanceId, ctx->res.tx_pin);
            continue;
        }
        if (ctx->feat.pins && data->id == ctx->res.rx_pin) {
            int64_t val = 0;
            if (1 != lwm2m_data_decode_int(data, &val)) return COAP_400_BAD_REQUEST;
            st->rx_pin = (int32_t)val;
            ESP_LOGI(ctx->tag, "rx_pin inst=%u -> %ld", (unsigned)instanceId, (long)st->rx_pin);
            notify_change(ctx, instanceId, ctx->res.rx_pin);
            continue;
        }
        if (ctx->feat.driver && data->id == ctx->res.driver) {
            if (data->type != LWM2M_TYPE_STRING && data->type != LWM2M_TYPE_OPAQUE) return COAP_400_BAD_REQUEST;

            size_t len = data->value.asBuffer.length;
            if (len > IFACE_DRIVER_MAX_LEN) return COAP_400_BAD_REQUEST;

            char *driver = (char *)malloc(len + 1);
            if (!driver) return COAP_500_INTERNAL_SERVER_ERROR;
            if (len > 0) memcpy(driver, data->value.asBuffer.buffer, len);
            driver[len] = '\0';

            esp_err_t err = iface_nvs_save_driver(ctx, instanceId, driver);
            if (err != ESP_OK) {
                free(driver);
                return COAP_500_INTERNAL_SERVER_ERROR;
            }

            err = iface_state_set_driver(st, driver);
            free(driver);
            if (err != ESP_OK) return COAP_500_INTERNAL_SERVER_ERROR;

            notify_change(ctx, instanceId, ctx->res.driver);
            continue;
        }
        if (ctx->feat.object_ids && data->id == ctx->res.object_ids) {
            if (data->type != LWM2M_TYPE_STRING && data->type != LWM2M_TYPE_OPAQUE) return COAP_400_BAD_REQUEST;

            size_t len = data->value.asBuffer.length;
            if (len > IFACE_OBJECT_IDS_MAX_LEN) return COAP_400_BAD_REQUEST;

            char *object_ids = (char *)malloc(len + 1);
            if (!object_ids) return COAP_500_INTERNAL_SERVER_ERROR;
            if (len > 0) memcpy(object_ids, data->value.asBuffer.buffer, len);
            object_ids[len] = '\0';

            if (!iface_validate_object_ids_string(object_ids)) {
                free(object_ids);
                return COAP_400_BAD_REQUEST;
            }

            esp_err_t err = iface_nvs_save_object_ids(ctx, instanceId, object_ids);
            if (err != ESP_OK) {
                free(object_ids);
                return COAP_500_INTERNAL_SERVER_ERROR;
            }

            err = iface_state_set_object_ids(st, object_ids);
            free(object_ids);
            if (err != ESP_OK) return COAP_500_INTERNAL_SERVER_ERROR;

            notify_change(ctx, instanceId, ctx->res.object_ids);
            continue;
        }

        return COAP_405_METHOD_NOT_ALLOWED;
    }

    return COAP_204_CHANGED;
}

static uint8_t prv_execute(lwm2m_context_t *contextP, uint16_t instanceId, uint16_t resourceId, uint8_t *buffer, int length, lwm2m_object_t *objectP)
{
    (void)contextP;
    (void)buffer;
    (void)length;
    iface_ctx_t *ctx = ctx_from_object(objectP);
    if (!ctx) return COAP_500_INTERNAL_SERVER_ERROR;

    iface_state_t *st = NULL;
    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (ctx->slots[i].used && ctx->slots[i].id == instanceId) {
            st = &ctx->slots[i];
            break;
        }
    }
    if (!st) return COAP_404_NOT_FOUND;

    if (resourceId == ctx->res.reset_counters) {
        st->tx_bytes = 0;
        st->rx_bytes = 0;
        st->error_count = 0;
        st->last_error[0] = '\0';
        notify_change(ctx, instanceId, ctx->res.tx_bytes);
        notify_change(ctx, instanceId, ctx->res.rx_bytes);
        notify_change(ctx, instanceId, ctx->res.error_count);
        notify_change(ctx, instanceId, ctx->res.last_error);
        return COAP_204_CHANGED;
    }

    return COAP_405_METHOD_NOT_ALLOWED;
}

lwm2m_object_t *iface_get_object(iface_ctx_t *ctx)
{
    if (!ctx) return NULL;
    if (ctx->obj) return ctx->obj;

    lwm2m_object_t *obj = (lwm2m_object_t *)lwm2m_malloc(sizeof(lwm2m_object_t));
    if (!obj) return NULL;
    memset(obj, 0, sizeof(lwm2m_object_t));
    obj->objID = ctx->obj_id;
    obj->readFunc = prv_read;
    obj->writeFunc = prv_write;
    obj->executeFunc = prv_execute;
    obj->userData = ctx;

    ctx->obj = obj;
    iface_register_ctx(ctx);
    return ctx->obj;
}

esp_err_t iface_update_counters(iface_ctx_t *ctx, uint16_t instance_id, uint32_t tx_bytes, uint32_t rx_bytes,
                                uint32_t error_count, uint32_t tx_rate, uint32_t rx_rate, const char *last_error)
{
    if (!ctx || !iface_get_object(ctx)) return ESP_FAIL;
    iface_state_t *st = NULL;
    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (ctx->slots[i].used && ctx->slots[i].id == instance_id) {
            st = &ctx->slots[i];
            break;
        }
    }
    if (!st) return ESP_ERR_INVALID_STATE;

    st->tx_bytes = tx_bytes;
    st->rx_bytes = rx_bytes;
    st->error_count = error_count;
    st->tx_rate = tx_rate;
    st->rx_rate = rx_rate;
    if (last_error) {
        strncpy(st->last_error, last_error, sizeof(st->last_error) - 1);
        st->last_error[sizeof(st->last_error) - 1] = '\0';
    }

    notify_change(ctx, instance_id, ctx->res.tx_bytes);
    notify_change(ctx, instance_id, ctx->res.rx_bytes);
    notify_change(ctx, instance_id, ctx->res.error_count);
    notify_change(ctx, instance_id, ctx->res.tx_rate);
    notify_change(ctx, instance_id, ctx->res.rx_rate);
    if (last_error) notify_change(ctx, instance_id, ctx->res.last_error);
    return ESP_OK;
}

esp_err_t iface_set_rx_cursor(iface_ctx_t *ctx, uint16_t instance_id, int32_t pos)
{
    if (!ctx || !iface_get_object(ctx)) return ESP_FAIL;
    iface_state_t *st = NULL;
    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (ctx->slots[i].used && ctx->slots[i].id == instance_id) {
            st = &ctx->slots[i];
            break;
        }
    }
    if (!st) return ESP_ERR_INVALID_STATE;
    if (pos < 0) pos = 0;
    st->rx_buffer_pos = pos;
    notify_change(ctx, instance_id, ctx->res.rx_buffer_pos);
    return ESP_OK;
}

esp_err_t iface_set_rx_buffer_size(iface_ctx_t *ctx, uint16_t instance_id, int32_t size)
{
    if (!ctx || !iface_get_object(ctx)) return ESP_FAIL;
    iface_state_t *st = NULL;
    for (size_t i = 0; i < MAX_IFACE_INSTANCES; i++) {
        if (ctx->slots[i].used && ctx->slots[i].id == instance_id) {
            st = &ctx->slots[i];
            break;
        }
    }
    if (!st) return ESP_ERR_INVALID_STATE;
    st->rx_buffer_size = size;
    notify_change(ctx, instance_id, ctx->res.rx_buffer_size);
    return ESP_OK;
}

void iface_set_handlers(iface_ctx_t *ctx, interface_tx_handler_t tx_handler, interface_rx_handler_t rx_handler,
                       interface_open_handler_t open_handler)
{
    if (!ctx) return;
    ctx->tx_handler = tx_handler;
    ctx->rx_handler = rx_handler;
    ctx->open_handler = open_handler;
    ESP_LOGI(ctx->tag, "handlers set: tx=%p rx=%p open=%p", (void *)tx_handler, (void *)rx_handler, (void *)open_handler);
}
