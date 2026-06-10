#include "object_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#endif


// Ring buffer configuration
#define LOG_RING_LINES 64
#define LOG_LINE_MAX   192
#define LOG_READ_MAX   400
#define LOG_MULTI_MAX_LINES 8
#define LOG_MULTI_MAX_BYTES 256
#define LOG_CAPTURE_MAX 256
#define LOG_NOTIFY_MIN_INTERVAL_MS 250

typedef struct
{
    char text[LOG_LINE_MAX];
    uint16_t len;
} log_line_t;

static log_line_t s_ring[LOG_RING_LINES];
static size_t s_head = 0;
static size_t s_tail = 0;
static size_t s_count = 0;
static size_t s_pending = 0;
static uint32_t s_dropped = 0;
static uint32_t s_notify_count = 0;

#ifdef ESP_PLATFORM
static TickType_t s_last_notify_tick = 0;
#endif

#ifdef ESP_PLATFORM
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOG_LOCK() portENTER_CRITICAL_SAFE(&s_log_lock)
#define LOG_UNLOCK() portEXIT_CRITICAL_SAFE(&s_log_lock)
#else
#define LOG_LOCK() ((void)0)
#define LOG_UNLOCK() ((void)0)
#endif

#ifdef ESP_PLATFORM
static vprintf_like_t s_prev_vprintf = NULL;
static bool s_hook_installed = false;
#endif

static void log_ringbuf_clear(void)
{
    LOG_LOCK();
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_pending = 0;
    s_dropped = 0;
    s_notify_count = 0;
#ifdef ESP_PLATFORM
    s_last_notify_tick = 0;
#endif
    LOG_UNLOCK();
}

static void log_ringbuf_push_line(const char *line, size_t len)
{
    if (!line || len == 0) return;

    if (len >= LOG_LINE_MAX) {
        len = LOG_LINE_MAX - 1;
    }

    LOG_LOCK();
    if (s_count == LOG_RING_LINES) {
        // Drop oldest
        s_tail = (s_tail + 1) % LOG_RING_LINES;
        s_count--;
        s_dropped++;
        if (s_pending > 0) s_pending--;
    }

    log_line_t *slot = &s_ring[s_head];
    memcpy(slot->text, line, len);
    slot->text[len] = '\0';
    slot->len = (uint16_t)len;

    s_head = (s_head + 1) % LOG_RING_LINES;
    s_count++;
    s_pending++;
    LOG_UNLOCK();

}

static size_t log_ringbuf_peek_lines(char *out, size_t out_len, size_t *lines_peeked)
{
    if (!out || out_len == 0) return 0;

    size_t out_pos = 0;
    size_t peeked = 0;

    LOG_LOCK();
    size_t idx = s_tail;
    size_t remaining = s_count;
    while (remaining > 0) {
        log_line_t *slot = &s_ring[idx];
        size_t needed = slot->len + (out_pos > 0 ? 1 : 0);
        if (out_pos + needed >= out_len) break;

        if (out_pos > 0) {
            out[out_pos++] = '\n';
        }
        memcpy(out + out_pos, slot->text, slot->len);
        out_pos += slot->len;

        idx = (idx + 1) % LOG_RING_LINES;
        remaining--;
        peeked++;
    }
    LOG_UNLOCK();

    if (lines_peeked) *lines_peeked = peeked;
    if (out_pos < out_len) out[out_pos] = '\0';

    return out_pos;
}

static size_t log_ringbuf_snapshot_lines(char lines[][LOG_LINE_MAX], size_t max_lines, size_t max_bytes)
{
    if (!lines || max_lines == 0 || max_bytes == 0) return 0;

    size_t count = 0;
    size_t total = 0;

    LOG_LOCK();
    size_t idx = s_tail;
    size_t remaining = s_count;
    while (remaining > 0 && count < max_lines) {
        log_line_t *slot = &s_ring[idx];
        if (total + slot->len > max_bytes) break;

        size_t copy_len = slot->len;
        if (copy_len >= LOG_LINE_MAX) copy_len = LOG_LINE_MAX - 1;
        memcpy(lines[count], slot->text, copy_len);
        lines[count][copy_len] = '\0';
        count++;
        total += copy_len;

        idx = (idx + 1) % LOG_RING_LINES;
        remaining--;
    }
    LOG_UNLOCK();

    return count;
}

static void log_ringbuf_pop_count(size_t count)
{
    if (count == 0) return;

    LOG_LOCK();
    while (count > 0 && s_count > 0) {
        s_tail = (s_tail + 1) % LOG_RING_LINES;
        s_count--;
        if (s_pending > 0) s_pending--;
        count--;
    }
    LOG_UNLOCK();
}

static void log_capture_split_and_push(const char *buf, size_t len)
{
    if (!buf || len == 0) return;

    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || buf[i] == '\n') {
            size_t line_len = i - start;
            if (line_len > 0) {
                log_ringbuf_push_line(buf + start, line_len);
            }
            start = i + 1;
        }
    }
}

#ifdef ESP_PLATFORM
static int lwm2m_log_vprintf(const char *fmt, va_list args)
{
    int ret = 0;

    if (s_prev_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = s_prev_vprintf(fmt, args_copy);
        va_end(args_copy);
    } else {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    char tmp[LOG_CAPTURE_MAX];
    va_list args_copy2;
    va_copy(args_copy2, args);
    int len = vsnprintf(tmp, sizeof(tmp), fmt, args_copy2);
    va_end(args_copy2);

    if (len > 0) {
        size_t use_len = strnlen(tmp, sizeof(tmp));
        log_capture_split_and_push(tmp, use_len);
    }

    return ret;
}
#endif

void log_object_install_hook(void)
{
#ifdef ESP_PLATFORM
    if (!s_hook_installed) {
        s_prev_vprintf = esp_log_set_vprintf(lwm2m_log_vprintf);
        s_hook_installed = true;
    } else {
        vprintf_like_t prev = esp_log_set_vprintf(lwm2m_log_vprintf);
        if (prev != lwm2m_log_vprintf && prev != NULL) {
            s_prev_vprintf = prev;
        }
    }
#endif
}

static uint8_t prv_log_set_value(lwm2m_data_t *dataP)
{
    switch (dataP->id)
    {
    case RES_LOG_LINES: {
        char lines_buf[LOG_MULTI_MAX_LINES][LOG_LINE_MAX];
        size_t count = log_ringbuf_snapshot_lines(lines_buf, LOG_MULTI_MAX_LINES, LOG_MULTI_MAX_BYTES);
        if (count == 0) {
            lwm2m_data_encode_string("", dataP);
            return COAP_205_CONTENT;
        }

        lwm2m_data_t *subTlvP = NULL;
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE) {
            subTlvP = dataP->value.asChildren.array;
            if ((size_t)dataP->value.asChildren.count < count) {
                count = dataP->value.asChildren.count;
            }
        } else {
            subTlvP = lwm2m_data_new((int)count);
            if (subTlvP == NULL) return COAP_500_INTERNAL_SERVER_ERROR;
            for (size_t i = 0; i < count; i++) subTlvP[i].id = (uint16_t)i;
            lwm2m_data_encode_instances(subTlvP, (int)count, dataP);
        }

        for (size_t i = 0; i < count; i++) {
            lwm2m_data_encode_string(lines_buf[i], subTlvP + i);
        }

        log_ringbuf_pop_count(count);
        return COAP_205_CONTENT;
    }
    case RES_LOG_DROPPED: {
        uint32_t dropped = 0;
        LOG_LOCK();
        dropped = s_dropped;
        LOG_UNLOCK();
        lwm2m_data_encode_int(dropped, dataP);
        return COAP_205_CONTENT;
    }
    case RES_LOG_PENDING: {
        uint32_t pending = 0;
        LOG_LOCK();
        pending = (uint32_t)s_pending;
        LOG_UNLOCK();
        lwm2m_data_encode_int(pending, dataP);
        return COAP_205_CONTENT;
    }
    case RES_LOG_NOTIFY: {
        uint32_t notify_count = 0;
        LOG_LOCK();
        notify_count = s_notify_count;
        LOG_UNLOCK();
        lwm2m_data_encode_int(notify_count, dataP);
        return COAP_205_CONTENT;
    }
    case RES_LOG_CLEAR:
        return COAP_405_METHOD_NOT_ALLOWED;
    default:
        return COAP_404_NOT_FOUND;
    }
}

static uint8_t prv_log_read(lwm2m_context_t *contextP,
                            uint16_t instanceId,
                            int *numDataP,
                            lwm2m_data_t **dataArrayP,
                            lwm2m_object_t *objectP)
{
    (void)contextP;
    (void)objectP;

    if (instanceId != 0) return COAP_404_NOT_FOUND;

    if (*numDataP == 0) {
        uint16_t resList[] = { RES_LOG_LINES, RES_LOG_DROPPED, RES_LOG_PENDING, RES_LOG_NOTIFY };
        *numDataP = (int)(sizeof(resList) / sizeof(resList[0]));
        *dataArrayP = lwm2m_data_new(*numDataP);
        if (*dataArrayP == NULL) return COAP_500_INTERNAL_SERVER_ERROR;
        for (int i = 0; i < *numDataP; ++i) {
            (*dataArrayP)[i].id = resList[i];
        }
    }

    for (int i = 0; i < *numDataP; ++i) {
        if ((*dataArrayP)[i].type == LWM2M_TYPE_MULTIPLE_RESOURCE && (*dataArrayP)[i].id != RES_LOG_LINES) {
            return COAP_404_NOT_FOUND;
        }
        uint8_t result = prv_log_set_value((*dataArrayP) + i);
        if (result != COAP_205_CONTENT) return result;
    }

    return COAP_205_CONTENT;
}

static uint8_t prv_log_write(lwm2m_context_t *contextP,
                             uint16_t instanceId,
                             int numData,
                             lwm2m_data_t *dataArray,
                             lwm2m_object_t *objectP,
                             lwm2m_write_type_t writeType)
{
    (void)contextP;
    (void)objectP;
    (void)writeType;

    if (instanceId != 0) return COAP_404_NOT_FOUND;

    for (int i = 0; i < numData; ++i) {
        switch (dataArray[i].id) {
        case RES_LOG_CLEAR:
            log_ringbuf_clear();
            break;
        default:
            return COAP_405_METHOD_NOT_ALLOWED;
        }
    }

    return COAP_204_CHANGED;
}

static uint8_t prv_log_execute(lwm2m_context_t *contextP,
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

    if (instanceId != 0) return COAP_404_NOT_FOUND;

    switch (resourceId)
    {
    case RES_LOG_CLEAR:
        log_ringbuf_clear();
        return COAP_204_CHANGED;
    default:
        return COAP_405_METHOD_NOT_ALLOWED;
    }
}

lwm2m_object_t *get_object_log(void)
{
    lwm2m_object_t *objectP = (lwm2m_object_t *)lwm2m_malloc(sizeof(lwm2m_object_t));
    if (objectP == NULL) return NULL;

    memset(objectP, 0, sizeof(lwm2m_object_t));
    objectP->objID = LWM2M_LOG_OBJECT_ID;

    // single instance
    lwm2m_list_t *instance = (lwm2m_list_t *)lwm2m_malloc(sizeof(lwm2m_list_t));
    if (instance == NULL) {
        lwm2m_free(objectP);
        return NULL;
    }
    memset(instance, 0, sizeof(lwm2m_list_t));
    instance->id = 0;
    objectP->instanceList = instance;

    objectP->readFunc = prv_log_read;
    objectP->writeFunc = prv_log_write;
    objectP->executeFunc = prv_log_execute;

    // Ensure hook is installed so logs are captured
    log_object_install_hook();

    return objectP;
}

void free_object_log(lwm2m_object_t *objectP)
{
    if (objectP == NULL) return;

    if (objectP->instanceList != NULL) {
        lwm2m_list_free(objectP->instanceList);
    }
    lwm2m_free(objectP);
}

void log_object_process(lwm2m_context_t *contextP)
{
    if (contextP == NULL) return;
    log_object_install_hook();
    bool has_pending = false;
    LOG_LOCK();
    has_pending = (s_pending > 0);
    LOG_UNLOCK();
    if (!has_pending) return;

#ifdef ESP_PLATFORM
    TickType_t now = xTaskGetTickCount();
    LOG_LOCK();
    TickType_t last = s_last_notify_tick;
    if (last != 0 && (now - last) < pdMS_TO_TICKS(LOG_NOTIFY_MIN_INTERVAL_MS)) {
        LOG_UNLOCK();
        return;
    }
    s_last_notify_tick = now;
    LOG_UNLOCK();
#endif

    LOG_LOCK();
    s_notify_count++;
    LOG_UNLOCK();

    lwm2m_uri_t uri;
    memset(&uri, 0, sizeof(uri));
    uri.objectId = LWM2M_LOG_OBJECT_ID;
    uri.instanceId = 0;
    uri.resourceId = RES_LOG_LINES;

    lwm2m_resource_value_changed(contextP, &uri);
}
