#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "prov.h"
#include "lwm2m_client.h"
#include "button.h"
#include "lwm2m/object_log.h"
#include "sampling.h"
#include "factory_data.h"

static const char *TAG = "main";

#ifdef CONFIG_ENABLE_MM_HALOW
#ifndef CONFIG_MM_EXPERIMENTAL_MESH_CHAN
#define CONFIG_MM_EXPERIMENTAL_MESH_CHAN 0
#endif
#endif

static bool s_lwm2m_init_started = false;
static bool s_lwm2m_started = false;
static sample_sleep_mode_t s_lwm2m_init_sleep_mode = SAMPLE_SLEEP_MODE_NO;
static TimerHandle_t s_lwm2m_ready_watchdog = NULL;
static TickType_t s_lwm2m_not_ready_since = 0;

#define LWM2M_READY_WATCHDOG_MS 180000
#define LWM2M_READY_WATCHDOG_POLL_MS 1000
#define LWM2M_NETWORK_READY_TIMEOUT_MS 15000

#define RESET_BUTTON_GPIO GPIO_NUM_0
#define RESET_HOLD_MS     5000
#define RESET_POLL_MS     50
#define RESET_TASK_STACK_SIZE 4096

#define FACTORY_RESET_MAGIC 0xA5F0C1E5u

static RTC_NOINIT_ATTR uint32_t s_factory_reset_magic;
static RTC_NOINIT_ATTR uint32_t s_factory_reset_magic_inv;

void lwm2m_client_on_registration_ota_needed(const char *client_version, const char *server_version)
{
    esp_err_t ota_err;

    ESP_LOGW(TAG,
             "[ota_check] callback(main): registration OTA mismatch detected (client_version=%s server_version=%s)",
             (client_version && client_version[0] != '\0') ? client_version : "unknown",
             (server_version && server_version[0] != '\0') ? server_version : "unknown");

    ota_err = lwm2m_client_trigger_pull_ota_for_version(server_version);
    if (ota_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "[ota_check] callback(main): OTA trigger accepted (target_version=%s)",
                 (server_version && server_version[0] != '\0') ? server_version : "unknown");
    } else {
        ESP_LOGW(TAG,
                 "[ota_check] callback(main): OTA trigger failed err=%s (target_version=%s)",
                 esp_err_to_name(ota_err),
                 (server_version && server_version[0] != '\0') ? server_version : "unknown");
    }
}

static void gpio0_long_press_reset_task(void *arg)
{
    (void)arg;

    const TickType_t poll_ticks = pdMS_TO_TICKS(RESET_POLL_MS);
    TickType_t pressed_since = 0;

    while (1) {
        bool pressed = (gpio_get_level(RESET_BUTTON_GPIO) == 0);

        if (pressed) {
            if (pressed_since == 0) {
                pressed_since = xTaskGetTickCount();
            } else {
                TickType_t held = xTaskGetTickCount() - pressed_since;
                if (held >= pdMS_TO_TICKS(RESET_HOLD_MS)) {
                    ESP_LOGW(TAG, "GPIO0 held for %d ms, scheduling factory reset (NVS erase)", RESET_HOLD_MS);
                    s_factory_reset_magic = FACTORY_RESET_MAGIC;
                    s_factory_reset_magic_inv = ~FACTORY_RESET_MAGIC;
                    pressed_since = 0;
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                }
            }
        } else {
            pressed_since = 0;
        }

        vTaskDelay(poll_ticks);
    }
}

static void start_gpio0_long_press_reset_monitor(void)
{
    const gpio_config_t reset_gpio_cfg = {
        .pin_bit_mask = (1ULL << RESET_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&reset_gpio_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO0 reset button: %s", esp_err_to_name(err));
        return;
    }

    BaseType_t created = xTaskCreate(gpio0_long_press_reset_task,
                                     "gpio0_reset",
                                     RESET_TASK_STACK_SIZE,
                                     NULL,
                                     5,
                                     NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPIO0 reset monitor task");
        return;
    }

    ESP_LOGI(TAG, "GPIO0 long-press reset enabled (hold %d ms)", RESET_HOLD_MS);
}

static void configure_runtime_log_levels(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("lwm2m_client", ESP_LOG_DEBUG);
    esp_log_level_set("wakaama", ESP_LOG_WARN);
    esp_log_level_set("wakaama_core", ESP_LOG_WARN);
    esp_log_level_set("wakaama_reg", ESP_LOG_WARN);
    esp_log_level_set("wakaama_tx", ESP_LOG_WARN);
    esp_log_level_set("wakaama_udp", ESP_LOG_WARN);
    esp_log_level_set("udp_wrapper", ESP_LOG_WARN);
    esp_log_level_set("lwm2m", ESP_LOG_DEBUG);

    esp_log_level_set("spi_master", ESP_LOG_WARN);
    esp_log_level_set("spi", ESP_LOG_WARN);
    esp_log_level_set("gdma", ESP_LOG_WARN);
    esp_log_level_set("gpio", ESP_LOG_INFO);
}

static const char *factory_mode_to_string(uint32_t mode)
{
    switch (mode) {
    case 1:
        return "mesh";
    case 2:
        return "ap";
    case 3:
        return "wifi";
    default:
        return "unspecified";
    }
}

static void lwm2m_ready_watchdog_cb(TimerHandle_t xTimer)
{
    (void)xTimer;

    sample_sleep_mode_t current_sleep_mode = sample_get_sleep_mode();
    bool lwm2m_ready = lwm2m_is_ready();

    if (current_sleep_mode != SAMPLE_SLEEP_MODE_NO || lwm2m_ready) {
        s_lwm2m_not_ready_since = 0;
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (s_lwm2m_not_ready_since == 0) {
        s_lwm2m_not_ready_since = now;
        return;
    }

    TickType_t held = now - s_lwm2m_not_ready_since;
    if (held >= pdMS_TO_TICKS(LWM2M_READY_WATCHDOG_MS)) {
        ESP_LOGE(TAG, "LwM2M not ready after %d ms in non-sleep mode, restarting",
                 LWM2M_READY_WATCHDOG_MS);
        esp_restart();
    }
}

static void start_lwm2m_ready_watchdog(sample_sleep_mode_t sleep_mode)
{
    if (sleep_mode != SAMPLE_SLEEP_MODE_NO) {
        ESP_LOGI(TAG, "LwM2M readiness watchdog skipped (sleep mode=%u)", (unsigned)sleep_mode);
        return;
    }

    if (s_lwm2m_ready_watchdog == NULL) {
        s_lwm2m_ready_watchdog = xTimerCreate("lwm2m_wd",
                                              pdMS_TO_TICKS(LWM2M_READY_WATCHDOG_POLL_MS),
                                              pdTRUE,
                                              NULL,
                                              lwm2m_ready_watchdog_cb);
        if (s_lwm2m_ready_watchdog == NULL) {
            ESP_LOGE(TAG, "Failed to create LwM2M readiness watchdog timer");
            return;
        }
    }

    if (xTimerStart(s_lwm2m_ready_watchdog, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start LwM2M readiness watchdog timer");
        return;
    }

    ESP_LOGI(TAG,
             "LwM2M readiness watchdog enabled (poll=%d ms, timeout=%d ms)",
             LWM2M_READY_WATCHDOG_POLL_MS,
             LWM2M_READY_WATCHDOG_MS);
}

static void network_lwm2m_task(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_OK;
    sample_sleep_mode_t sleep_mode = s_lwm2m_init_sleep_mode;

    const TickType_t backoff_min = pdMS_TO_TICKS(2000);
    const TickType_t backoff_max = pdMS_TO_TICKS(30000);
    TickType_t backoff = backoff_min;

    log_object_install_hook();

    while (!s_lwm2m_started) {
        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
            goto retry;
        }
#ifdef CONFIG_ENABLE_MM_HALOW
        extern bool g_lwip_initialized_by_esp_netif;
        g_lwip_initialized_by_esp_netif = true;
#endif

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
            goto retry;
        }

        err = wifi_prov_init_and_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "wifi_prov_init_and_start failed: %s", esp_err_to_name(err));
            goto retry;
        }

        ESP_LOGI(TAG, "Waiting for Wi-Fi to be provisioned and connected...");
        err = wifi_prov_wait_connected();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi_prov_wait_connected failed: %s", esp_err_to_name(err));
            goto retry;
        }
        ESP_LOGI(TAG, "Wi-Fi connected successfully");

        esp_err_t net_ready_err = wifi_prov_wait_network_ready_timeout(LWM2M_NETWORK_READY_TIMEOUT_MS);
        if (net_ready_err != ESP_OK && net_ready_err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Network readiness check failed: %s", esp_err_to_name(net_ready_err));
            goto retry;
        }
        if (net_ready_err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG,
                     "Network readiness timeout after %d ms; retrying before LwM2M start",
                     LWM2M_NETWORK_READY_TIMEOUT_MS);
            goto retry;
        }

        lwm2m_connection_type_t conn_type = wifi_prov_get_connection_type();


        /* ESP32-S3 and HT-HC33: Only button on GPIO0 */
        lwm2m_object_t *button_obj = button_init_and_start(GPIO_NUM_0);
        if (button_obj) {
            lwm2m_client_add_object(button_obj);
        } else {
            ESP_LOGW(TAG, "Button object not available; skipping LwM2M registration");
        }


        err = lwm2m_client_start(conn_type);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "lwm2m_client_start failed: %s", esp_err_to_name(err));
            goto retry;
        }

        s_lwm2m_started = true;
        s_lwm2m_init_started = false;
        ESP_LOGI(TAG, "LwM2M client started successfully");
        break;

retry:
        ESP_LOGW(TAG, "Retrying network/LwM2M init in %lu ms", (unsigned long)(backoff * portTICK_PERIOD_MS));
        vTaskDelay(backoff);
        if (backoff < backoff_max) {
            TickType_t next = backoff * 2;
            backoff = (next > backoff_max) ? backoff_max : next;
        }
    }

    vTaskDelete(NULL);
}

static void init_network_and_lwm2m(sample_sleep_mode_t sleep_mode)
{
    if (s_lwm2m_started || s_lwm2m_init_started) return;

    s_lwm2m_init_sleep_mode = sleep_mode;
    s_lwm2m_init_started = true;
    BaseType_t created = xTaskCreate(network_lwm2m_task,
                                     "net_lwm2m_task",
                                     8192,
                                     NULL,
                                     5,
                                     NULL);
    if (created != pdPASS) {
        s_lwm2m_init_started = false;
        ESP_LOGE(TAG, "Failed to create network/LwM2M init task");
        return;
    }

    ESP_LOGI(TAG, "Network/LwM2M init started in background task");
}

void app_main(void)
{
    factory_data_config_t factory_cfg = {0};

    configure_runtime_log_levels();

    bool factory_reset_requested =
        (s_factory_reset_magic == FACTORY_RESET_MAGIC) &&
        (s_factory_reset_magic_inv == (uint32_t)~FACTORY_RESET_MAGIC) &&
        (esp_reset_reason() == ESP_RST_SW);

    if (factory_reset_requested) {
        ESP_LOGW(TAG, "Factory reset requested, erasing NVS on boot");
        s_factory_reset_magic = 0;
        s_factory_reset_magic_inv = 0;

        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_LOGI(TAG, "Factory reset complete");
    } else {
        s_factory_reset_magic = 0;
        s_factory_reset_magic_inv = 0;
        // Initialize NVS
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    start_gpio0_long_press_reset_monitor();

    esp_err_t factory_err = factory_data_load(&factory_cfg);
    if (factory_err == ESP_OK && factory_cfg.present) {
        ESP_LOGI(TAG,
                 "Factory mode decoded: %u (%s), country=%u",
                 (unsigned)factory_cfg.mode,
                 factory_mode_to_string(factory_cfg.mode),
                 (unsigned)factory_cfg.country);

        char factory_uri[128] = {0};
        if (factory_data_build_lwm2m_uri(&factory_cfg, factory_uri, sizeof(factory_uri)) == ESP_OK) {
            esp_err_t uri_err = lwm2m_client_set_server_uri_override(factory_uri);
            if (uri_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to apply factory LwM2M URI override: %s", esp_err_to_name(uri_err));
            }
        }

#ifdef CONFIG_ENABLE_MM_HALOW
        char halow_country_code[3] = {0};
        if (factory_data_country_to_code(&factory_cfg, halow_country_code) == ESP_OK) {
            esp_err_t country_err = wifi_prov_set_halow_country_code(halow_country_code);
            if (country_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to apply factory HaLow country code %s: %s",
                         halow_country_code, esp_err_to_name(country_err));
            }
        }

        if (factory_data_is_wifi_only(&factory_cfg)) {
            ESP_LOGI(TAG, "Factory config requests Wi-Fi mode, clearing persisted HaLow credentials");
            esp_err_t clear_err = wifi_prov_clear_halow_credentials();
            if (clear_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to clear HaLow credentials: %s", esp_err_to_name(clear_err));
            }
        } else if (factory_data_is_mesh_mode(&factory_cfg)) {
            esp_err_t mesh_profile_err = wifi_prov_set_startup_halow_mesh_profile(true,
                                                                                   CONFIG_MM_EXPERIMENTAL_MESH_CHAN,
                                                                                   1);
            if (mesh_profile_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to enable startup HaLow mesh profile: %s",
                         esp_err_to_name(mesh_profile_err));
            } else {
                ESP_LOGI(TAG,
                         "Factory config requests mesh mode; provisioning will use dedicated HaLow mesh bootstrap");
            }
        }
#endif
    } else if (factory_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Factory data not applied: %s", esp_err_to_name(factory_err));
    }


    const gpio_config_t power_gpio_cfg = {
        .pin_bit_mask = (1ULL << GPIO_NUM_13) | (1ULL << GPIO_NUM_14),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&power_gpio_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Restarting due to critical GPIO init failure");
        esp_restart();
    }
    err = gpio_set_level(GPIO_NUM_13, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed setting GPIO13 low: %s", esp_err_to_name(err));
    }
    err = gpio_set_level(GPIO_NUM_14, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed setting GPIO14 low: %s", esp_err_to_name(err));
    }

    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG,
             "Loading sample settings from NVS at boot (reset_reason=%d, rtc_ready=%d)",
             (int)reset_reason,
             sample_is_rtc_cache_ready() ? 1 : 0);
    sample_settings_load_from_nvs_to_rtc();

    sample_sleep_mode_t sleep_mode = sample_get_sleep_mode();
    ESP_LOGI(TAG, "Sampling is disabled based on NVS settings");
    init_network_and_lwm2m(sleep_mode);
    start_lwm2m_ready_watchdog(sleep_mode);
    ESP_LOGI(TAG, "Edge device initialization complete");

    if (sample_is_sampling_enabled()) {
        ESP_LOGI(TAG, "Sampling is enabled based on NVS settings");
        const char *sleep_mode_name = sample_sleep_mode_to_string(sleep_mode);

        if (sleep_mode == SAMPLE_SLEEP_MODE_NO) {
            sample_set_startup_network_guard(false);
            ESP_LOGI(TAG, "Sleep mode is %u (%s), starting sampling task + periodic reporting loop",
                     (unsigned)sleep_mode, sleep_mode_name);
            sample_start_no_sleep_tasks();
            ESP_LOGI(TAG, "Edge device initialization complete");
            return;
        } else if (sleep_mode == SAMPLE_SLEEP_MODE_LIGHT) {
            sample_set_startup_network_guard(false);
            ESP_LOGI(TAG, "Sleep mode is %u (%s), entering wake-window scheduler",
                     (unsigned)sleep_mode, sleep_mode_name);
            sample_run_light_sleep_mode_loop();
            return;
        } else if (sleep_mode == SAMPLE_SLEEP_MODE_DEEP) {
            ESP_LOGI(TAG, "Sleep mode is %u (%s), running deep-sleep cycle", (unsigned)sleep_mode, sleep_mode_name);
            sample_run_deep_sleep_mode_cycle();
            return;
        } else {
            ESP_LOGW(TAG, "Invalid sleep mode %u, defaulting to no sleep", (unsigned)sleep_mode);
        }
    }


}
