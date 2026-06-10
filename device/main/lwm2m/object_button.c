#include "object_button.h"

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "lwm2m_client.h"

static const char *TAG = "lwm2m_btn";

typedef struct {
    bool pressed;
    uint32_t counter;
} button_state_t;

static button_state_t s_button = {0};
static lwm2m_object_t *s_button_obj = NULL;

static void notify_change(uint16_t resource_id)
{
    lwm2m_context_t *ctx = get_lwm2m_context();
    if (!ctx || ctx->state != STATE_READY) return;

    lwm2m_uri_t uri = {
        .objectId = LWM2M_OBJ_BUTTON,
        .instanceId = 0,
        .resourceId = resource_id,
    };
    lwm2m_resource_value_changed(ctx, &uri);
}

static uint8_t prv_read(lwm2m_context_t *contextP, uint16_t instanceId, int *numDataP, lwm2m_data_t **dataArrayP, lwm2m_object_t *objectP)
{
    (void)contextP;
    (void)objectP;

    if (instanceId != 0) return COAP_404_NOT_FOUND;

    if (*numDataP == 0) {
        uint16_t resources[] = {RES_BUTTON_STATE, RES_BUTTON_COUNTER};
        int count = sizeof(resources) / sizeof(resources[0]);
        *dataArrayP = lwm2m_data_new(count);
        if (!*dataArrayP) return COAP_500_INTERNAL_SERVER_ERROR;
        *numDataP = count;
        for (int i = 0; i < count; i++) (*dataArrayP)[i].id = resources[i];
    }

    for (int i = 0; i < *numDataP; i++) {
        switch ((*dataArrayP)[i].id) {
            case RES_BUTTON_STATE:
                lwm2m_data_encode_bool(s_button.pressed, (*dataArrayP) + i);
                break;
            case RES_BUTTON_COUNTER:
                lwm2m_data_encode_int(s_button.counter, (*dataArrayP) + i);
                break;
            default:
                return COAP_404_NOT_FOUND;
        }
    }

    return COAP_205_CONTENT;
}

lwm2m_object_t *get_button_object(void)
{
    if (s_button_obj) return s_button_obj;

    lwm2m_object_t *obj = (lwm2m_object_t *)lwm2m_malloc(sizeof(lwm2m_object_t));
    if (!obj) return NULL;
    memset(obj, 0, sizeof(lwm2m_object_t));
    obj->objID = LWM2M_OBJ_BUTTON;
    obj->instanceList = (lwm2m_list_t *)lwm2m_malloc(sizeof(lwm2m_list_t));
    if (!obj->instanceList) {
        lwm2m_free(obj);
        return NULL;
    }
    memset(obj->instanceList, 0, sizeof(lwm2m_list_t));
    obj->readFunc = prv_read;
    obj->userData = &s_button;

    s_button_obj = obj;
    return s_button_obj;
}

esp_err_t button_object_update(bool pressed)
{
    bool changed = (s_button.pressed != pressed);
    s_button.pressed = pressed;
    if (changed && pressed) {
        s_button.counter++;
    }

    if (changed) {
        notify_change(RES_BUTTON_STATE);
        notify_change(RES_BUTTON_COUNTER);
        ESP_LOGI(TAG, "Button %s (count=%" PRIu32 ")", pressed ? "PRESSED" : "RELEASED", s_button.counter);
    }

    return ESP_OK;
}
