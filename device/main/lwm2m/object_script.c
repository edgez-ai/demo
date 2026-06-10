#include "object_script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sampling.h"
#include "lwm2m_client.h"

#define SCRIPT_NVS_PARTITION      "driver_nvs"
#define SCRIPT_NVS_NAMESPACE      "lwm2m_script"
#define SCRIPT_NVS_INDEX_KEY      "index"

#define SCRIPT_MAX_COUNT          16
#define SCRIPT_MAX_SIZE           (64 * 1024)
#define SCRIPT_NAME_MAX_LEN       64
#define SCRIPT_MIME_MAX_LEN       64
#define SCRIPT_EXEC_RESULT_MAX_LEN SAMPLE_SCRIPT_RESULT_PAYLOAD_MAX_LEN
#define SCRIPT_EXEC_RESULT_BASE64_MAX_LEN ((((SCRIPT_EXEC_RESULT_MAX_LEN) + 2) / 3) * 4 + 1)
#define SCRIPT_GLOBAL_BUFFER_SIZE_DEFAULT (256 * 1024)
#define SCRIPT_GLOBAL_BUFFER_SIZE_MAX     (1024 * 1024)

typedef struct {
    uint16_t id;
    uint16_t reserved;
    uint32_t size;
} script_meta_t;

typedef struct {
    bool active;
    uint16_t script_id;
    uint8_t *buffer;
    size_t expected_size;
    size_t received_size;
} script_upload_t;

static const char *TAG = "lwm2m_script";

static lwm2m_object_t *s_obj = NULL;
static bool s_partition_ready = false;
static bool s_loaded = false;
static script_meta_t s_meta[SCRIPT_MAX_COUNT] = {0};
static size_t s_meta_count = 0;
static script_upload_t s_upload = {0};

typedef struct {
    uint16_t script_id;
    bool valid;
    char json[SCRIPT_EXEC_RESULT_MAX_LEN];
} script_exec_result_cache_t;

static script_exec_result_cache_t s_exec_result_cache[SCRIPT_MAX_COUNT] = {0};

static esp_err_t load_index(void);
static esp_err_t load_script_name(uint16_t script_id, char *out_name, size_t out_name_size);
static esp_err_t save_script_name(uint16_t script_id, const uint8_t *buf, size_t len);
static esp_err_t load_script_global_buffer_size(uint16_t script_id, uint32_t *out_size);
static esp_err_t save_script_global_buffer_size(uint16_t script_id, uint32_t value);
static esp_err_t load_script_version(uint16_t script_id, uint32_t *out_version);
static esp_err_t save_script_version(uint16_t script_id, uint32_t value);
static esp_err_t load_script_buffer_mime_type(uint16_t script_id, char *out_mime, size_t out_mime_size);
static esp_err_t save_script_buffer_mime_type(uint16_t script_id, const uint8_t *buf, size_t len);
static esp_err_t load_script_exec_result_json(uint16_t script_id, char *out_json, size_t out_json_size);
static esp_err_t save_script_exec_result_json(uint16_t script_id, const uint8_t *buf, size_t len);
static esp_err_t apply_runtime_global_buffer_size(uint16_t script_id);
static void notify_script_resource_change(uint16_t instance_id, uint16_t resource_id);
static void rebuild_instance_list(void);
static esp_err_t save_index_locked(nvs_handle_t nvs);
static void log_script_version_payload_debug(const char *op,
                                             uint16_t instance_id,
                                             const lwm2m_data_t *data);

static size_t encode_base64_text(const uint8_t *src, size_t src_len, char *dst, size_t dst_size)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    if (!src || !dst || dst_size == 0) {
        return 0;
    }

    size_t out_len = 0;
    size_t i = 0;

    while (i + 2 < src_len) {
        if (out_len + 4 >= dst_size) {
            return 0;
        }

        uint32_t value = ((uint32_t)src[i] << 16) |
                         ((uint32_t)src[i + 1] << 8) |
                         (uint32_t)src[i + 2];

        dst[out_len++] = table[(value >> 18) & 0x3F];
        dst[out_len++] = table[(value >> 12) & 0x3F];
        dst[out_len++] = table[(value >> 6) & 0x3F];
        dst[out_len++] = table[value & 0x3F];
        i += 3;
    }

    if (i < src_len) {
        if (out_len + 4 >= dst_size) {
            return 0;
        }

        uint32_t value = ((uint32_t)src[i] << 16);
        dst[out_len++] = table[(value >> 18) & 0x3F];

        if (i + 1 < src_len) {
            value |= ((uint32_t)src[i + 1] << 8);
            dst[out_len++] = table[(value >> 12) & 0x3F];
            dst[out_len++] = table[(value >> 6) & 0x3F];
            dst[out_len++] = '=';
        } else {
            dst[out_len++] = table[(value >> 12) & 0x3F];
            dst[out_len++] = '=';
            dst[out_len++] = '=';
        }
    }

    if (out_len >= dst_size) {
        return 0;
    }

    dst[out_len] = '\0';
    return out_len;
}

static const char *script_resource_name(uint16_t resource_id)
{
    switch (resource_id) {
        case RES_SCRIPT_ID:
            return "script_id";
        case RES_SCRIPT_SIZE:
            return "script_size";
        case RES_SCRIPT_CONTROL:
            return "script_control";
        case RES_SCRIPT_CHUNK:
            return "script_chunk";
        case RES_SCRIPT_NAME:
            return "script_name";
        case RES_SCRIPT_EXECUTE:
            return "script_execute";
        case RES_SCRIPT_GLOBAL_BUFFER_SIZE:
            return "global_buffer_size";
        case RES_SCRIPT_BUFFER_MIME_TYPE:
            return "buffer_mime_type";
        case RES_SCRIPT_GLOBAL_BUFFER_LEN:
            return "global_buffer_len";
        case RES_SCRIPT_GLOBAL_BUFFER_DATA:
            return "global_buffer_data";
        case RES_SCRIPT_EXEC_RESULT_JSON:
            return "exec_result_json";
        case RES_SCRIPT_VERSION:
            return "script_version";
        default:
            return "unknown";
    }
}

static void log_script_write_resource(uint16_t instance_id, int index, const lwm2m_data_t *data)
{
    if (!data) {
        return;
    }

    size_t payload_len = 0;
    const uint8_t *payload = NULL;
    if (data->type == LWM2M_TYPE_STRING || data->type == LWM2M_TYPE_OPAQUE) {
        payload = data->value.asBuffer.buffer;
        payload_len = data->value.asBuffer.length;
    }

    uint8_t head0 = (payload && payload_len > 0) ? payload[0] : 0;
    uint8_t head1 = (payload && payload_len > 1) ? payload[1] : 0;
    uint8_t head2 = (payload && payload_len > 2) ? payload[2] : 0;
    uint8_t head3 = (payload && payload_len > 3) ? payload[3] : 0;

    ESP_LOGI(TAG,
             "script write request: id=%u idx=%d res=%u(%s) type=%d payload_len=%u payload_ptr=%p head=%02X %02X %02X %02X",
             (unsigned)instance_id,
             index,
             (unsigned)data->id,
             script_resource_name(data->id),
             (int)data->type,
             (unsigned)payload_len,
             (const void *)payload,
             head0,
             head1,
             head2,
             head3);

        printf("[WAKAAMA TRACE] script write request: id=%u idx=%d res=%u(%s) type=%d payload_len=%u payload_ptr=%p head=%02X %02X %02X %02X\n",
            (unsigned)instance_id,
            index,
            (unsigned)data->id,
            script_resource_name(data->id),
            (int)data->type,
            (unsigned)payload_len,
            (const void *)payload,
            head0,
            head1,
            head2,
            head3);
}

static void log_script_version_payload_debug(const char *op,
                                             uint16_t instance_id,
                                             const lwm2m_data_t *data)
{
    if (!data) {
        ESP_LOGW(TAG,
                 "script %s version debug: id=%u data=NULL",
                 op ? op : "unknown",
                 (unsigned)instance_id);
        return;
    }

    int64_t as_int = 0;
    double as_float = 0.0;
    int int_ok = (lwm2m_data_decode_int((lwm2m_data_t *)data, &as_int) == 1) ? 1 : 0;
    int float_ok = (lwm2m_data_decode_float((lwm2m_data_t *)data, &as_float) == 1) ? 1 : 0;

    const uint8_t *buf = NULL;
    size_t len = 0;
    if (data->type == LWM2M_TYPE_STRING || data->type == LWM2M_TYPE_OPAQUE) {
        buf = data->value.asBuffer.buffer;
        len = data->value.asBuffer.length;
    }

    ESP_LOGW(TAG,
             "script %s version debug: id=%u type=%d len=%u int_ok=%d int=%lld float_ok=%d float=%f",
             op ? op : "unknown",
             (unsigned)instance_id,
             (int)data->type,
             (unsigned)len,
             int_ok,
             (long long)as_int,
             float_ok,
             as_float);

    if (buf && len > 0) {
        size_t dump_len = len > 16 ? 16 : len;
        char hex_buf[16 * 3 + 1] = {0};
        char txt_buf[16 + 1] = {0};
        size_t out = 0;

        for (size_t i = 0; i < dump_len; i++) {
            out += snprintf(hex_buf + out,
                            sizeof(hex_buf) - out,
                            "%02X%s",
                            buf[i],
                            (i + 1 < dump_len) ? " " : "");
            txt_buf[i] = isprint((unsigned char)buf[i]) ? (char)buf[i] : '.';
        }
        txt_buf[dump_len] = '\0';

        ESP_LOGW(TAG,
                 "script %s version raw: id=%u dump_len=%u/%u hex=%s text=%s",
                 op ? op : "unknown",
                 (unsigned)instance_id,
                 (unsigned)dump_len,
                 (unsigned)len,
                 hex_buf,
                 txt_buf);
    }
}

static int find_exec_result_cache_index(uint16_t script_id)
{
    for (size_t i = 0; i < SCRIPT_MAX_COUNT; i++) {
        if (s_exec_result_cache[i].valid && s_exec_result_cache[i].script_id == script_id) {
            return (int)i;
        }
    }
    return -1;
}

static void clear_exec_result_cache(uint16_t script_id)
{
    int idx = find_exec_result_cache_index(script_id);
    if (idx < 0) {
        return;
    }

    s_exec_result_cache[idx].valid = false;
    s_exec_result_cache[idx].script_id = 0;
    s_exec_result_cache[idx].json[0] = '\0';
}

static void refresh_sample_script_cache_on_script_change(const char *reason)
{
    sample_refresh_script_cache_from_selectors();
    ESP_LOGI(TAG,
             "Refreshed sample aggregate script cache after script object change (%s)",
             reason ? reason : "unknown");
}

static void notify_script_resource_change(uint16_t instance_id, uint16_t resource_id)
{
    lwm2m_context_t *ctx = get_lwm2m_context();
    if (!ctx || ctx->state != STATE_READY) {
        return;
    }

    lwm2m_uri_t uri = {
        .objectId = LWM2M_OBJ_SCRIPT,
        .instanceId = instance_id,
        .resourceId = resource_id,
    };
    lwm2m_resource_value_changed(ctx, &uri);
}

static size_t normalize_script_name(const char *src, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) {
        return 0;
    }

    size_t src_len = strlen(src);
    size_t begin = 0;
    size_t end = src_len;

    while (begin < end && isspace((unsigned char)src[begin])) {
        begin++;
    }
    while (end > begin && isspace((unsigned char)src[end - 1])) {
        end--;
    }

    while (begin < end && (src[begin] == '"' || src[begin] == '\'' || src[begin] == '[' || src[begin] == '{' || src[begin] == '(')) {
        begin++;
    }
    while (end > begin && (src[end - 1] == '"' || src[end - 1] == '\'' || src[end - 1] == ']' || src[end - 1] == '}' || src[end - 1] == ')')) {
        end--;
    }

    size_t written = 0;
    for (size_t i = begin; i < end && written + 1 < dst_size; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (isalnum(ch)) {
            dst[written++] = (char)tolower(ch);
        }
    }

    if (written >= 3 &&
        dst[written - 3] == 'l' &&
        dst[written - 2] == 'u' &&
        dst[written - 1] == 'a') {
        written -= 3;
    }

    dst[written] = '\0';
    return written;
}

static int cmp_meta_by_id(const void *a, const void *b)
{
    const script_meta_t *lhs = (const script_meta_t *)a;
    const script_meta_t *rhs = (const script_meta_t *)b;
    if (lhs->id < rhs->id) return -1;
    if (lhs->id > rhs->id) return 1;
    return 0;
}

static void make_blob_key(uint16_t script_id, char *key, size_t key_size)
{
    snprintf(key, key_size, "s%05u", (unsigned)script_id);
}

static void make_name_key(uint16_t script_id, char *key, size_t key_size)
{
    snprintf(key, key_size, "n%05u", (unsigned)script_id);
}

static void make_global_buffer_size_key(uint16_t script_id, char *key, size_t key_size)
{
    snprintf(key, key_size, "b%05u", (unsigned)script_id);
}

static void make_version_key(uint16_t script_id, char *key, size_t key_size)
{
    snprintf(key, key_size, "v%05u", (unsigned)script_id);
}

static void make_buffer_mime_key(uint16_t script_id, char *key, size_t key_size)
{
    snprintf(key, key_size, "m%05u", (unsigned)script_id);
}

static esp_err_t script_nvs_open(nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    if (!s_partition_ready) {
        esp_err_t init_err = nvs_flash_init_partition(SCRIPT_NVS_PARTITION);
        if (init_err == ESP_ERR_NVS_NO_FREE_PAGES || init_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase_partition(SCRIPT_NVS_PARTITION));
            init_err = nvs_flash_init_partition(SCRIPT_NVS_PARTITION);
        }
        if (init_err != ESP_OK) {
            return init_err;
        }
        s_partition_ready = true;
    }

    return nvs_open_from_partition(SCRIPT_NVS_PARTITION, SCRIPT_NVS_NAMESPACE, mode, out);
}

static int find_meta_index(uint16_t script_id)
{
    for (size_t i = 0; i < s_meta_count; i++) {
        if (s_meta[i].id == script_id) return (int)i;
    }
    return -1;
}

static esp_err_t create_script_instance(uint16_t script_id)
{
    if (script_id == 0 || script_id == LWM2M_MAX_ID) {
        return ESP_ERR_INVALID_ARG;
    }

    if (find_meta_index(script_id) >= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_meta_count >= SCRIPT_MAX_COUNT) {
        return ESP_ERR_NO_MEM;
    }

    size_t insert_at = s_meta_count;
    while (insert_at > 0 && s_meta[insert_at - 1].id > script_id) {
        s_meta[insert_at] = s_meta[insert_at - 1];
        insert_at--;
    }

    s_meta[insert_at].id = script_id;
    s_meta[insert_at].size = 0;
    s_meta[insert_at].reserved = 0;
    s_meta_count++;
    clear_exec_result_cache(script_id);

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = save_index_locked(nvs);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }

    if (err != ESP_OK) {
        int index = find_meta_index(script_id);
        if (index >= 0) {
            for (size_t i = (size_t)index; i + 1 < s_meta_count; i++) {
                s_meta[i] = s_meta[i + 1];
            }
            if (s_meta_count > 0) {
                s_meta_count--;
            }
        }
        return err;
    }

    rebuild_instance_list();
    return ESP_OK;
}

static void rebuild_instance_list(void)
{
    if (!s_obj) return;

    lwm2m_list_free(s_obj->instanceList);
    s_obj->instanceList = NULL;

    for (size_t i = 0; i < s_meta_count; i++) {
        lwm2m_list_t *node = (lwm2m_list_t *)lwm2m_malloc(sizeof(lwm2m_list_t));
        if (!node) {
            ESP_LOGW(TAG, "Failed allocating instance node for script id=%u", (unsigned)s_meta[i].id);
            continue;
        }
        memset(node, 0, sizeof(lwm2m_list_t));
        node->id = s_meta[i].id;
        s_obj->instanceList = LWM2M_LIST_ADD(s_obj->instanceList, node);
    }
}

static esp_err_t save_index_locked(nvs_handle_t nvs)
{
    esp_err_t err;
    if (s_meta_count == 0) {
        err = nvs_erase_key(nvs, SCRIPT_NVS_INDEX_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        return err;
    }

    return nvs_set_blob(nvs, SCRIPT_NVS_INDEX_KEY, s_meta, s_meta_count * sizeof(script_meta_t));
}

static size_t trim_trailing_lua_space(const char *s, size_t len)
{
    while (len > 0) {
        char c = s[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            len--;
            continue;
        }
        break;
    }
    return len;
}

static size_t trim_line_trailing_space(const char *s, size_t begin, size_t end)
{
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        end--;
    }
    return end;
}

static size_t skip_line_leading_space(const char *s, size_t begin, size_t end)
{
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) {
        begin++;
    }
    return begin;
}

static size_t remove_return_result_suffix(const char *script, size_t len)
{
    const char *target = "return result";
    size_t target_len = strlen(target);

    size_t end = trim_trailing_lua_space(script, len);
    if (end == 0) return len;

    size_t line_start = end;
    while (line_start > 0 && script[line_start - 1] != '\n' && script[line_start - 1] != '\r') {
        line_start--;
    }

    size_t check_begin = skip_line_leading_space(script, line_start, end);
    size_t check_end = trim_line_trailing_space(script, check_begin, end);

    if (check_end >= check_begin && (check_end - check_begin) == target_len &&
        strncmp(script + check_begin, target, target_len) == 0) {
        return line_start;
    }

    return len;
}

static size_t remove_local_result_prefix(const char *script, size_t len)
{
    const char *target = "local result = {}";
    size_t target_len = strlen(target);

    if (len == 0) return 0;

    size_t start = 0;
    while (start < len && (script[start] == ' ' || script[start] == '\t' || script[start] == '\r' || script[start] == '\n')) {
        start++;
    }

    size_t line_end = start;
    while (line_end < len && script[line_end] != '\n' && script[line_end] != '\r') {
        line_end++;
    }

    size_t check_begin = skip_line_leading_space(script, start, line_end);
    size_t check_end = trim_line_trailing_space(script, check_begin, line_end);

    if (check_end >= check_begin && (check_end - check_begin) == target_len &&
        strncmp(script + check_begin, target, target_len) == 0) {
        size_t next = line_end;
        if (next < len && script[next] == '\r') next++;
        if (next < len && script[next] == '\n') next++;
        return next;
    }

    return 0;
}

static esp_err_t read_script_blob_locked(nvs_handle_t nvs, uint16_t script_id, char **out_buf, size_t *out_len)
{
    char key[16] = {0};
    size_t blob_size = 0;
    make_blob_key(script_id, key, sizeof(key));

    esp_err_t err = nvs_get_blob(nvs, key, NULL, &blob_size);
    if (err != ESP_OK) return err;
    if (blob_size == 0) return ESP_ERR_NOT_FOUND;

    char *buf = (char *)malloc(blob_size + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    err = nvs_get_blob(nvs, key, buf, &blob_size);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    buf[blob_size] = '\0';
    *out_buf = buf;
    if (out_len) *out_len = blob_size;
    return ESP_OK;
}

static bool script_id_is_selected(uint16_t script_id, const uint16_t *selected_ids, size_t selected_count)
{
    if (!selected_ids || selected_count == 0) {
        return true;
    }

    for (size_t i = 0; i < selected_count; i++) {
        if (selected_ids[i] == script_id) {
            return true;
        }
    }

    return false;
}

static esp_err_t build_aggregate_script_locked(nvs_handle_t scripts_nvs,
                                               const uint16_t *selected_ids,
                                               size_t selected_count,
                                               char **out_script,
                                               size_t *out_len)
{
    if (!out_script || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_script = NULL;
    *out_len = 0;

    if (s_meta_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ESP_OK;
    size_t total_cap = 0;
    size_t total_len = 0;
    char *combined = NULL;

    script_meta_t selected_meta[SCRIPT_MAX_COUNT] = {0};
    size_t selected_meta_count = 0;
    for (size_t i = 0; i < s_meta_count; i++) {
        if (script_id_is_selected(s_meta[i].id, selected_ids, selected_count)) {
            selected_meta[selected_meta_count++] = s_meta[i];
        }
    }

    if (selected_meta_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t i = 0; i < selected_meta_count; i++) {
        char *script = NULL;
        size_t script_len = 0;
        err = read_script_blob_locked(scripts_nvs, selected_meta[i].id, &script, &script_len);
        if (err != ESP_OK || !script || script_len == 0) {
            free(script);
            continue;
        }

        size_t seg_start = 0;
        size_t seg_len = script_len;
        if (i > 0) {
            seg_start = remove_local_result_prefix(script, script_len);
            if (seg_start > script_len) seg_start = 0;
            seg_len = script_len - seg_start;
        }
        if (i + 1 < selected_meta_count) {
            size_t suffix_end = remove_return_result_suffix(script + seg_start, seg_len);
            if (suffix_end <= seg_len) {
                seg_len = suffix_end;
            }
        }

        size_t needed = total_len + seg_len + 2;
        if (needed > total_cap) {
            size_t new_cap = total_cap == 0 ? 2048 : total_cap;
            while (new_cap < needed) {
                new_cap *= 2;
            }
            char *next = (char *)realloc(combined, new_cap);
            if (!next) {
                free(combined);
                free(script);
                return ESP_ERR_NO_MEM;
            }
            combined = next;
            total_cap = new_cap;
        }

        if (total_len > 0 && combined[total_len - 1] != '\n') {
            combined[total_len++] = '\n';
        }
        if (seg_len > 0) {
            memcpy(combined + total_len, script + seg_start, seg_len);
            total_len += seg_len;
        }

        free(script);
    }

    if (!combined || total_len == 0) {
        free(combined);
        return ESP_ERR_NOT_FOUND;
    }

    *out_script = combined;
    *out_len = total_len;
    return ESP_OK;
}

esp_err_t lwm2m_script_build_aggregate(uint8_t *out_buf, size_t out_buf_size, size_t *out_len)
{
    return lwm2m_script_build_aggregate_for_ids(out_buf, out_buf_size, NULL, 0, out_len);
}

esp_err_t lwm2m_script_build_aggregate_for_ids(uint8_t *out_buf,
                                               size_t out_buf_size,
                                               const uint16_t *script_ids,
                                               size_t script_id_count,
                                               size_t *out_len)
{
    if (!out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_len = 0;

    esp_err_t load_err = load_index();
    if (load_err != ESP_OK) {
        return load_err;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    char *combined = NULL;
    size_t combined_len = 0;
    err = build_aggregate_script_locked(nvs, script_ids, script_id_count, &combined, &combined_len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    *out_len = combined_len;
    if (out_buf == NULL || out_buf_size == 0) {
        free(combined);
        return ESP_OK;
    }

    if (combined_len > out_buf_size) {
        free(combined);
        return ESP_ERR_NO_MEM;
    }

    memcpy(out_buf, combined, combined_len);
    free(combined);
    return ESP_OK;
}

esp_err_t lwm2m_script_build_aggregate_for_names(uint8_t *out_buf,
                                                 size_t out_buf_size,
                                                 const char * const *script_names,
                                                 size_t script_name_count,
                                                 size_t *out_len)
{
    if (!out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_len = 0;

    if (!script_names || script_name_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t selected_ids[SCRIPT_MAX_COUNT] = {0};
    size_t selected_count = 0;

    for (size_t i = 0; i < script_name_count && selected_count < SCRIPT_MAX_COUNT; i++) {
        if (!script_names[i] || script_names[i][0] == '\0') {
            continue;
        }

        uint16_t matched_id = 0;
        esp_err_t find_err = lwm2m_script_find_id_by_name(script_names[i], &matched_id);
        if (find_err != ESP_OK || matched_id == 0) {
            continue;
        }

        bool exists = false;
        for (size_t j = 0; j < selected_count; j++) {
            if (selected_ids[j] == matched_id) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            selected_ids[selected_count++] = matched_id;
        }
    }

    if (selected_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    return lwm2m_script_build_aggregate_for_ids(out_buf,
                                                out_buf_size,
                                                selected_ids,
                                                selected_count,
                                                out_len);
}

esp_err_t lwm2m_script_find_id_by_name(const char *script_name, uint16_t *out_id)
{
    if (!script_name || script_name[0] == '\0' || !out_id) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_id = 0;

    esp_err_t load_err = load_index();
    if (load_err != ESP_OK) {
        return load_err;
    }

    char normalized_query[SCRIPT_NAME_MAX_LEN] = {0};
    size_t normalized_query_len = normalize_script_name(script_name, normalized_query, sizeof(normalized_query));

    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < s_meta_count; i++) {
        char saved_name[SCRIPT_NAME_MAX_LEN] = {0};
        esp_err_t name_err = load_script_name(s_meta[i].id, saved_name, sizeof(saved_name));
        if (name_err != ESP_OK || saved_name[0] == '\0') {
            continue;
        }

        const char *saved_effective = saved_name;
        const char *saved_colon = strchr(saved_name, ':');
        if (saved_colon && *(saved_colon + 1) != '\0') {
            saved_effective = saved_colon + 1;
        }

        const char *query_effective = script_name;
        const char *query_colon = strchr(script_name, ':');
        if (query_colon && *(query_colon + 1) != '\0') {
            query_effective = query_colon + 1;
        }

        if (strcmp(saved_name, script_name) == 0) {
            *out_id = s_meta[i].id;
            err = ESP_OK;
            break;
        }

        if (strcmp(saved_effective, query_effective) == 0) {
            *out_id = s_meta[i].id;
            err = ESP_OK;
            break;
        }

        if (normalized_query_len > 0) {
            char normalized_saved[SCRIPT_NAME_MAX_LEN] = {0};
            size_t normalized_saved_len = normalize_script_name(saved_name, normalized_saved, sizeof(normalized_saved));
            if (normalized_saved_len > 0 && strcmp(normalized_saved, normalized_query) == 0) {
                *out_id = s_meta[i].id;
                err = ESP_OK;
                break;
            }

            char normalized_saved_effective[SCRIPT_NAME_MAX_LEN] = {0};
            size_t normalized_saved_effective_len = normalize_script_name(saved_effective,
                                                                          normalized_saved_effective,
                                                                          sizeof(normalized_saved_effective));
            char normalized_query_effective[SCRIPT_NAME_MAX_LEN] = {0};
            size_t normalized_query_effective_len = normalize_script_name(query_effective,
                                                                          normalized_query_effective,
                                                                          sizeof(normalized_query_effective));

            if (normalized_saved_effective_len > 0 &&
                normalized_query_effective_len > 0 &&
                strcmp(normalized_saved_effective, normalized_query_effective) == 0) {
                *out_id = s_meta[i].id;
                err = ESP_OK;
                break;
            }
        }
    }

    if (err != ESP_OK) {
        char *id_end = NULL;
        unsigned long parsed_id = strtoul(script_name, &id_end, 10);
        if (id_end != script_name && parsed_id > 0 && parsed_id <= UINT16_MAX) {
            if (*id_end == '\0' || *id_end == '-' || *id_end == ':') {
                for (size_t i = 0; i < s_meta_count; i++) {
                    if (s_meta[i].id == (uint16_t)parsed_id) {
                        *out_id = (uint16_t)parsed_id;
                        ESP_LOGI(TAG,
                                 "Script selector fallback matched by id: query='%s' -> id=%u",
                                 script_name,
                                 (unsigned)*out_id);
                        return ESP_OK;
                    }
                }
            }
        }

        ESP_LOGW(TAG,
                 "Script name lookup failed: query='%s' meta_count=%lu",
                 script_name,
                 (unsigned long)s_meta_count);
    }

    return err;
}

bool lwm2m_script_has_instance(uint16_t script_id)
{
    if (script_id == 0 || script_id == LWM2M_MAX_ID) {
        return false;
    }

    if (!s_loaded) {
        if (load_index() != ESP_OK) {
            return false;
        }
    }

    return find_meta_index(script_id) >= 0;
}

bool lwm2m_script_has_nonempty_instance(uint16_t script_id)
{
    if (script_id == 0 || script_id == LWM2M_MAX_ID) {
        return false;
    }

    if (!s_loaded) {
        if (load_index() != ESP_OK) {
            return false;
        }
    }

    int meta_idx = find_meta_index(script_id);
    if (meta_idx < 0) {
        return false;
    }

    return s_meta[meta_idx].size > 0;
}

static esp_err_t load_index(void)
{
    if (s_loaded) return ESP_OK;
    s_loaded = true;
    s_meta_count = 0;

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Script NVS open failed: %s", esp_err_to_name(err));
        }
        return ESP_OK;
    }

    size_t blob_size = 0;
    err = nvs_get_blob(nvs, SCRIPT_NVS_INDEX_KEY, NULL, &blob_size);
    if (err == ESP_OK && blob_size >= sizeof(script_meta_t)) {
        size_t count = blob_size / sizeof(script_meta_t);
        if (count > SCRIPT_MAX_COUNT) {
            count = SCRIPT_MAX_COUNT;
        }

        script_meta_t temp[SCRIPT_MAX_COUNT] = {0};
        size_t to_read = count * sizeof(script_meta_t);
        err = nvs_get_blob(nvs, SCRIPT_NVS_INDEX_KEY, temp, &to_read);
        if (err == ESP_OK) {
            for (size_t i = 0; i < count; i++) {
                if (temp[i].id == 0) continue;

                char key[16] = {0};
                make_blob_key(temp[i].id, key, sizeof(key));
                size_t actual = 0;
                if (nvs_get_blob(nvs, key, NULL, &actual) == ESP_OK && actual > 0) {
                    temp[i].size = (uint32_t)actual;
                    s_meta[s_meta_count++] = temp[i];
                }
            }
        }
    }

    qsort(s_meta, s_meta_count, sizeof(script_meta_t), cmp_meta_by_id);

    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t persist_script_blob(uint16_t script_id, const uint8_t *data, size_t size)
{
    if (script_id == 0 || !data || size == 0 || size > SCRIPT_MAX_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    int index = find_meta_index(script_id);
    if (index < 0) {
        if (s_meta_count >= SCRIPT_MAX_COUNT) {
            nvs_close(nvs);
            return ESP_ERR_NO_MEM;
        }
        index = (int)s_meta_count;
        s_meta[s_meta_count].id = script_id;
        s_meta[s_meta_count].size = 0;
        s_meta[s_meta_count].reserved = 0;
        s_meta_count++;
    }

    char key[16] = {0};
    make_blob_key(script_id, key, sizeof(key));

    err = nvs_set_blob(nvs, key, data, size);
    if (err == ESP_OK) {
        s_meta[index].size = (uint32_t)size;
        qsort(s_meta, s_meta_count, sizeof(script_meta_t), cmp_meta_by_id);
        err = save_index_locked(nvs);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    rebuild_instance_list();
    if (err == ESP_OK) {
        clear_exec_result_cache(script_id);
    }
    if (err == ESP_OK) {
        refresh_sample_script_cache_on_script_change("script_blob");
    }
    return err;
}

static esp_err_t delete_script_blob(uint16_t script_id)
{
    if (script_id == 0) return ESP_ERR_INVALID_ARG;

    int index = find_meta_index(script_id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    char key[16] = {0};
    make_blob_key(script_id, key, sizeof(key));
    err = nvs_erase_key(nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }

    if (err == ESP_OK) {
        char name_key[16] = {0};
        make_name_key(script_id, name_key, sizeof(name_key));
        esp_err_t name_erase_err = nvs_erase_key(nvs, name_key);
        if (name_erase_err != ESP_OK && name_erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = name_erase_err;
        }
    }

    if (err == ESP_OK) {
        char size_key[16] = {0};
        make_global_buffer_size_key(script_id, size_key, sizeof(size_key));
        esp_err_t size_erase_err = nvs_erase_key(nvs, size_key);
        if (size_erase_err != ESP_OK && size_erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = size_erase_err;
        }
    }

    if (err == ESP_OK) {
        char version_key[16] = {0};
        make_version_key(script_id, version_key, sizeof(version_key));
        esp_err_t version_erase_err = nvs_erase_key(nvs, version_key);
        if (version_erase_err != ESP_OK && version_erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = version_erase_err;
        }
    }

    if (err == ESP_OK) {
        char mime_key[16] = {0};
        make_buffer_mime_key(script_id, mime_key, sizeof(mime_key));
        esp_err_t mime_erase_err = nvs_erase_key(nvs, mime_key);
        if (mime_erase_err != ESP_OK && mime_erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = mime_erase_err;
        }
    }

    if (err == ESP_OK) {
        for (size_t i = (size_t)index; i + 1 < s_meta_count; i++) {
            s_meta[i] = s_meta[i + 1];
        }
        if (s_meta_count > 0) {
            s_meta_count--;
        }
        err = save_index_locked(nvs);
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    if (err == ESP_OK) {
        clear_exec_result_cache(script_id);
    }
    rebuild_instance_list();
    if (err == ESP_OK) {
        refresh_sample_script_cache_on_script_change("script_delete");
    }
    return err;
}

static void upload_reset(void)
{
    if (s_upload.buffer) {
        free(s_upload.buffer);
        s_upload.buffer = NULL;
    }
    s_upload.active = false;
    s_upload.script_id = 0;
    s_upload.expected_size = 0;
    s_upload.received_size = 0;
}

static uint8_t upload_start(uint16_t script_id, size_t total_size)
{
    if (script_id == 0 || total_size == 0 || total_size > SCRIPT_MAX_SIZE) {
        return COAP_400_BAD_REQUEST;
    }

    upload_reset();
    s_upload.buffer = (uint8_t *)malloc(total_size);
    if (!s_upload.buffer) {
        return COAP_500_INTERNAL_SERVER_ERROR;
    }

    s_upload.active = true;
    s_upload.script_id = script_id;
    s_upload.expected_size = total_size;
    s_upload.received_size = 0;
    return COAP_204_CHANGED;
}

static uint8_t upload_append(uint16_t script_id, const uint8_t *chunk, size_t chunk_len)
{
    if (!s_upload.active || s_upload.script_id != script_id || !chunk || chunk_len == 0) {
        return COAP_400_BAD_REQUEST;
    }
    if (s_upload.received_size + chunk_len > s_upload.expected_size) {
        return COAP_400_BAD_REQUEST;
    }

    memcpy(s_upload.buffer + s_upload.received_size, chunk, chunk_len);
    s_upload.received_size += chunk_len;
    return COAP_204_CHANGED;
}

static uint8_t upload_finish(uint16_t script_id)
{
    if (!s_upload.active || s_upload.script_id != script_id) {
        return COAP_400_BAD_REQUEST;
    }
    if (s_upload.received_size != s_upload.expected_size) {
        return COAP_400_BAD_REQUEST;
    }

    esp_err_t err = persist_script_blob(script_id, s_upload.buffer, s_upload.received_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist script id=%u: %s", (unsigned)script_id, esp_err_to_name(err));
        return COAP_500_INTERNAL_SERVER_ERROR;
    }

    upload_reset();
    return COAP_204_CHANGED;
}

static uint8_t handle_control(uint16_t script_id, const uint8_t *buf, size_t len)
{
    if (!buf || len == 0 || len >= 96 || script_id == 0) return COAP_400_BAD_REQUEST;

    char cmd[96] = {0};
    memcpy(cmd, buf, len);
    cmd[len] = '\0';

    while (len > 0 && (cmd[len - 1] == '\r' || cmd[len - 1] == '\n' || cmd[len - 1] == ' ' || cmd[len - 1] == '\t')) {
        cmd[len - 1] = '\0';
        len--;
    }

    if (strncmp(cmd, "START ", 6) == 0) {
        char *endptr = NULL;
        unsigned long size_ul = strtoul(cmd + 6, &endptr, 10);
        if (endptr == cmd + 6 || *endptr != '\0') return COAP_400_BAD_REQUEST;
        if (size_ul == 0 || size_ul > SCRIPT_MAX_SIZE) return COAP_400_BAD_REQUEST;
        return upload_start(script_id, (size_t)size_ul);
    }

    if (strcmp(cmd, "FINISH") == 0) {
        return upload_finish(script_id);
    }

    if (strcmp(cmd, "ABORT") == 0) {
        if (s_upload.active && s_upload.script_id == script_id) {
            upload_reset();
        }
        return COAP_204_CHANGED;
    }

    if (strcmp(cmd, "DELETE") == 0) {
        esp_err_t err = delete_script_blob(script_id);
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
            return COAP_204_CHANGED;
        }
        return COAP_500_INTERNAL_SERVER_ERROR;
    }

    return COAP_400_BAD_REQUEST;
}

static uint8_t handle_control_or_name(uint16_t script_id, const uint8_t *buf, size_t len)
{
    uint8_t code = handle_control(script_id, buf, len);
    if (code == COAP_204_CHANGED) {
        return code;
    }

    if (!buf || len == 0) {
        return COAP_400_BAD_REQUEST;
    }

    esp_err_t err = save_script_name(script_id, buf, len);
    if (err == ESP_OK) {
        return COAP_204_CHANGED;
    }

    return (err == ESP_ERR_INVALID_ARG) ? COAP_400_BAD_REQUEST : COAP_500_INTERNAL_SERVER_ERROR;
}

static esp_err_t load_script_name(uint16_t script_id, char *out_name, size_t out_name_size)
{
    if (!out_name || out_name_size == 0 || script_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_name[0] = '\0';

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    char key[16] = {0};
    make_name_key(script_id, key, sizeof(key));
    size_t read_len = out_name_size;
    err = nvs_get_str(nvs, key, out_name, &read_len);
    nvs_close(nvs);
    return err;
}

static esp_err_t save_script_name(uint16_t script_id, const uint8_t *buf, size_t len)
{
    if (script_id == 0 || !buf || len == 0 || len >= SCRIPT_NAME_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    char name[SCRIPT_NAME_MAX_LEN] = {0};
    memcpy(name, buf, len);
    name[len] = '\0';

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == ':')) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    char key[16] = {0};
    make_name_key(script_id, key, sizeof(key));
    err = nvs_set_str(nvs, key, name);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    if (err == ESP_OK) {
        refresh_sample_script_cache_on_script_change("script_name");
    }
    return err;
}

static esp_err_t load_script_global_buffer_size(uint16_t script_id, uint32_t *out_size)
{
    if (!out_size || script_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_size = SCRIPT_GLOBAL_BUFFER_SIZE_DEFAULT;

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    char key[16] = {0};
    make_global_buffer_size_key(script_id, key, sizeof(key));
    uint32_t stored = 0;
    err = nvs_get_u32(nvs, key, &stored);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    if (stored == 0 || stored > SCRIPT_GLOBAL_BUFFER_SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_size = stored;
    return ESP_OK;
}

static esp_err_t save_script_global_buffer_size(uint16_t script_id, uint32_t value)
{
    if (script_id == 0 || value == 0 || value > SCRIPT_GLOBAL_BUFFER_SIZE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    char key[16] = {0};
    make_global_buffer_size_key(script_id, key, sizeof(key));
    err = nvs_set_u32(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t load_script_version(uint16_t script_id, uint32_t *out_version)
{
    if (!out_version || script_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_version = 0;

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    char key[16] = {0};
    make_version_key(script_id, key, sizeof(key));
    uint32_t stored = 0;
    err = nvs_get_u32(nvs, key, &stored);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    *out_version = stored;
    return ESP_OK;
}

static esp_err_t save_script_version(uint16_t script_id, uint32_t value)
{
    if (script_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    char key[16] = {0};
    make_version_key(script_id, key, sizeof(key));
    err = nvs_set_u32(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t apply_runtime_global_buffer_size(uint16_t script_id)
{
    uint32_t configured_size = SCRIPT_GLOBAL_BUFFER_SIZE_DEFAULT;
    esp_err_t load_err = load_script_global_buffer_size(script_id, &configured_size);
    if (load_err != ESP_OK && load_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG,
                 "Failed to load global buffer size for script id=%u: %s",
                 (unsigned)script_id,
                 esp_err_to_name(load_err));
        return load_err;
    }

    esp_err_t apply_err = sample_script_global_buffer_set_capacity((size_t)configured_size);
    if (apply_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to apply global buffer size=%lu for script id=%u: %s",
                 (unsigned long)configured_size,
                 (unsigned)script_id,
                 esp_err_to_name(apply_err));
        return apply_err;
    }

    return ESP_OK;
}

static esp_err_t load_script_buffer_mime_type(uint16_t script_id, char *out_mime, size_t out_mime_size)
{
    if (!out_mime || out_mime_size == 0 || script_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_mime[0] = '\0';

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    char key[16] = {0};
    make_buffer_mime_key(script_id, key, sizeof(key));
    size_t read_len = out_mime_size;
    err = nvs_get_str(nvs, key, out_mime, &read_len);
    nvs_close(nvs);
    return err;
}

static esp_err_t save_script_buffer_mime_type(uint16_t script_id, const uint8_t *buf, size_t len)
{
    if (script_id == 0 || !buf || len == 0 || len >= SCRIPT_MIME_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    char mime[SCRIPT_MIME_MAX_LEN] = {0};
    memcpy(mime, buf, len);
    mime[len] = '\0';

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)mime[i];
        if (!isprint(ch) || isspace(ch)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    char key[16] = {0};
    make_buffer_mime_key(script_id, key, sizeof(key));
    err = nvs_set_str(nvs, key, mime);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t load_script_exec_result_json(uint16_t script_id, char *out_json, size_t out_json_size)
{
    if (!out_json || out_json_size == 0 || script_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_json[0] = '\0';
    int idx = find_exec_result_cache_index(script_id);
    if (idx < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t result_len = strnlen(s_exec_result_cache[idx].json, sizeof(s_exec_result_cache[idx].json));
    if (result_len == 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (result_len + 1 > out_json_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out_json, s_exec_result_cache[idx].json, result_len + 1);
    return ESP_OK;
}

static esp_err_t save_script_exec_result_json(uint16_t script_id, const uint8_t *buf, size_t len)
{
    if (script_id == 0 || !buf || len == 0 || len >= SCRIPT_EXEC_RESULT_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    char json[SCRIPT_EXEC_RESULT_MAX_LEN] = {0};
    memcpy(json, buf, len);
    json[len] = '\0';

    int idx = find_exec_result_cache_index(script_id);
    if (idx < 0) {
        for (size_t i = 0; i < SCRIPT_MAX_COUNT; i++) {
            if (!s_exec_result_cache[i].valid) {
                idx = (int)i;
                break;
            }
        }
    }

    if (idx < 0) {
        return ESP_ERR_NO_MEM;
    }

    s_exec_result_cache[idx].script_id = script_id;
    s_exec_result_cache[idx].valid = true;
    memcpy(s_exec_result_cache[idx].json, json, len + 1);
    return ESP_OK;
}

static uint8_t prv_read(lwm2m_context_t *contextP,
                        uint16_t instanceId,
                        int *numDataP,
                        lwm2m_data_t **dataArrayP,
                        lwm2m_object_t *objectP)
{
    (void)contextP;
    (void)objectP;

    int meta_idx = find_meta_index(instanceId);
    if (meta_idx < 0) {
        return COAP_404_NOT_FOUND;
    }

    if (*numDataP == 0) {
        uint16_t resources[] = {
            RES_SCRIPT_ID,
            RES_SCRIPT_SIZE,
            RES_SCRIPT_CONTROL,
            RES_SCRIPT_NAME,
            RES_SCRIPT_GLOBAL_BUFFER_SIZE,
            RES_SCRIPT_BUFFER_MIME_TYPE,
            RES_SCRIPT_GLOBAL_BUFFER_LEN,
            RES_SCRIPT_GLOBAL_BUFFER_DATA,
            RES_SCRIPT_EXEC_RESULT_JSON,
            RES_SCRIPT_VERSION,
        };
        int count = (int)(sizeof(resources) / sizeof(resources[0]));
        *dataArrayP = lwm2m_data_new(count);
        if (!*dataArrayP) return COAP_500_INTERNAL_SERVER_ERROR;
        *numDataP = count;
        for (int i = 0; i < count; i++) {
            (*dataArrayP)[i].id = resources[i];
        }
    }

    for (int i = 0; i < *numDataP; i++) {
        switch ((*dataArrayP)[i].id) {
            case RES_SCRIPT_ID:
                lwm2m_data_encode_int((int64_t)instanceId, (*dataArrayP) + i);
                break;
            case RES_SCRIPT_SIZE:
                lwm2m_data_encode_int((int64_t)s_meta[meta_idx].size, (*dataArrayP) + i);
                break;
            case RES_SCRIPT_CONTROL:
                if (s_upload.active && s_upload.script_id == instanceId) {
                    lwm2m_data_encode_string("uploading", (*dataArrayP) + i);
                } else {
                    lwm2m_data_encode_string("idle", (*dataArrayP) + i);
                }
                break;
            case RES_SCRIPT_NAME: {
                char name[SCRIPT_NAME_MAX_LEN] = {0};
                esp_err_t err = load_script_name(instanceId, name, sizeof(name));
                if (err == ESP_OK && name[0] != '\0') {
                    lwm2m_data_encode_string(name, (*dataArrayP) + i);
                } else {
                    char fallback[20] = {0};
                    snprintf(fallback, sizeof(fallback), "script_%u", (unsigned)instanceId);
                    lwm2m_data_encode_string(fallback, (*dataArrayP) + i);
                }
                break;
            }
            case RES_SCRIPT_GLOBAL_BUFFER_SIZE: {
                uint32_t size_value = SCRIPT_GLOBAL_BUFFER_SIZE_DEFAULT;
                esp_err_t err = load_script_global_buffer_size(instanceId, &size_value);
                if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
                    return COAP_500_INTERNAL_SERVER_ERROR;
                }
                lwm2m_data_encode_int((int64_t)size_value, (*dataArrayP) + i);
                break;
            }
            case RES_SCRIPT_BUFFER_MIME_TYPE: {
                char mime[SCRIPT_MIME_MAX_LEN] = {0};
                esp_err_t err = load_script_buffer_mime_type(instanceId, mime, sizeof(mime));
                if (err == ESP_OK && mime[0] != '\0') {
                    lwm2m_data_encode_string(mime, (*dataArrayP) + i);
                } else {
                    lwm2m_data_encode_string("application/octet-stream", (*dataArrayP) + i);
                }
                break;
            }
            case RES_SCRIPT_GLOBAL_BUFFER_LEN: {
                size_t global_len = sample_script_global_buffer_get_length();
                lwm2m_data_encode_int((int64_t)global_len, (*dataArrayP) + i);
                break;
            }
            case RES_SCRIPT_GLOBAL_BUFFER_DATA: {
                const uint8_t *global_data = sample_script_global_buffer_get_data();
                size_t global_len = sample_script_global_buffer_get_length();

                if (global_len > 0 && global_data == NULL) {
                    ESP_LOGW(TAG,
                             "script read buffer data rejected: id=%u len=%u data=NULL",
                             (unsigned)instanceId,
                             (unsigned)global_len);
                    return COAP_500_INTERNAL_SERVER_ERROR;
                }
                /**
                if (global_len > 0) {
                    ESP_LOGI(TAG,
                             "script read buffer data [rbf3]: id=%u len=%u head=%02X %02X %02X %02X",
                             (unsigned)instanceId,
                             (unsigned)global_len,
                             global_data[0],
                             (global_len > 1) ? global_data[1] : 0,
                             (global_len > 2) ? global_data[2] : 0,
                             (global_len > 3) ? global_data[3] : 0);
                } else {
                    ESP_LOGI(TAG,
                             "script read buffer data: id=%u len=0",
                             (unsigned)instanceId);
                }
                */

                size_t encode_len = global_len;
                lwm2m_data_t *encoded = (*dataArrayP) + i;

                while (true) {
                    lwm2m_data_encode_opaque(global_data ? global_data : (const uint8_t *)"",
                                             global_data ? encode_len : 0,
                                             encoded);

                    if (encode_len == 0 ||
                        (encoded->type == LWM2M_TYPE_OPAQUE &&
                         encoded->value.asBuffer.buffer != NULL &&
                         encoded->value.asBuffer.length == encode_len)) {
                        break;
                    }

                    if (encode_len <= 1024) {
                        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
                        size_t min_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
                        size_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
                        size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        ESP_LOGE(TAG,
                                 "script read buffer encode failed [rbf3]: id=%u len=%u type=%d enc_len=%u free_heap=%u min_free=%u largest_8bit=%u largest_psram=%u",
                                 (unsigned)instanceId,
                                 (unsigned)global_len,
                                 (int)encoded->type,
                                 (unsigned)encoded->value.asBuffer.length,
                                 (unsigned)free_heap,
                                 (unsigned)min_heap,
                                 (unsigned)largest_8bit,
                                 (unsigned)largest_psram);
                        return COAP_500_INTERNAL_SERVER_ERROR;
                    }

                    encode_len /= 2;
                }

                if (encode_len < global_len) {
                    ESP_LOGW(TAG,
                             "script read buffer truncated due to allocator limits: id=%u total_len=%u sent_len=%u",
                             (unsigned)instanceId,
                             (unsigned)global_len,
                             (unsigned)encode_len);
                }
                break;
            }
            case RES_SCRIPT_EXEC_RESULT_JSON: {
                char result_json[SCRIPT_EXEC_RESULT_MAX_LEN] = {0};
                esp_err_t err = load_script_exec_result_json(instanceId, result_json, sizeof(result_json));
                if (err == ESP_OK && result_json[0] != '\0') {
                    char result_b64[SCRIPT_EXEC_RESULT_BASE64_MAX_LEN] = {0};
                    size_t result_len = strnlen(result_json, sizeof(result_json));
                    size_t b64_len = encode_base64_text((const uint8_t *)result_json,
                                                        result_len,
                                                        result_b64,
                                                        sizeof(result_b64));
                    if (b64_len == 0) {
                        ESP_LOGW(TAG,
                                 "script read result base64 encode failed: id=%u len=%u",
                                 (unsigned)instanceId,
                                 (unsigned)result_len);
                        return COAP_500_INTERNAL_SERVER_ERROR;
                    }

                    lwm2m_data_encode_string(result_b64, (*dataArrayP) + i);
                } else if (err == ESP_ERR_NVS_NOT_FOUND) {
                    lwm2m_data_encode_string("W10=", (*dataArrayP) + i);
                } else {
                    return COAP_500_INTERNAL_SERVER_ERROR;
                }
                break;
            }
            case RES_SCRIPT_VERSION:
            {
                uint32_t script_version = 0;
                esp_err_t err = load_script_version(instanceId, &script_version);
                if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
                    return COAP_500_INTERNAL_SERVER_ERROR;
                }
                lwm2m_data_encode_int((int64_t)script_version, (*dataArrayP) + i);
                break;
            }
            case RES_SCRIPT_CHUNK:
                return COAP_405_METHOD_NOT_ALLOWED;
            default:
                return COAP_404_NOT_FOUND;
        }
    }

    return COAP_205_CONTENT;
}

static esp_err_t parse_script_buffer_size_from_data(const lwm2m_data_t *data, uint32_t *out_size)
{
    if (!data || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    if (data->type == LWM2M_TYPE_INTEGER) {
        int64_t parsed = 0;
        if (lwm2m_data_decode_int((lwm2m_data_t *)data, &parsed) != 1) {
            return ESP_ERR_INVALID_ARG;
        }
        if (parsed <= 0 || parsed > SCRIPT_GLOBAL_BUFFER_SIZE_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        *out_size = (uint32_t)parsed;
        return ESP_OK;
    }

    if (data->type != LWM2M_TYPE_STRING && data->type != LWM2M_TYPE_OPAQUE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!data->value.asBuffer.buffer || data->value.asBuffer.length == 0 || data->value.asBuffer.length >= 32) {
        return ESP_ERR_INVALID_ARG;
    }

    char text[32] = {0};
    memcpy(text, data->value.asBuffer.buffer, data->value.asBuffer.length);
    text[data->value.asBuffer.length] = '\0';

    char *endptr = NULL;
    unsigned long parsed_ul = strtoul(text, &endptr, 10);
    if (endptr == text || *endptr != '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (parsed_ul == 0 || parsed_ul > SCRIPT_GLOBAL_BUFFER_SIZE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_size = (uint32_t)parsed_ul;
    return ESP_OK;
}

static esp_err_t parse_script_version_from_data(const lwm2m_data_t *data, uint32_t *out_version)
{
    if (!data || !out_version) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Accept any wire type that can be losslessly decoded as integer. */
    {
        int64_t parsed = 0;
        if (lwm2m_data_decode_int((lwm2m_data_t *)data, &parsed) == 1) {
            if (parsed < 0 || parsed > UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            *out_version = (uint32_t)parsed;
            return ESP_OK;
        }
    }

    if (data->type == LWM2M_TYPE_INTEGER) {
        int64_t parsed = 0;
        if (lwm2m_data_decode_int((lwm2m_data_t *)data, &parsed) != 1) {
            return ESP_ERR_INVALID_ARG;
        }
        if (parsed < 0 || parsed > UINT32_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        *out_version = (uint32_t)parsed;
        return ESP_OK;
    }

    if (data->type == LWM2M_TYPE_FLOAT) {
        double parsed = 0.0;
        if (lwm2m_data_decode_float((lwm2m_data_t *)data, &parsed) != 1) {
            return ESP_ERR_INVALID_ARG;
        }
        if (parsed < 0.0 || parsed > (double)UINT32_MAX) {
            return ESP_ERR_INVALID_ARG;
        }

        uint32_t version = (uint32_t)parsed;
        if ((double)version != parsed) {
            return ESP_ERR_INVALID_ARG;
        }

        *out_version = version;
        return ESP_OK;
    }

    if (data->type != LWM2M_TYPE_STRING && data->type != LWM2M_TYPE_OPAQUE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!data->value.asBuffer.buffer || data->value.asBuffer.length == 0 || data->value.asBuffer.length >= 32) {
        return ESP_ERR_INVALID_ARG;
    }

    char text[32] = {0};
    memcpy(text, data->value.asBuffer.buffer, data->value.asBuffer.length);
    text[data->value.asBuffer.length] = '\0';

    char *endptr = NULL;
    unsigned long parsed_ul = strtoul(text, &endptr, 10);
    if (endptr == text || *endptr != '\0' || parsed_ul > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_version = (uint32_t)parsed_ul;
    return ESP_OK;
}

static uint8_t unwrap_script_instance_resources(uint16_t instance_id,
                                                int num_data,
                                                lwm2m_data_t *data_array,
                                                lwm2m_data_t **out_resources,
                                                int *out_resource_count)
{
    if (!out_resources || !out_resource_count) {
        return COAP_400_BAD_REQUEST;
    }

    *out_resources = data_array;
    *out_resource_count = num_data;

    if (num_data == 1 && data_array != NULL && data_array[0].type == LWM2M_TYPE_OBJECT_INSTANCE) {
        lwm2m_data_t *instance_node = &data_array[0];

        if (instance_node->id != instance_id) {
            ESP_LOGW(TAG,
                     "script payload rejected: object instance id mismatch payload=%u uri=%u",
                     (unsigned)instance_node->id,
                     (unsigned)instance_id);
            return COAP_400_BAD_REQUEST;
        }

        *out_resources = instance_node->value.asChildren.array;
        *out_resource_count = (int)instance_node->value.asChildren.count;
    }

    return COAP_204_CHANGED;
}

static uint8_t prv_write(lwm2m_context_t *contextP,
                         uint16_t instanceId,
                         int numData,
                         lwm2m_data_t *dataArray,
                         lwm2m_object_t *objectP,
                         lwm2m_write_type_t writeType)
{
    (void)contextP;
    (void)objectP;

    ESP_LOGI(TAG,
             "script write begin: id=%u writeType=%d numData=%d",
             (unsigned)instanceId,
             (int)writeType,
             numData);
        printf("[WAKAAMA TRACE] script write begin: id=%u writeType=%d numData=%d\n",
            (unsigned)instanceId,
            (int)writeType,
            numData);

    if (instanceId == 0 || instanceId == LWM2M_MAX_ID) {
        ESP_LOGW(TAG, "script write rejected: invalid instance id=0");
        return COAP_400_BAD_REQUEST;
    }

    lwm2m_data_t *resources = dataArray;
    int resource_count = numData;
    uint8_t unwrap_code = unwrap_script_instance_resources(instanceId,
                                                           numData,
                                                           dataArray,
                                                           &resources,
                                                           &resource_count);
    if (unwrap_code != COAP_204_CHANGED) {
        return unwrap_code;
    }

    if (resource_count <= 0 || resources == NULL) {
        /*
         * Some blockwise completion paths can surface an empty effective payload
         * after a valid multi-block transfer. Treat this as an idempotent no-op
         * instead of failing the whole script push.
         */
        ESP_LOGI(TAG,
                 "script write no-op: empty resource payload id=%u writeType=%d",
                 (unsigned)instanceId,
                 (int)writeType);
        return COAP_204_CHANGED;
    }

    bool created_instance = false;
    if (find_meta_index(instanceId) < 0) {
        if (writeType != LWM2M_WRITE_REPLACE_INSTANCE) {
            return COAP_404_NOT_FOUND;
        }

        esp_err_t create_err = create_script_instance(instanceId);
        if (create_err == ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG,
                     "script write rejected: create instance invalid argument id=%u writeType=%d",
                     (unsigned)instanceId,
                     (int)writeType);
            return COAP_400_BAD_REQUEST;
        }
        if (create_err == ESP_ERR_INVALID_STATE) {
            created_instance = false;
        } else if (create_err != ESP_OK) {
            return COAP_500_INTERNAL_SERVER_ERROR;
        } else {
            created_instance = true;
        }
    }

    for (int i = 0; i < resource_count; i++) {
        log_script_write_resource(instanceId, i, &resources[i]);

        switch (resources[i].id) {
            case RES_SCRIPT_CONTROL: {
                if (resources[i].type != LWM2M_TYPE_STRING && resources[i].type != LWM2M_TYPE_OPAQUE) {
                    ESP_LOGW(TAG,
                             "script write rejected: invalid control type=%d id=%u",
                             (int)resources[i].type,
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                uint8_t code = handle_control_or_name(instanceId,
                                                      resources[i].value.asBuffer.buffer,
                                                      resources[i].value.asBuffer.length);
                if (code != COAP_204_CHANGED) {
                    if (created_instance) {
                        (void)delete_script_blob(instanceId);
                    }
                    return code;
                }
                break;
            }
            case RES_SCRIPT_CHUNK: {
                if (resources[i].type != LWM2M_TYPE_STRING && resources[i].type != LWM2M_TYPE_OPAQUE) {
                    ESP_LOGW(TAG,
                             "script write rejected: invalid chunk type=%d id=%u",
                             (int)resources[i].type,
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                if (s_upload.active && s_upload.script_id == instanceId) {
                    uint8_t code = upload_append(instanceId,
                                                 resources[i].value.asBuffer.buffer,
                                                 resources[i].value.asBuffer.length);
                    if (code != COAP_204_CHANGED) {
                        if (created_instance) {
                            (void)delete_script_blob(instanceId);
                        }
                        return code;
                    }
                } else {
                    size_t blob_len = resources[i].value.asBuffer.length;
                    if (blob_len == 0 || blob_len > SCRIPT_MAX_SIZE) {
                        ESP_LOGW(TAG,
                                 "script write rejected: invalid direct chunk length=%u id=%u",
                                 (unsigned)blob_len,
                                 (unsigned)instanceId);
                        if (created_instance) {
                            (void)delete_script_blob(instanceId);
                        }
                        return COAP_400_BAD_REQUEST;
                    }

                    esp_err_t persist_err = persist_script_blob(instanceId,
                                                                resources[i].value.asBuffer.buffer,
                                                                blob_len);
                    if (persist_err != ESP_OK) {
                        ESP_LOGW(TAG,
                                 "script write failed: direct chunk persist id=%u err=%s",
                                 (unsigned)instanceId,
                                 esp_err_to_name(persist_err));
                        if (created_instance) {
                            (void)delete_script_blob(instanceId);
                        }
                        return COAP_500_INTERNAL_SERVER_ERROR;
                    }

                    ESP_LOGI(TAG,
                             "script write accepted: direct blob replace id=%u size=%u",
                             (unsigned)instanceId,
                             (unsigned)blob_len);
                }
                break;
            }
            case RES_SCRIPT_NAME: {
                if (resources[i].type != LWM2M_TYPE_STRING && resources[i].type != LWM2M_TYPE_OPAQUE) {
                    ESP_LOGW(TAG,
                             "script write rejected: invalid name type=%d id=%u",
                             (int)resources[i].type,
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                if (!resources[i].value.asBuffer.buffer || resources[i].value.asBuffer.length == 0) {
                    ESP_LOGI(TAG,
                             "script write no-op: empty script_name id=%u",
                             (unsigned)instanceId);
                    break;
                }
                esp_err_t err = save_script_name(instanceId,
                                                 resources[i].value.asBuffer.buffer,
                                                 resources[i].value.asBuffer.length);
                if (err != ESP_OK) {
                    if (created_instance) {
                        (void)delete_script_blob(instanceId);
                    }
                    return (err == ESP_ERR_INVALID_ARG) ? COAP_400_BAD_REQUEST : COAP_500_INTERNAL_SERVER_ERROR;
                }
                break;
            }
            case RES_SCRIPT_GLOBAL_BUFFER_SIZE: {
                uint32_t parsed_size = 0;
                esp_err_t parse_err = parse_script_buffer_size_from_data(&resources[i], &parsed_size);
                if (parse_err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "script write rejected: invalid global buffer size payload id=%u type=%d len=%u err=%s",
                             (unsigned)instanceId,
                             (int)resources[i].type,
                             (unsigned)resources[i].value.asBuffer.length,
                             esp_err_to_name(parse_err));
                    if (created_instance) {
                        (void)delete_script_blob(instanceId);
                    }
                    return COAP_400_BAD_REQUEST;
                }

                esp_err_t save_err = save_script_global_buffer_size(instanceId, parsed_size);
                if (save_err != ESP_OK) {
                    if (created_instance) {
                        (void)delete_script_blob(instanceId);
                    }
                    return COAP_500_INTERNAL_SERVER_ERROR;
                }

                esp_err_t apply_err = sample_script_global_buffer_set_capacity((size_t)parsed_size);
                if (apply_err != ESP_OK) {
                    if (created_instance) {
                        (void)delete_script_blob(instanceId);
                    }
                    return COAP_500_INTERNAL_SERVER_ERROR;
                }

                notify_script_resource_change(instanceId, RES_SCRIPT_GLOBAL_BUFFER_SIZE);
                break;
            }
            case RES_SCRIPT_BUFFER_MIME_TYPE: {
                if (resources[i].type != LWM2M_TYPE_STRING && resources[i].type != LWM2M_TYPE_OPAQUE) {
                    ESP_LOGW(TAG,
                             "script write rejected: invalid buffer mime type wire type=%d id=%u",
                             (int)resources[i].type,
                             (unsigned)instanceId);
                    if (created_instance) {
                        (void)delete_script_blob(instanceId);
                    }
                    return COAP_400_BAD_REQUEST;
                }

                esp_err_t save_err = save_script_buffer_mime_type(instanceId,
                                                                  resources[i].value.asBuffer.buffer,
                                                                  resources[i].value.asBuffer.length);
                if (save_err != ESP_OK) {
                    if (created_instance) {
                        (void)delete_script_blob(instanceId);
                    }
                    return (save_err == ESP_ERR_INVALID_ARG) ? COAP_400_BAD_REQUEST : COAP_500_INTERNAL_SERVER_ERROR;
                }
                break;
            }
            case RES_SCRIPT_VERSION: {
                uint32_t parsed_version = 0;
                esp_err_t parse_err = parse_script_version_from_data(&resources[i], &parsed_version);
                if (parse_err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "script write rejected: invalid script version payload id=%u type=%d len=%u err=%s",
                             (unsigned)instanceId,
                             (int)resources[i].type,
                             (unsigned)resources[i].value.asBuffer.length,
                             esp_err_to_name(parse_err));
                    log_script_version_payload_debug("write", instanceId, &resources[i]);
                    if (created_instance) {
                        (void)delete_script_blob(instanceId);
                    }
                    return COAP_400_BAD_REQUEST;
                }

                esp_err_t save_err = save_script_version(instanceId, parsed_version);
                if (save_err != ESP_OK) {
                    if (created_instance) {
                        (void)delete_script_blob(instanceId);
                    }
                    return COAP_500_INTERNAL_SERVER_ERROR;
                }

                notify_script_resource_change(instanceId, RES_SCRIPT_VERSION);
                break;
            }
            default:
                if (created_instance) {
                    (void)delete_script_blob(instanceId);
                }
                return COAP_405_METHOD_NOT_ALLOWED;
        }
    }

    return COAP_204_CHANGED;
}

static uint8_t prv_execute(lwm2m_context_t *contextP,
                           uint16_t instanceId,
                           uint16_t resourceId,
                           uint8_t *buffer,
                           int length,
                           lwm2m_object_t *objectP)
{
    (void)contextP;
    (void)buffer;
    (void)length;
    (void)objectP;

    if (resourceId != RES_SCRIPT_EXECUTE) {
        return COAP_405_METHOD_NOT_ALLOWED;
    }

    if (find_meta_index(instanceId) < 0) {
        ESP_LOGW(TAG,
                 "script execute rejected: script id=%u not found",
                 (unsigned)instanceId);
        return COAP_404_NOT_FOUND;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = script_nvs_open(NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "script execute failed: open NVS failed id=%u err=%s",
                 (unsigned)instanceId,
                 esp_err_to_name(err));
        return COAP_500_INTERNAL_SERVER_ERROR;
    }

    char *script_buf = NULL;
    size_t script_len = 0;
    err = read_script_blob_locked(nvs, instanceId, &script_buf, &script_len);
    nvs_close(nvs);
    if (err != ESP_OK || !script_buf || script_len == 0) {
        free(script_buf);
        ESP_LOGW(TAG,
                 "script execute failed: script read failed id=%u err=%s",
                 (unsigned)instanceId,
                 esp_err_to_name(err));
        return (err == ESP_ERR_NOT_FOUND) ? COAP_404_NOT_FOUND : COAP_500_INTERNAL_SERVER_ERROR;
    }

    sample_script_global_buffer_reset();

    char result_payload[SCRIPT_EXEC_RESULT_MAX_LEN] = {0};
    err = sample_execute_script_and_log_results((const uint8_t *)script_buf,
                                                script_len,
                                                result_payload,
                                                sizeof(result_payload));
    free(script_buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "script execute failed: id=%u err=%s",
                 (unsigned)instanceId,
                 esp_err_to_name(err));
        return COAP_500_INTERNAL_SERVER_ERROR;
    }

    ESP_LOGI(TAG,
             "script execute complete: id=%u result_payload=%s",
             (unsigned)instanceId,
             result_payload);

    size_t result_payload_len = strnlen(result_payload, sizeof(result_payload));
    if (result_payload_len == 0) {
        strcpy(result_payload, "[]");
        result_payload_len = 2;
    }

    esp_err_t save_result_err = save_script_exec_result_json(instanceId,
                                                             (const uint8_t *)result_payload,
                                                             result_payload_len);
    if (save_result_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "script execute failed: save result json id=%u err=%s",
                 (unsigned)instanceId,
                 esp_err_to_name(save_result_err));
        return COAP_500_INTERNAL_SERVER_ERROR;
    }

    size_t runtime_len = sample_script_global_buffer_get_length();
    const uint8_t *runtime_data = sample_script_global_buffer_get_data();
    ESP_LOGI(TAG,
             "script execute buffer prepared: id=%u len=%u head=%02X %02X %02X %02X",
             (unsigned)instanceId,
             (unsigned)runtime_len,
             (runtime_len > 0 && runtime_data != NULL) ? runtime_data[0] : 0,
             (runtime_len > 1 && runtime_data != NULL) ? runtime_data[1] : 0,
             (runtime_len > 2 && runtime_data != NULL) ? runtime_data[2] : 0,
             (runtime_len > 3 && runtime_data != NULL) ? runtime_data[3] : 0);

    notify_script_resource_change(instanceId, RES_SCRIPT_GLOBAL_BUFFER_LEN);
    notify_script_resource_change(instanceId, RES_SCRIPT_GLOBAL_BUFFER_DATA);
    notify_script_resource_change(instanceId, RES_SCRIPT_EXEC_RESULT_JSON);
    return COAP_204_CHANGED;
}

static uint8_t prv_create(lwm2m_context_t *contextP,
                          uint16_t instanceId,
                          int numData,
                          lwm2m_data_t *dataArray,
                          lwm2m_object_t *objectP)
{
    (void)contextP;
    (void)objectP;

    if (instanceId == 0 || instanceId == LWM2M_MAX_ID) {
        ESP_LOGW(TAG, "script create rejected: invalid instance id=0");
        return COAP_400_BAD_REQUEST;
    }

    lwm2m_data_t *resources = dataArray;
    int resource_count = numData;
    uint8_t unwrap_code = unwrap_script_instance_resources(instanceId,
                                                           numData,
                                                           dataArray,
                                                           &resources,
                                                           &resource_count);
    if (unwrap_code != COAP_204_CHANGED) {
        return unwrap_code;
    }

    const uint8_t *create_chunk = NULL;
    size_t create_chunk_len = 0;
    const uint8_t *create_name = NULL;
    size_t create_name_len = 0;
    const uint8_t *create_control = NULL;
    size_t create_control_len = 0;
    bool has_create_global_buffer_size = false;
    uint32_t create_global_buffer_size = SCRIPT_GLOBAL_BUFFER_SIZE_DEFAULT;
    bool has_create_version = false;
    uint32_t create_version = 0;
    const uint8_t *create_mime = NULL;
    size_t create_mime_len = 0;

    if (resource_count > 0 && resources != NULL) {
        for (int i = 0; i < resource_count; i++) {
            if (resources[i].id == RES_SCRIPT_ID) {
                int64_t parsed = 0;
                if (lwm2m_data_decode_int(&resources[i], &parsed) == 1) {
                    if (parsed <= 0 || parsed > 65535 || (uint16_t)parsed != instanceId) {
                        ESP_LOGW(TAG,
                                 "script create rejected: RES_SCRIPT_ID mismatch parsed=%lld instance=%u",
                                 (long long)parsed,
                                 (unsigned)instanceId);
                        return COAP_400_BAD_REQUEST;
                    }
                }
            } else if (resources[i].id == RES_SCRIPT_NAME) {
                if (resources[i].type != LWM2M_TYPE_STRING && resources[i].type != LWM2M_TYPE_OPAQUE) {
                    ESP_LOGW(TAG,
                             "script create rejected: invalid name type=%d id=%u",
                             (int)resources[i].type,
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                if (!resources[i].value.asBuffer.buffer || resources[i].value.asBuffer.length == 0) {
                    ESP_LOGW(TAG,
                             "script create rejected: invalid empty name id=%u",
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                create_name = resources[i].value.asBuffer.buffer;
                create_name_len = resources[i].value.asBuffer.length;
            } else if (resources[i].id == RES_SCRIPT_CONTROL) {
                if (resources[i].type != LWM2M_TYPE_STRING && resources[i].type != LWM2M_TYPE_OPAQUE) {
                    ESP_LOGW(TAG,
                             "script create rejected: invalid control type=%d id=%u",
                             (int)resources[i].type,
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                if (!resources[i].value.asBuffer.buffer || resources[i].value.asBuffer.length == 0) {
                    ESP_LOGW(TAG,
                             "script create rejected: empty control payload id=%u",
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                create_control = resources[i].value.asBuffer.buffer;
                create_control_len = resources[i].value.asBuffer.length;
            } else if (resources[i].id == RES_SCRIPT_CHUNK) {
                if (resources[i].type != LWM2M_TYPE_STRING && resources[i].type != LWM2M_TYPE_OPAQUE) {
                    ESP_LOGW(TAG,
                             "script create rejected: invalid chunk type=%d id=%u",
                             (int)resources[i].type,
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                if (!resources[i].value.asBuffer.buffer ||
                    resources[i].value.asBuffer.length == 0 ||
                    resources[i].value.asBuffer.length > SCRIPT_MAX_SIZE) {
                    ESP_LOGW(TAG,
                             "script create rejected: invalid chunk length=%u (max=%u) id=%u",
                             (unsigned)resources[i].value.asBuffer.length,
                             (unsigned)SCRIPT_MAX_SIZE,
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                create_chunk = resources[i].value.asBuffer.buffer;
                create_chunk_len = resources[i].value.asBuffer.length;
            } else if (resources[i].id == RES_SCRIPT_GLOBAL_BUFFER_SIZE) {
                esp_err_t parse_err = parse_script_buffer_size_from_data(&resources[i], &create_global_buffer_size);
                if (parse_err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "script create rejected: invalid global buffer size id=%u",
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                has_create_global_buffer_size = true;
            } else if (resources[i].id == RES_SCRIPT_BUFFER_MIME_TYPE) {
                if (resources[i].type != LWM2M_TYPE_STRING && resources[i].type != LWM2M_TYPE_OPAQUE) {
                    ESP_LOGW(TAG,
                             "script create rejected: invalid mime type type=%d id=%u",
                             (int)resources[i].type,
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                if (!resources[i].value.asBuffer.buffer || resources[i].value.asBuffer.length == 0) {
                    ESP_LOGW(TAG,
                             "script create rejected: empty mime type id=%u",
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                create_mime = resources[i].value.asBuffer.buffer;
                create_mime_len = resources[i].value.asBuffer.length;
            } else if (resources[i].id == RES_SCRIPT_VERSION) {
                esp_err_t parse_err = parse_script_version_from_data(&resources[i], &create_version);
                if (parse_err != ESP_OK) {
                    log_script_version_payload_debug("create", instanceId, &resources[i]);
                    ESP_LOGW(TAG,
                             "script create rejected: invalid version id=%u",
                             (unsigned)instanceId);
                    return COAP_400_BAD_REQUEST;
                }
                has_create_version = true;
            } else {
                return COAP_405_METHOD_NOT_ALLOWED;
            }
        }
    }

    bool instance_exists = (find_meta_index(instanceId) >= 0);
    if (instance_exists) {
        if (create_name != NULL && create_name_len > 0) {
            esp_err_t name_err = save_script_name(instanceId, create_name, create_name_len);
            if (name_err != ESP_OK) {
                return (name_err == ESP_ERR_INVALID_ARG) ? COAP_400_BAD_REQUEST : COAP_500_INTERNAL_SERVER_ERROR;
            }
        }

        if (create_control != NULL && create_control_len > 0) {
            uint8_t control_code = handle_control_or_name(instanceId, create_control, create_control_len);
            if (control_code != COAP_204_CHANGED) {
                return control_code;
            }
        }

        if (has_create_global_buffer_size) {
            esp_err_t size_err = save_script_global_buffer_size(instanceId, create_global_buffer_size);
            if (size_err != ESP_OK) {
                return (size_err == ESP_ERR_INVALID_ARG) ? COAP_400_BAD_REQUEST : COAP_500_INTERNAL_SERVER_ERROR;
            }

            esp_err_t apply_err = sample_script_global_buffer_set_capacity((size_t)create_global_buffer_size);
            if (apply_err != ESP_OK) {
                return COAP_500_INTERNAL_SERVER_ERROR;
            }

            notify_script_resource_change(instanceId, RES_SCRIPT_GLOBAL_BUFFER_SIZE);
        }

        if (create_mime != NULL && create_mime_len > 0) {
            esp_err_t mime_err = save_script_buffer_mime_type(instanceId, create_mime, create_mime_len);
            if (mime_err != ESP_OK) {
                return (mime_err == ESP_ERR_INVALID_ARG) ? COAP_400_BAD_REQUEST : COAP_500_INTERNAL_SERVER_ERROR;
            }
        }

        if (has_create_version) {
            esp_err_t version_err = save_script_version(instanceId, create_version);
            if (version_err != ESP_OK) {
                return COAP_500_INTERNAL_SERVER_ERROR;
            }

            notify_script_resource_change(instanceId, RES_SCRIPT_VERSION);
        }

        if (create_chunk && create_chunk_len > 0) {
            esp_err_t persist_err = persist_script_blob(instanceId, create_chunk, create_chunk_len);
            if (persist_err != ESP_OK) {
                return COAP_500_INTERNAL_SERVER_ERROR;
            }
        }

        return COAP_204_CHANGED;
    }

    esp_err_t create_err = create_script_instance(instanceId);
    if (create_err == ESP_ERR_INVALID_ARG) {
        return COAP_400_BAD_REQUEST;
    }
    if (create_err == ESP_ERR_INVALID_STATE) {
        return COAP_406_NOT_ACCEPTABLE;
    }
    if (create_err != ESP_OK) {
        return COAP_500_INTERNAL_SERVER_ERROR;
    }

    if (create_name != NULL && create_name_len > 0) {
        esp_err_t name_err = save_script_name(instanceId, create_name, create_name_len);
        if (name_err != ESP_OK) {
            (void)delete_script_blob(instanceId);
            return (name_err == ESP_ERR_INVALID_ARG) ? COAP_400_BAD_REQUEST : COAP_500_INTERNAL_SERVER_ERROR;
        }
    }

    if (create_control != NULL && create_control_len > 0) {
        uint8_t control_code = handle_control_or_name(instanceId, create_control, create_control_len);
        if (control_code != COAP_204_CHANGED) {
            (void)delete_script_blob(instanceId);
            return control_code;
        }
    }

    if (has_create_global_buffer_size) {
        esp_err_t size_err = save_script_global_buffer_size(instanceId, create_global_buffer_size);
        if (size_err != ESP_OK) {
            (void)delete_script_blob(instanceId);
            return (size_err == ESP_ERR_INVALID_ARG) ? COAP_400_BAD_REQUEST : COAP_500_INTERNAL_SERVER_ERROR;
        }

        esp_err_t apply_err = sample_script_global_buffer_set_capacity((size_t)create_global_buffer_size);
        if (apply_err != ESP_OK) {
            (void)delete_script_blob(instanceId);
            return COAP_500_INTERNAL_SERVER_ERROR;
        }

        notify_script_resource_change(instanceId, RES_SCRIPT_GLOBAL_BUFFER_SIZE);
    }

    if (create_mime != NULL && create_mime_len > 0) {
        esp_err_t mime_err = save_script_buffer_mime_type(instanceId, create_mime, create_mime_len);
        if (mime_err != ESP_OK) {
            (void)delete_script_blob(instanceId);
            return (mime_err == ESP_ERR_INVALID_ARG) ? COAP_400_BAD_REQUEST : COAP_500_INTERNAL_SERVER_ERROR;
        }
    }

    if (has_create_version) {
        esp_err_t version_err = save_script_version(instanceId, create_version);
        if (version_err != ESP_OK) {
            (void)delete_script_blob(instanceId);
            return COAP_500_INTERNAL_SERVER_ERROR;
        }

        notify_script_resource_change(instanceId, RES_SCRIPT_VERSION);
    }

    if (create_chunk && create_chunk_len > 0) {
        esp_err_t persist_err = persist_script_blob(instanceId, create_chunk, create_chunk_len);
        if (persist_err != ESP_OK) {
            (void)delete_script_blob(instanceId);
            return COAP_500_INTERNAL_SERVER_ERROR;
        }
    }

    return COAP_201_CREATED;
}

static uint8_t prv_delete(lwm2m_context_t *contextP,
                          uint16_t id,
                          lwm2m_object_t *objectP)
{
    (void)contextP;
    (void)objectP;

    if (id == 0) {
        return COAP_400_BAD_REQUEST;
    }

    bool had_active_upload = (s_upload.active && s_upload.script_id == id);
    if (s_upload.active && s_upload.script_id == id) {
        upload_reset();
    }

    if (find_meta_index(id) < 0) {
        if (had_active_upload) {
            ESP_LOGI(TAG, "Deleted active upload state for non-persisted script id=%u", (unsigned)id);
            return COAP_202_DELETED;
        }
        return COAP_404_NOT_FOUND;
    }

    esp_err_t err = delete_script_blob(id);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Deleted script instance id=%u", (unsigned)id);
        return COAP_202_DELETED;
    }

    return COAP_500_INTERNAL_SERVER_ERROR;
}

lwm2m_object_t *get_script_object(void)
{
    if (s_obj) return s_obj;

    load_index();

    lwm2m_object_t *obj = (lwm2m_object_t *)lwm2m_malloc(sizeof(lwm2m_object_t));
    if (!obj) return NULL;

    memset(obj, 0, sizeof(lwm2m_object_t));
    obj->objID = LWM2M_OBJ_SCRIPT;
    obj->readFunc = prv_read;
    obj->writeFunc = prv_write;
    obj->executeFunc = prv_execute;
    obj->createFunc = prv_create;
    obj->deleteFunc = prv_delete;

    s_obj = obj;
    rebuild_instance_list();

    if (s_meta_count > 0) {
        esp_err_t apply_err = apply_runtime_global_buffer_size(s_meta[0].id);
        if (apply_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Failed to apply persisted global buffer size at startup: %s",
                     esp_err_to_name(apply_err));
        }
    } else {
        (void)sample_script_global_buffer_set_capacity(SCRIPT_GLOBAL_BUFFER_SIZE_DEFAULT);
    }

    ESP_LOGI(TAG, "Script object initialized with %lu persisted scripts", (unsigned long)s_meta_count);
    return s_obj;
}
