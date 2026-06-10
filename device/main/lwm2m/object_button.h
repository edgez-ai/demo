#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lwm2mclient.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LWM2M_OBJ_BUTTON 3347
#define RES_BUTTON_STATE 5500
#define RES_BUTTON_COUNTER 5501

lwm2m_object_t *get_button_object(void);

esp_err_t button_object_update(bool pressed);

#ifdef __cplusplus
}
#endif
