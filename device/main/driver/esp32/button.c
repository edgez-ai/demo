#include "button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwm2m/object_button.h"

#define POLL_MS 30
#define DEBOUNCE_COUNT 2

static const char *TAG = "button";
static TaskHandle_t s_button_task = NULL;
static gpio_num_t s_button_gpio = GPIO_NUM_NC;

static void button_task(void *arg)
{
    (void)arg;
    bool last_level = false;
    uint32_t stable = 0;

    while (1) {
        int level = gpio_get_level(s_button_gpio);
        bool pressed = (level == 0); // active low

        if (pressed == last_level) {
            if (stable < DEBOUNCE_COUNT) stable++;
        } else {
            stable = 0;
        }

        if (stable == DEBOUNCE_COUNT) {
            button_object_update(pressed);
            stable++; // avoid repeated notify until change
        }

        last_level = pressed;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

lwm2m_object_t *button_init_and_start(gpio_num_t pin)
{
    /* Create the LwM2M object first so callers can register it even if task already exists */
    lwm2m_object_t *obj = get_button_object();
    if (!obj) {
        ESP_LOGE(TAG, "Failed to allocate LwM2M button object");
        return NULL;
    }

    if (s_button_task) return obj;

    s_button_gpio = pin;

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << s_button_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return NULL;
    }

    BaseType_t res = xTaskCreate(button_task, "btn_poll", 4096, NULL, 5, &s_button_task);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to start button task");
        return NULL;
    }

    ESP_LOGI(TAG, "Button monitor started on GPIO %d", s_button_gpio);
    return obj;
}
