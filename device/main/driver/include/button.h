#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

/* Forward declaration of wakaama object type to avoid heavy includes */
struct _lwm2m_object_t;
typedef struct _lwm2m_object_t lwm2m_object_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the button GPIO/task and return the LwM2M object backing it (NULL on error). */
lwm2m_object_t *button_init_and_start(gpio_num_t pin);

#ifdef __cplusplus
}
#endif
