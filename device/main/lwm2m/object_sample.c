#include "object_sample.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwm2m_client.h"
#include "prov.h"
#include "sampling.h"
#include "object_script.h"
#include "nvs.h"

static const char *TAG = "lwm2m_sample";

#define SAMPLE_NVS_NAMESPACE   "sample"
#define SAMPLE_NVS_KEY_VER     "ver"
#define SAMPLE_NVS_KEY_EN      "en"
#define SAMPLE_NVS_KEY_SLEEP   "sleep"
#define SAMPLE_NVS_KEY_SLEEP_MODE "sleep_mode"
#define SAMPLE_NVS_KEY_S_RATE  "s_rate"
#define SAMPLE_NVS_KEY_R_RATE  "r_rate"
#define SAMPLE_NVS_KEY_SEND_ACK_TO "send_ack_to"
#define SAMPLE_NVS_KEY_SEND_RETRY_DELAY "snd_retry_dly"
#define SAMPLE_NVS_KEY_SEND_RETRY_CNT "send_retry_cnt"
#define SAMPLE_NVS_KEY_S_U2C   "s_u2c"
#define SAMPLE_NVS_KEY_S_RS    "s_rs485"
#define SAMPLE_NVS_KEY_SCRIPT_SIZE "script_sz"
#define SAMPLE_NVS_KEY_SCRIPT_BLOB "script_bin"
#define SAMPLE_RESTART_PROV_GRACE_MS 8000
#define SAMPLE_HALOW_RESTART_SHUTDOWN_TIMEOUT_MS 4000
#define SAMPLE_HALOW_RESTART_SHUTDOWN_TASK_STACK 4096

typedef struct {
    uint32_t config_version;
    bool sampling_enabled;
    char sleep_mode[6];
    uint32_t sample_rate;
    uint32_t report_rate;
    uint32_t send_ack_timeout_ms;
    uint32_t send_retry_delay_ms;
    uint32_t send_retry_count;
    char sensor_uart_i2c[32];
    char sensor_rs485[32];
} sample_state_t;

static sample_state_t s_sample = {
    .config_version = 1,
    .sampling_enabled = true,
    .sleep_mode = "no",
    .sample_rate = 60,
    .report_rate = 300,
    .send_ack_timeout_ms = 5000,
    .send_retry_delay_ms = 1000,
    .send_retry_count = 3,
    .sensor_uart_i2c = "",
    .sensor_rs485 = "",
};
static uint8_t *s_sample_data_lines[SAMPLE_DATA_FIELD_MAX_LINES] = {0};
static size_t s_sample_data_line_lens[SAMPLE_DATA_FIELD_MAX_LINES] = {0};
static size_t s_sample_data_line_count = 0;
static void notify_change(uint16_t resource_id);
static void sample_data_field_clear_all(void);
static esp_err_t sample_data_field_store_line(size_t index, const uint8_t *buf, size_t len);
static bool s_sample_restart_scheduled = false;

static bool sample_extract_script_id_from_selector(const char *selector, uint16_t *out_script_id)
{
    const char *cursor;
    const char *end;
    const char *value_start;
    uint32_t parsed = 0;

    if (!selector || !out_script_id)
    {
        return false;
    }

    cursor = selector;
    while (*cursor == ' ' || *cursor == '\t')
    {
        cursor++;
    }
    if (*cursor == '\0')
    {
        return false;
    }

    if ((cursor[0] == 'n' || cursor[0] == 'N') &&
        (cursor[1] == 'o' || cursor[1] == 'O') &&
        (cursor[2] == 'n' || cursor[2] == 'N') &&
        (cursor[3] == 'e' || cursor[3] == 'E') &&
        (cursor[4] == '\0' || cursor[4] == ',' || cursor[4] == ' ' || cursor[4] == '\t'))
    {
        return false;
    }

    end = cursor;
    while (*end != '\0' && *end != ',')
    {
        end++;
    }

    value_start = cursor;
    for (const char *p = cursor; p < end; p++)
    {
        if (*p == ':')
        {
            value_start = p + 1;
        }
    }

    while (value_start < end && (*value_start == ' ' || *value_start == '\t'))
    {
        value_start++;
    }

    if (value_start >= end || *value_start < '0' || *value_start > '9')
    {
        return false;
    }

    for (const char *p = value_start; p < end; p++)
    {
        if (*p < '0' || *p > '9')
        {
            break;
        }
        parsed = (parsed * 10u) + (uint32_t)(*p - '0');
        if (parsed > UINT16_MAX)
        {
            return false;
        }
    }

    if (parsed == 0)
    {
        return false;
    }

    *out_script_id = (uint16_t)parsed;
    return true;
}

static int sample_interface_script_missing(const char *selector)
{
    uint16_t script_id = 0;

    if (!sample_extract_script_id_from_selector(selector, &script_id))
    {
        return 0;
    }

    return lwm2m_script_has_nonempty_instance(script_id) ? 0 : 1;
}

#ifdef CONFIG_ENABLE_MM_HALOW
static void sample_halow_restart_shutdown_task(void *arg)
{
    TaskHandle_t waiter = (TaskHandle_t)arg;
    wifi_prov_shutdown_halow_for_restart();
    if (waiter != NULL) {
        xTaskNotifyGive(waiter);
    }
    vTaskDelete(NULL);
}

static void sample_shutdown_halow_with_guard(uint32_t timeout_ms)
{
    TaskHandle_t waiter = xTaskGetCurrentTaskHandle();
    TaskHandle_t shutdown_task = NULL;
    TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);
    if (wait_ticks == 0) {
        wait_ticks = 1;
    }

    BaseType_t created = xTaskCreate(sample_halow_restart_shutdown_task,
                                     "halow_restart_off",
                                     SAMPLE_HALOW_RESTART_SHUTDOWN_TASK_STACK,
                                     (void *)waiter,
                                     5,
                                     &shutdown_task);
    if (created != pdPASS) {
        ESP_LOGW(TAG,
                 "Sample restart: failed to create HaLow shutdown task, skipping shutdown guard");
        return;
    }

    uint32_t notified = ulTaskNotifyTake(pdTRUE, wait_ticks);
    if (notified == 0) {
        ESP_LOGW(TAG,
                 "Sample restart: HaLow shutdown timed out after %u ms, forcing reboot",
                 (unsigned)timeout_ms);
    } else {
        ESP_LOGI(TAG, "Sample restart: HaLow shutdown completed");
    }
}
#endif

static void sample_restart_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Sample config changed, coordinating restart");

    /* Allow write response/logs to flush before reboot sequence. */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Wait briefly for in-flight LwM2M traffic to drain. */
    const TickType_t wait_step = pdMS_TO_TICKS(100);
    const TickType_t wait_timeout = pdMS_TO_TICKS(5000);
    TickType_t waited = 0;
    while (!lwm2m_is_idle_for_sleep() && waited < wait_timeout) {
        vTaskDelay(wait_step);
        waited += wait_step;
    }

    if (wifi_prov_service_active() || wifi_prov_recent_success(SAMPLE_RESTART_PROV_GRACE_MS)) {
        ESP_LOGI(TAG,
                 "Sample restart: provisioning in-progress/recent, delaying reboot by %u ms",
                 (unsigned)SAMPLE_RESTART_PROV_GRACE_MS);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_RESTART_PROV_GRACE_MS));
    }

#ifdef CONFIG_ENABLE_MM_HALOW
    if (wifi_prov_get_connection_type() == LWM2M_CONN_TYPE_HALOW) {
        ESP_LOGI(TAG, "Sample restart: shutting down HaLow before reboot");
        sample_shutdown_halow_with_guard(SAMPLE_HALOW_RESTART_SHUTDOWN_TIMEOUT_MS);
    }
#endif

    ESP_LOGI(TAG, "Sample config changed, rebooting now");
    esp_restart();
}

static void schedule_immediate_sample_restart(void)
{
    if (s_sample_restart_scheduled) {
        return;
    }

    if (xTaskCreate(sample_restart_task, "sample_restart", 3072, NULL, 5, NULL) == pdPASS) {
        s_sample_restart_scheduled = true;
    } else {
        ESP_LOGW(TAG, "Failed to schedule immediate reboot after sample config update");
    }
}

static lwm2m_object_t *s_sample_obj = NULL;
static bool s_sample_nvs_loaded = false;

enum {
    SAMPLE_CHANGED_VERSION = 1 << 0,
    SAMPLE_CHANGED_ENABLED = 1 << 1,
    SAMPLE_CHANGED_RATE = 1 << 2,
    SAMPLE_CHANGED_REPORT_RATE = 1 << 3,
    SAMPLE_CHANGED_SLEEP_MODE = 1 << 4,
    SAMPLE_CHANGED_SENSOR_UART_I2C = 1 << 5,
    SAMPLE_CHANGED_SENSOR_RS485 = 1 << 6,
    SAMPLE_CHANGED_SEND_ACK_TIMEOUT = 1 << 7,
    SAMPLE_CHANGED_SEND_RETRY_DELAY = 1 << 8,
    SAMPLE_CHANGED_SEND_RETRY_COUNT = 1 << 9,
};

static esp_err_t sample_nvs_erase_legacy_script_locked(nvs_handle_t nvs)
{
    esp_err_t err = nvs_erase_key(nvs, SAMPLE_NVS_KEY_SCRIPT_SIZE);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    err = nvs_erase_key(nvs, SAMPLE_NVS_KEY_SCRIPT_BLOB);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    return ESP_OK;
}

static void notify_change(uint16_t resource_id)
{
    lwm2m_context_t *ctx = get_lwm2m_context();
    if (!ctx || ctx->state != STATE_READY) return;

    lwm2m_uri_t uri = {
        .objectId = LWM2M_OBJ_SAMPLE,
        .instanceId = 0,
        .resourceId = resource_id,
    };
    lwm2m_resource_value_changed(ctx, &uri);
}

static bool parse_sleep_mode(const uint8_t *buf, size_t len, char *out_mode, size_t out_sz)
{
    if (!buf || len == 0 || len + 1 > out_sz) return false;

    char tmp[8] = {0};
    if (len >= sizeof(tmp)) return false;

    memcpy(tmp, buf, len);
    tmp[len] = '\0';

    for (size_t i = 0; i < len; i++) {
        if (tmp[i] >= 'A' && tmp[i] <= 'Z') tmp[i] = (char)(tmp[i] - 'A' + 'a');
    }

    if (strcmp(tmp, "no") == 0 || strcmp(tmp, "light") == 0 || strcmp(tmp, "deep") == 0) {
        strncpy(out_mode, tmp, out_sz - 1);
        out_mode[out_sz - 1] = '\0';
        return true;
    }

    return false;
}

static uint8_t sleep_mode_to_u8(const char *mode)
{
    if (!mode) return 0;
    if (strcmp(mode, "light") == 0) return 1;
    if (strcmp(mode, "deep") == 0) return 2;
    return 0;
}

static const char *sleep_mode_from_u8(uint8_t mode)
{
    switch (mode) {
        case 1:
            return "light";
        case 2:
            return "deep";
        default:
            return "no";
    }
}

static bool parse_sensor_name(const uint8_t *buf, size_t len, char *out_name, size_t out_sz)
{
    if (!buf || !out_name || out_sz == 0) return false;
    if (len + 1 > out_sz) return false;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = buf[i];
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }

    memcpy(out_name, buf, len);
    out_name[len] = '\0';
    return true;
}

static const char *json_find_key_value(const char *json, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static bool json_get_u32(const char *json, const char *key, uint32_t *out)
{
    const char *p = json_find_key_value(json, key);
    char *endptr = NULL;
    unsigned long v;
    if (!p || !out) return false;
    v = strtoul(p, &endptr, 10);
    if (endptr == p || v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

static bool json_get_bool(const char *json, const char *key, bool *out)
{
    const char *p = json_find_key_value(json, key);
    if (!p || !out) return false;
    if (strncmp(p, "true", 4) == 0 || *p == '1') {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0 || *p == '0') {
        *out = false;
        return true;
    }
    return false;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_sz)
{
    const char *p = json_find_key_value(json, key);
    const char *q;
    size_t len;
    if (!p || !out || out_sz == 0) return false;
    if (*p != '"') return false;
    p++;
    q = strchr(p, '"');
    if (!q) return false;
    len = (size_t)(q - p);
    if (len + 1 > out_sz) return false;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static esp_err_t sample_nvs_save_state(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(SAMPLE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_u32(nvs, SAMPLE_NVS_KEY_VER, s_sample.config_version);
    if (err == ESP_OK) err = nvs_set_u8(nvs, SAMPLE_NVS_KEY_EN, s_sample.sampling_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(nvs, SAMPLE_NVS_KEY_SLEEP, s_sample.sleep_mode);
    if (err == ESP_OK) err = nvs_set_u8(nvs, SAMPLE_NVS_KEY_SLEEP_MODE, sleep_mode_to_u8(s_sample.sleep_mode));
    if (err == ESP_OK) err = nvs_set_u32(nvs, SAMPLE_NVS_KEY_S_RATE, s_sample.sample_rate);
    if (err == ESP_OK) err = nvs_set_u32(nvs, SAMPLE_NVS_KEY_R_RATE, s_sample.report_rate);
    if (err == ESP_OK) err = nvs_set_u32(nvs, SAMPLE_NVS_KEY_SEND_ACK_TO, s_sample.send_ack_timeout_ms);
    if (err == ESP_OK) err = nvs_set_u32(nvs, SAMPLE_NVS_KEY_SEND_RETRY_DELAY, s_sample.send_retry_delay_ms);
    if (err == ESP_OK) err = nvs_set_u32(nvs, SAMPLE_NVS_KEY_SEND_RETRY_CNT, s_sample.send_retry_count);
    if (err == ESP_OK) err = nvs_set_str(nvs, SAMPLE_NVS_KEY_S_U2C, s_sample.sensor_uart_i2c);
    if (err == ESP_OK) err = nvs_set_str(nvs, SAMPLE_NVS_KEY_S_RS, s_sample.sensor_rs485);
    if (err == ESP_OK) err = sample_nvs_erase_legacy_script_locked(nvs);
    if (err == ESP_OK) err = nvs_commit(nvs);

    nvs_close(nvs);
    return err;
}

static void sample_nvs_load_state(void)
{
    if (s_sample_nvs_loaded) return;
    s_sample_nvs_loaded = true;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(SAMPLE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        }
        return;
    }

    uint32_t val = 0;
    if (nvs_get_u32(nvs, SAMPLE_NVS_KEY_VER, &val) == ESP_OK && val > 0) {
        s_sample.config_version = val;
    }

    uint8_t en = 0;
    if (nvs_get_u8(nvs, SAMPLE_NVS_KEY_EN, &en) == ESP_OK) {
        s_sample.sampling_enabled = (en != 0);
    }

    uint8_t sleep_mode_u8 = 0;
    err = nvs_get_u8(nvs, SAMPLE_NVS_KEY_SLEEP_MODE, &sleep_mode_u8);
    if (err == ESP_OK) {
        strncpy(s_sample.sleep_mode, sleep_mode_from_u8(sleep_mode_u8), sizeof(s_sample.sleep_mode) - 1);
        s_sample.sleep_mode[sizeof(s_sample.sleep_mode) - 1] = '\0';
    } else {
        char sleep_buf[sizeof(s_sample.sleep_mode)] = {0};
        size_t sleep_len = sizeof(sleep_buf);
        err = nvs_get_str(nvs, SAMPLE_NVS_KEY_SLEEP, sleep_buf, &sleep_len);
        if (err == ESP_OK) {
            char parsed[sizeof(s_sample.sleep_mode)] = {0};
            size_t parsed_len = strnlen(sleep_buf, sizeof(sleep_buf));
            if (parse_sleep_mode((const uint8_t *)sleep_buf, parsed_len, parsed, sizeof(parsed))) {
                strncpy(s_sample.sleep_mode, parsed, sizeof(s_sample.sleep_mode) - 1);
                s_sample.sleep_mode[sizeof(s_sample.sleep_mode) - 1] = '\0';
            }
        }
    }

    err = nvs_get_u32(nvs, SAMPLE_NVS_KEY_S_RATE, &val);
    if (err == ESP_OK && val > 0) {
        s_sample.sample_rate = val;
    }

    err = nvs_get_u32(nvs, SAMPLE_NVS_KEY_R_RATE, &val);
    if (err == ESP_OK && val > 0) {
        s_sample.report_rate = val;
    }

    err = nvs_get_u32(nvs, SAMPLE_NVS_KEY_SEND_ACK_TO, &val);
    if (err == ESP_OK && val > 0) {
        s_sample.send_ack_timeout_ms = val;
    }

    err = nvs_get_u32(nvs, SAMPLE_NVS_KEY_SEND_RETRY_DELAY, &val);
    if (err == ESP_OK && val > 0) {
        s_sample.send_retry_delay_ms = val;
    }

    err = nvs_get_u32(nvs, SAMPLE_NVS_KEY_SEND_RETRY_CNT, &val);
    if (err == ESP_OK && val > 0) {
        s_sample.send_retry_count = val;
    }

    char sensor_u2c[sizeof(s_sample.sensor_uart_i2c)] = {0};
    size_t sensor_u2c_len = sizeof(sensor_u2c);
    if (nvs_get_str(nvs, SAMPLE_NVS_KEY_S_U2C, sensor_u2c, &sensor_u2c_len) == ESP_OK) {
        strncpy(s_sample.sensor_uart_i2c, sensor_u2c, sizeof(s_sample.sensor_uart_i2c) - 1);
        s_sample.sensor_uart_i2c[sizeof(s_sample.sensor_uart_i2c) - 1] = '\0';
    }

    char sensor_rs[sizeof(s_sample.sensor_rs485)] = {0};
    size_t sensor_rs_len = sizeof(sensor_rs);
    if (nvs_get_str(nvs, SAMPLE_NVS_KEY_S_RS, sensor_rs, &sensor_rs_len) == ESP_OK) {
        strncpy(s_sample.sensor_rs485, sensor_rs, sizeof(s_sample.sensor_rs485) - 1);
        s_sample.sensor_rs485[sizeof(s_sample.sensor_rs485) - 1] = '\0';
    }

    err = sample_nvs_erase_legacy_script_locked(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clean legacy sample script fields from NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs);
    ESP_LOGI(TAG, "Loaded sample state from NVS: ver=%lu enabled=%d sleep=%s sample_rate=%lu report_rate=%lu ack_to=%lu retry_delay=%lu retry_count=%lu u2c=%s rs485=%s script_size=%lu",
             (unsigned long)s_sample.config_version, s_sample.sampling_enabled ? 1 : 0, s_sample.sleep_mode,
             (unsigned long)s_sample.sample_rate, (unsigned long)s_sample.report_rate,
             (unsigned long)s_sample.send_ack_timeout_ms, (unsigned long)s_sample.send_retry_delay_ms, (unsigned long)s_sample.send_retry_count,
             s_sample.sensor_uart_i2c, s_sample.sensor_rs485,
             0UL);
}

static uint8_t commit_state(const sample_state_t *next, uint32_t changed_mask)
{
    sample_state_t prev = s_sample;
    s_sample = *next;
    esp_err_t persist_err = sample_nvs_save_state();
    if (persist_err != ESP_OK) {
        s_sample = prev;
        ESP_LOGW(TAG, "Failed to persist sample config to NVS: %s", esp_err_to_name(persist_err));
        ESP_LOGW(TAG, "Persist context: ver=%lu enabled=%d sleep=%s sample_rate=%lu report_rate=%lu ack_to=%lu retry_delay=%lu retry_count=%lu u2c='%s' rs485='%s' changed_mask=0x%lx",
                 (unsigned long)next->config_version,
                 next->sampling_enabled ? 1 : 0,
                 next->sleep_mode,
                 (unsigned long)next->sample_rate,
                 (unsigned long)next->report_rate,
                 (unsigned long)next->send_ack_timeout_ms,
                 (unsigned long)next->send_retry_delay_ms,
                 (unsigned long)next->send_retry_count,
                 next->sensor_uart_i2c,
                 next->sensor_rs485,
                 (unsigned long)changed_mask);
        return COAP_500_INTERNAL_SERVER_ERROR;
    }

    if (changed_mask & SAMPLE_CHANGED_VERSION) notify_change(RES_SAMPLE_CONFIG_VERSION);
    if (changed_mask & SAMPLE_CHANGED_ENABLED) notify_change(RES_SAMPLE_ENABLED);
    if (changed_mask & SAMPLE_CHANGED_RATE) notify_change(RES_SAMPLE_RATE);
    if (changed_mask & SAMPLE_CHANGED_REPORT_RATE) notify_change(RES_SAMPLE_REPORT_RATE);
    if (changed_mask & SAMPLE_CHANGED_SLEEP_MODE) notify_change(RES_SAMPLE_SLEEP_MODE);
    if (changed_mask & SAMPLE_CHANGED_SEND_ACK_TIMEOUT) notify_change(RES_SAMPLE_SEND_ACK_TIMEOUT_MS);
    if (changed_mask & SAMPLE_CHANGED_SEND_RETRY_DELAY) notify_change(RES_SAMPLE_SEND_RETRY_DELAY_MS);
    if (changed_mask & SAMPLE_CHANGED_SEND_RETRY_COUNT) notify_change(RES_SAMPLE_SEND_RETRY_COUNT);
    if (changed_mask & SAMPLE_CHANGED_SENSOR_UART_I2C) notify_change(RES_SAMPLE_SENSOR_UART_I2C);
    if (changed_mask & SAMPLE_CHANGED_SENSOR_RS485) notify_change(RES_SAMPLE_SENSOR_RS485);

    sample_settings_load_from_nvs_to_rtc();
    schedule_immediate_sample_restart();

    return COAP_204_CHANGED;
}

static uint8_t apply_json_config(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0 || len >= 512) return COAP_400_BAD_REQUEST;

    char json[512] = {0};
    memcpy(json, buf, len);
    json[len] = '\0';

    sample_state_t next = s_sample;
    uint32_t changed = 0;

    uint32_t v = 0;
    bool b = false;
    char str[32] = {0};

    if (json_get_u32(json, "version", &v) && v > 0 && v != next.config_version) {
        next.config_version = v;
        changed |= SAMPLE_CHANGED_VERSION;
    }
    if (json_get_bool(json, "sampling_enabled", &b) && b != next.sampling_enabled) {
        next.sampling_enabled = b;
        changed |= SAMPLE_CHANGED_ENABLED;
    }
    if (json_get_u32(json, "sampling_rate", &v) && v > 0 && v != next.sample_rate) {
        next.sample_rate = v;
        changed |= SAMPLE_CHANGED_RATE;
    }
    if (json_get_u32(json, "report_rate", &v) && v > 0 && v != next.report_rate) {
        next.report_rate = v;
        changed |= SAMPLE_CHANGED_REPORT_RATE;
    }
    if (json_get_u32(json, "send_ack_timeout_ms", &v) && v > 0 && v != next.send_ack_timeout_ms) {
        next.send_ack_timeout_ms = v;
        changed |= SAMPLE_CHANGED_SEND_ACK_TIMEOUT;
    }
    if (json_get_u32(json, "send_retry_delay_ms", &v) && v > 0 && v != next.send_retry_delay_ms) {
        next.send_retry_delay_ms = v;
        changed |= SAMPLE_CHANGED_SEND_RETRY_DELAY;
    }
    if (json_get_u32(json, "send_retry_count", &v) && v > 0 && v != next.send_retry_count) {
        next.send_retry_count = v;
        changed |= SAMPLE_CHANGED_SEND_RETRY_COUNT;
    }
    if (json_get_string(json, "sleep_mode", str, sizeof(str))) {
        char parsed_sleep[sizeof(next.sleep_mode)] = {0};
        if (!parse_sleep_mode((const uint8_t *)str, strlen(str), parsed_sleep, sizeof(parsed_sleep))) {
            return COAP_400_BAD_REQUEST;
        }
        if (strcmp(parsed_sleep, next.sleep_mode) != 0) {
            strncpy(next.sleep_mode, parsed_sleep, sizeof(next.sleep_mode) - 1);
            next.sleep_mode[sizeof(next.sleep_mode) - 1] = '\0';
            changed |= SAMPLE_CHANGED_SLEEP_MODE;
        }
    }
    if (json_get_string(json, "sensor_uart_i2c", str, sizeof(str))) {
        char parsed_sensor[sizeof(next.sensor_uart_i2c)] = {0};
        if (!parse_sensor_name((const uint8_t *)str, strlen(str), parsed_sensor, sizeof(parsed_sensor))) {
            return COAP_400_BAD_REQUEST;
        }
        if (strcmp(parsed_sensor, next.sensor_uart_i2c) != 0) {
            strncpy(next.sensor_uart_i2c, parsed_sensor, sizeof(next.sensor_uart_i2c) - 1);
            next.sensor_uart_i2c[sizeof(next.sensor_uart_i2c) - 1] = '\0';
            changed |= SAMPLE_CHANGED_SENSOR_UART_I2C;
        }
    }
    if (json_get_string(json, "sensor_rs485", str, sizeof(str))) {
        char parsed_sensor[sizeof(next.sensor_rs485)] = {0};
        if (!parse_sensor_name((const uint8_t *)str, strlen(str), parsed_sensor, sizeof(parsed_sensor))) {
            return COAP_400_BAD_REQUEST;
        }
        if (strcmp(parsed_sensor, next.sensor_rs485) != 0) {
            strncpy(next.sensor_rs485, parsed_sensor, sizeof(next.sensor_rs485) - 1);
            next.sensor_rs485[sizeof(next.sensor_rs485) - 1] = '\0';
            changed |= SAMPLE_CHANGED_SENSOR_RS485;
        }
    }

    if (changed == 0) return COAP_204_CHANGED;
    return commit_state(&next, changed);
}

static uint8_t set_u32_field(uint16_t resource_id, int64_t value)
{
    if (value <= 0 || value > UINT32_MAX) return COAP_400_BAD_REQUEST;

    sample_state_t next = s_sample;
    uint32_t changed = 0;
    uint32_t casted = (uint32_t)value;

    switch (resource_id) {
        case RES_SAMPLE_CONFIG_VERSION:
            if (next.config_version == casted) return COAP_204_CHANGED;
            next.config_version = casted;
            changed = SAMPLE_CHANGED_VERSION;
            break;
        case RES_SAMPLE_RATE:
            if (next.sample_rate == casted) return COAP_204_CHANGED;
            next.sample_rate = casted;
            changed = SAMPLE_CHANGED_RATE;
            break;
        case RES_SAMPLE_REPORT_RATE:
            if (next.report_rate == casted) return COAP_204_CHANGED;
            next.report_rate = casted;
            changed = SAMPLE_CHANGED_REPORT_RATE;
            break;
        case RES_SAMPLE_SEND_ACK_TIMEOUT_MS:
            if (next.send_ack_timeout_ms == casted) return COAP_204_CHANGED;
            next.send_ack_timeout_ms = casted;
            changed = SAMPLE_CHANGED_SEND_ACK_TIMEOUT;
            break;
        case RES_SAMPLE_SEND_RETRY_DELAY_MS:
            if (next.send_retry_delay_ms == casted) return COAP_204_CHANGED;
            next.send_retry_delay_ms = casted;
            changed = SAMPLE_CHANGED_SEND_RETRY_DELAY;
            break;
        case RES_SAMPLE_SEND_RETRY_COUNT:
            if (casted > 10) {
                return COAP_400_BAD_REQUEST;
            }
            if (next.send_retry_count == casted) return COAP_204_CHANGED;
            next.send_retry_count = casted;
            changed = SAMPLE_CHANGED_SEND_RETRY_COUNT;
            break;
        default:
            return COAP_404_NOT_FOUND;
    }

    return commit_state(&next, changed);
}

static uint8_t set_enabled(const lwm2m_data_t *data)
{
    bool enabled;
    int64_t int_val = 0;

    if (lwm2m_data_decode_bool((lwm2m_data_t *)data, &enabled) != 1) {
        if (lwm2m_data_decode_int((lwm2m_data_t *)data, &int_val) != 1) return COAP_400_BAD_REQUEST;
        enabled = (int_val != 0);
    }

    if (s_sample.sampling_enabled == enabled) return COAP_204_CHANGED;

    sample_state_t next = s_sample;
    next.sampling_enabled = enabled;
    return commit_state(&next, SAMPLE_CHANGED_ENABLED);
}

static uint8_t set_sleep_mode(const uint8_t *buf, size_t len)
{
    char new_mode[sizeof(s_sample.sleep_mode)] = {0};
    if (!parse_sleep_mode(buf, len, new_mode, sizeof(new_mode))) return COAP_400_BAD_REQUEST;

    if (strcmp(new_mode, s_sample.sleep_mode) == 0) return COAP_204_CHANGED;

    sample_state_t next = s_sample;
    strncpy(next.sleep_mode, new_mode, sizeof(next.sleep_mode) - 1);
    next.sleep_mode[sizeof(next.sleep_mode) - 1] = '\0';
    return commit_state(&next, SAMPLE_CHANGED_SLEEP_MODE);
}

static uint8_t set_sensor(uint16_t resource_id, const uint8_t *buf, size_t len)
{
    char parsed[32] = {0};
    sample_state_t next = s_sample;
    uint32_t changed = 0;

    if (!parse_sensor_name(buf, len, parsed, sizeof(parsed))) return COAP_400_BAD_REQUEST;

    if (resource_id == RES_SAMPLE_SENSOR_UART_I2C) {
        if (strcmp(parsed, s_sample.sensor_uart_i2c) == 0) return COAP_204_CHANGED;
        strncpy(next.sensor_uart_i2c, parsed, sizeof(next.sensor_uart_i2c) - 1);
        next.sensor_uart_i2c[sizeof(next.sensor_uart_i2c) - 1] = '\0';
        changed |= SAMPLE_CHANGED_SENSOR_UART_I2C;
    }
    else if (resource_id == RES_SAMPLE_SENSOR_RS485) {
        if (strcmp(parsed, s_sample.sensor_rs485) == 0) return COAP_204_CHANGED;
        strncpy(next.sensor_rs485, parsed, sizeof(next.sensor_rs485) - 1);
        next.sensor_rs485[sizeof(next.sensor_rs485) - 1] = '\0';
        changed |= SAMPLE_CHANGED_SENSOR_RS485;
    } else {
        return COAP_404_NOT_FOUND;
    }

    return commit_state(&next, changed);
}

static uint8_t set_data_field(const uint8_t *buf, size_t len)
{
    if (!buf || len > SAMPLE_DATA_FIELD_LINE_MAX_LEN) {
        return COAP_400_BAD_REQUEST;
    }

    if (s_sample_data_line_count == 1 &&
        s_sample_data_line_lens[0] == len &&
        (len == 0 || (s_sample_data_lines[0] && memcmp(s_sample_data_lines[0], buf, len) == 0))) {
        return COAP_204_CHANGED;
    }

    sample_data_field_clear_all();
    if (sample_data_field_store_line(0, buf, len) != ESP_OK) {
        return COAP_500_INTERNAL_SERVER_ERROR;
    }
    s_sample_data_line_count = 1;
    notify_change(RES_SAMPLE_DATA_FIELD);
    return COAP_204_CHANGED;
}

static uint8_t set_data_field_multi(const lwm2m_data_t *multi)
{
    if (!multi || multi->type != LWM2M_TYPE_MULTIPLE_RESOURCE) {
        return COAP_400_BAD_REQUEST;
    }

    size_t count = multi->value.asChildren.count;
    if (count > SAMPLE_DATA_FIELD_MAX_LINES) {
        return COAP_413_ENTITY_TOO_LARGE;
    }

    lwm2m_data_t *children = multi->value.asChildren.array;
    if (count > 0 && !children) {
        return COAP_400_BAD_REQUEST;
    }

    for (size_t i = 0; i < count; i++) {
        if (children[i].type != LWM2M_TYPE_STRING && children[i].type != LWM2M_TYPE_OPAQUE) {
            return COAP_400_BAD_REQUEST;
        }
        if (children[i].value.asBuffer.length > SAMPLE_DATA_FIELD_LINE_MAX_LEN) {
            return COAP_413_ENTITY_TOO_LARGE;
        }
    }

    bool unchanged = (count == s_sample_data_line_count);
    if (unchanged) {
        for (size_t i = 0; i < count; i++) {
            size_t len = children[i].value.asBuffer.length;
            if (s_sample_data_line_lens[i] != len ||
                (len > 0 && (!s_sample_data_lines[i] ||
                             memcmp(s_sample_data_lines[i], children[i].value.asBuffer.buffer, len) != 0))) {
                unchanged = false;
                break;
            }
        }
    }
    if (unchanged) {
        return COAP_204_CHANGED;
    }

    sample_data_field_clear_all();
    for (size_t i = 0; i < count; i++) {
        size_t len = children[i].value.asBuffer.length;
        if (sample_data_field_store_line(i, children[i].value.asBuffer.buffer, len) != ESP_OK) {
            sample_data_field_clear_all();
            return COAP_500_INTERNAL_SERVER_ERROR;
        }
    }
    s_sample_data_line_count = count;
    notify_change(RES_SAMPLE_DATA_FIELD);
    return COAP_204_CHANGED;
}

static bool looks_like_json(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return false;
    size_t i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) i++;
    return i < len && buf[i] == '{';
}

static void sample_data_field_clear_all(void)
{
    for (size_t i = 0; i < SAMPLE_DATA_FIELD_MAX_LINES; i++) {
        if (s_sample_data_lines[i]) {
            free(s_sample_data_lines[i]);
            s_sample_data_lines[i] = NULL;
        }
        s_sample_data_line_lens[i] = 0;
    }
    s_sample_data_line_count = 0;
}

static esp_err_t sample_data_field_store_line(size_t index, const uint8_t *buf, size_t len)
{
    if (index >= SAMPLE_DATA_FIELD_MAX_LINES) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len == 0) {
        if (s_sample_data_lines[index]) {
            free(s_sample_data_lines[index]);
            s_sample_data_lines[index] = NULL;
        }
        s_sample_data_line_lens[index] = 0;
        return ESP_OK;
    }

    uint8_t *next = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) {
        next = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_8BIT);
    }
    if (!next) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(next, buf, len);
    if (s_sample_data_lines[index]) {
        free(s_sample_data_lines[index]);
    }
    s_sample_data_lines[index] = next;
    s_sample_data_line_lens[index] = len;
    return ESP_OK;
}

static uint8_t prv_read(lwm2m_context_t *contextP, uint16_t instanceId, int *numDataP, lwm2m_data_t **dataArrayP, lwm2m_object_t *objectP)
{
    (void)contextP;
    (void)objectP;

    if (instanceId != 0) return COAP_404_NOT_FOUND;

    if (*numDataP == 0) {
        uint16_t resources[] = {
            RES_SAMPLE_CONFIG_VERSION,
            RES_SAMPLE_ENABLED,
            RES_SAMPLE_RATE,
            RES_SAMPLE_REPORT_RATE,
            RES_SAMPLE_SEND_ACK_TIMEOUT_MS,
            RES_SAMPLE_SEND_RETRY_DELAY_MS,
            RES_SAMPLE_SEND_RETRY_COUNT,
            RES_SAMPLE_SERVER_ALIGNED_UPTIME_MS,
            RES_SAMPLE_UPTIME_DIFFERENCE_MS,
            RES_SAMPLE_SLEEP_MODE,
            RES_SAMPLE_SENSOR_UART_I2C,
            RES_SAMPLE_SENSOR_RS485,
            RES_SAMPLE_DATA_FIELD
        };
        int count = sizeof(resources) / sizeof(resources[0]);
        *dataArrayP = lwm2m_data_new(count);
        if (!*dataArrayP) return COAP_500_INTERNAL_SERVER_ERROR;
        *numDataP = count;
        for (int i = 0; i < count; i++) (*dataArrayP)[i].id = resources[i];
    }

    for (int i = 0; i < *numDataP; i++) {
        switch ((*dataArrayP)[i].id) {
            case RES_SAMPLE_CONFIG_VERSION:
                lwm2m_data_encode_int((int64_t)s_sample.config_version, (*dataArrayP) + i);
                break;
            case RES_SAMPLE_ENABLED:
                lwm2m_data_encode_bool(s_sample.sampling_enabled, (*dataArrayP) + i);
                break;
            case RES_SAMPLE_RATE:
                lwm2m_data_encode_int((int64_t)s_sample.sample_rate, (*dataArrayP) + i);
                break;
            case RES_SAMPLE_REPORT_RATE:
                lwm2m_data_encode_int((int64_t)s_sample.report_rate, (*dataArrayP) + i);
                break;
            case RES_SAMPLE_SEND_ACK_TIMEOUT_MS:
                lwm2m_data_encode_int((int64_t)s_sample.send_ack_timeout_ms, (*dataArrayP) + i);
                break;
            case RES_SAMPLE_SEND_RETRY_DELAY_MS:
                lwm2m_data_encode_int((int64_t)s_sample.send_retry_delay_ms, (*dataArrayP) + i);
                break;
            case RES_SAMPLE_SEND_RETRY_COUNT:
                lwm2m_data_encode_int((int64_t)s_sample.send_retry_count, (*dataArrayP) + i);
                break;
            case RES_SAMPLE_SERVER_ALIGNED_UPTIME_MS: {
                lwm2m_context_t *ctx = get_lwm2m_context();
                int64_t aligned_ms = lwm2m_registration_uptime_ms(ctx);
                lwm2m_data_encode_int(aligned_ms, (*dataArrayP) + i);
                break;
            }
            case RES_SAMPLE_UPTIME_DIFFERENCE_MS: {
                lwm2m_context_t *ctx = get_lwm2m_context();
                int64_t diff_ms = lwm2m_registration_uptime_offset_ms(ctx);
                lwm2m_data_encode_int(diff_ms, (*dataArrayP) + i);
                break;
            }
            case RES_SAMPLE_SLEEP_MODE:
                lwm2m_data_encode_string(s_sample.sleep_mode, (*dataArrayP) + i);
                break;
            case RES_SAMPLE_SENSOR_UART_I2C:
                lwm2m_data_encode_string(s_sample.sensor_uart_i2c, (*dataArrayP) + i);
                break;
            case RES_SAMPLE_SENSOR_RS485:
                lwm2m_data_encode_string(s_sample.sensor_rs485, (*dataArrayP) + i);
                break;
            case RES_SAMPLE_DATA_FIELD:
                if (s_sample_data_line_count == 0) {
                    lwm2m_data_t *sub = lwm2m_data_new(1);
                    if (!sub) return COAP_500_INTERNAL_SERVER_ERROR;
                    sub[0].id = 0;
                    lwm2m_data_encode_opaque(NULL, 0, sub + 0);
                    lwm2m_data_encode_instances(sub, 1, (*dataArrayP) + i);
                } else {
                    lwm2m_data_t *sub = lwm2m_data_new((int)s_sample_data_line_count);
                    if (!sub) return COAP_500_INTERNAL_SERVER_ERROR;
                    for (size_t j = 0; j < s_sample_data_line_count; j++) {
                        sub[j].id = (uint16_t)j;
                        lwm2m_data_encode_opaque(s_sample_data_lines[j] ? s_sample_data_lines[j] : (const uint8_t *)"",
                                                 s_sample_data_line_lens[j],
                                                 sub + j);
                    }
                    lwm2m_data_encode_instances(sub, s_sample_data_line_count, (*dataArrayP) + i);
                }
                break;
            default:
                return COAP_404_NOT_FOUND;
        }
    }

    return COAP_205_CONTENT;
}

static uint8_t prv_write(lwm2m_context_t *contextP, uint16_t instanceId, int numData, lwm2m_data_t *dataArray, lwm2m_object_t *objectP, lwm2m_write_type_t writeType)
{
    (void)contextP;
    (void)objectP;
    (void)writeType;

    if (instanceId != 0) return COAP_404_NOT_FOUND;

    for (int i = 0; i < numData; i++) {
        switch (dataArray[i].id) {
            case RES_SAMPLE_CONFIG_VERSION: {
                if ((dataArray[i].type == LWM2M_TYPE_STRING || dataArray[i].type == LWM2M_TYPE_OPAQUE) &&
                    looks_like_json(dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length)) {
                    uint8_t code = apply_json_config(dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length);
                    if (code != COAP_204_CHANGED) return code;
                    break;
                }
                int64_t val = 0;
                if (1 != lwm2m_data_decode_int(&dataArray[i], &val)) return COAP_400_BAD_REQUEST;
                uint8_t code = set_u32_field(dataArray[i].id, val);
                if (code != COAP_204_CHANGED) return code;
                break;
            }
            case RES_SAMPLE_ENABLED: {
                uint8_t code = set_enabled(&dataArray[i]);
                if (code != COAP_204_CHANGED) return code;
                break;
            }
            case RES_SAMPLE_SLEEP_MODE: {
                if (dataArray[i].type != LWM2M_TYPE_STRING && dataArray[i].type != LWM2M_TYPE_OPAQUE) return COAP_400_BAD_REQUEST;
                uint8_t code = set_sleep_mode(dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length);
                if (code != COAP_204_CHANGED) return code;
                break;
            }
            case RES_SAMPLE_RATE:
            case RES_SAMPLE_REPORT_RATE: {
                int64_t val = 0;
                if (1 != lwm2m_data_decode_int(&dataArray[i], &val)) return COAP_400_BAD_REQUEST;
                uint8_t code = set_u32_field(dataArray[i].id, val);
                if (code != COAP_204_CHANGED) return code;
                break;
            }
            case RES_SAMPLE_SEND_ACK_TIMEOUT_MS:
            case RES_SAMPLE_SEND_RETRY_DELAY_MS:
            case RES_SAMPLE_SEND_RETRY_COUNT: {
                int64_t val = 0;
                if (1 != lwm2m_data_decode_int(&dataArray[i], &val)) return COAP_400_BAD_REQUEST;
                uint8_t code = set_u32_field(dataArray[i].id, val);
                if (code != COAP_204_CHANGED) return code;
                break;
            }
            case RES_SAMPLE_SENSOR_UART_I2C:
            case RES_SAMPLE_SENSOR_RS485: {
                if (dataArray[i].type != LWM2M_TYPE_STRING && dataArray[i].type != LWM2M_TYPE_OPAQUE) return COAP_400_BAD_REQUEST;
                uint8_t code = set_sensor(dataArray[i].id, dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length);
                if (code != COAP_204_CHANGED) return code;
                break;
            }
            case RES_SAMPLE_DATA_FIELD: {
                uint8_t code;
                if (dataArray[i].type == LWM2M_TYPE_MULTIPLE_RESOURCE) {
                    code = set_data_field_multi(&dataArray[i]);
                } else {
                    if (dataArray[i].type != LWM2M_TYPE_STRING && dataArray[i].type != LWM2M_TYPE_OPAQUE) return COAP_400_BAD_REQUEST;
                    code = set_data_field(dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length);
                }
                if (code != COAP_204_CHANGED) return code;
                break;
            }
            default:
                return COAP_405_METHOD_NOT_ALLOWED;
        }
    }

    return COAP_204_CHANGED;
}

lwm2m_object_t *get_sample_object(void)
{
    if (s_sample_obj) return s_sample_obj;

    sample_nvs_load_state();

    lwm2m_object_t *obj = (lwm2m_object_t *)lwm2m_malloc(sizeof(lwm2m_object_t));
    if (!obj) return NULL;

    memset(obj, 0, sizeof(lwm2m_object_t));
    obj->objID = LWM2M_OBJ_SAMPLE;

    obj->instanceList = (lwm2m_list_t *)lwm2m_malloc(sizeof(lwm2m_list_t));
    if (!obj->instanceList) {
        lwm2m_free(obj);
        return NULL;
    }

    memset(obj->instanceList, 0, sizeof(lwm2m_list_t));
    obj->readFunc = prv_read;
    obj->writeFunc = prv_write;
    obj->userData = &s_sample;

    s_sample_obj = obj;
    return s_sample_obj;
}

uint32_t lwm2m_sample_get_config_version(void)
{
    sample_nvs_load_state();
    return s_sample.config_version;
}

int lwm2m_sample_get_i2c_script_missing(void)
{
    sample_nvs_load_state();
    return sample_interface_script_missing(s_sample.sensor_uart_i2c);
}

int lwm2m_sample_get_rs485_script_missing(void)
{
    sample_nvs_load_state();
    return sample_interface_script_missing(s_sample.sensor_rs485);
}

esp_err_t lwm2m_sample_apply_json_config(const uint8_t *json, size_t len)
{
    uint8_t code;

    sample_nvs_load_state();
    code = apply_json_config(json, len);
    if (code == COAP_204_CHANGED)
    {
        return ESP_OK;
    }
    if (code == COAP_400_BAD_REQUEST)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_FAIL;
}

esp_err_t lwm2m_sample_set_data_field(const char *data, size_t len)
{
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len > SAMPLE_DATA_FIELD_LINE_MAX_LEN) {
        len = SAMPLE_DATA_FIELD_LINE_MAX_LEN;
    }

    const uint8_t *lines[1] = {(const uint8_t *)data};
    size_t lens[1] = {len};
    return lwm2m_sample_set_data_field_multi(lines, lens, 1);
}

esp_err_t lwm2m_sample_set_data_field_multi(const uint8_t * const *lines,
                                            const size_t *line_lens,
                                            size_t line_count)
{
    if (line_count > 0 && (!lines || !line_lens)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (line_count > SAMPLE_DATA_FIELD_MAX_LINES) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < line_count; i++) {
        if (!lines[i] || line_lens[i] > SAMPLE_DATA_FIELD_LINE_MAX_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    bool unchanged = (line_count == s_sample_data_line_count);
    if (unchanged) {
        for (size_t i = 0; i < line_count; i++) {
            if (s_sample_data_line_lens[i] != line_lens[i] ||
                (line_lens[i] > 0 && (!s_sample_data_lines[i] ||
                                      memcmp(s_sample_data_lines[i], lines[i], line_lens[i]) != 0))) {
                unchanged = false;
                break;
            }
        }
    }
    if (unchanged) {
        return ESP_OK;
    }

    sample_data_field_clear_all();
    for (size_t i = 0; i < line_count; i++) {
        if (sample_data_field_store_line(i, lines[i], line_lens[i]) != ESP_OK) {
            sample_data_field_clear_all();
            return ESP_ERR_NO_MEM;
        }
    }
    s_sample_data_line_count = line_count;
    notify_change(RES_SAMPLE_DATA_FIELD);
    return ESP_OK;
}
