/**
 * @file lwm2m_client_esp32.c
 * @brief ESP32-specific LwM2M client implementation
 * 
 * This implementation is specific to ESP32 series chips.
 */

#include "lwm2m_client.h"

#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <esp_netif.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "esp_task_wdt.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwm2mclient.h"
#include "dtlsconnection.h"
#include "dtls_debug.h"
#include "er-coap-13/er-coap-13.h"
#include "custom_coap.h"  // Now from main/lwm2m/
#include "object_gateway.h"

#include "udp_wrapper.h"
#include "lwm2m/object_interface.h"
#include "lwm2m/object_log.h"
#include "lwm2m/object_sample.h"
#include "lwm2m/object_script.h"
#include "interface_bridge.h"
#include "prov.h"
#include "factory_data.h"

#define UNUSED_ATTR __attribute__((unused))

/* Global variable to track connection type */
static lwm2m_connection_type_t s_connection_type = LWM2M_CONN_TYPE_WIFI;

/* Keep RTC persisted data across deep sleep resets */
RTC_DATA_ATTR char rtc_lwm2m_server_uri[128] = {0};
RTC_DATA_ATTR char rtc_lwm2m_identity[64] = {0};
RTC_DATA_ATTR char rtc_lwm2m_psk[64] = {0};
RTC_DATA_ATTR uint32_t rtc_server_sec_of_year = 0;
RTC_DATA_ATTR uint8_t rtc_server_sec_of_year_valid = 0;
static client_data_t client_data = {0};
static uint64_t s_server_sec_last_update_local_us = 0;
static char s_last_ota_checked_server_version[64] = {0};
static bool s_last_ota_checked_needed = false;
/*
 * ESP32-C6 has a much smaller LP memory region, so keeping this large buffer
 * in RTC_FAST causes the lp_reserved segment to overflow at link time.
 * We only need regular RAM for this buffer, so keep RTC_FAST for other chips
 * but place it in DRAM on C6.
 */
#if CONFIG_IDF_TARGET_ESP32C6
static uint8_t proto_buffer[8000] __attribute__((unused)); /* Buffer for lwm2m proto model */
#else
RTC_FAST_ATTR uint8_t proto_buffer[8000] __attribute__((unused)); /* Buffer for lwm2m proto model */
#endif

static const char *TAG = "lwm2m_client";
static UNUSED_ATTR const char *LOCAL_PORT = "56830";
//static const char *LWM2M_GROUP_ID = "12345"; /* Hard-coded group identifier */
static const bool s_enable_lwm2m_task_wdt = false;

const char *lwm2m_client_get_firmware_version(void)
{
    return CONFIG_LWM2M_DEVICE_FIRMWARE_VERSION;
}

void lwm2m_client_advance_server_sec_of_year(uint64_t extra_us)
{
    if (!rtc_server_sec_of_year_valid) {
        return;
    }

    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint64_t elapsed_us = now_us;
    if (s_server_sec_last_update_local_us > 0 && now_us >= s_server_sec_last_update_local_us) {
        elapsed_us = now_us - s_server_sec_last_update_local_us;
    }

    uint64_t total_us = elapsed_us + extra_us;
    uint32_t delta_sec = (uint32_t)(total_us / 1000000ULL);
    uint64_t rem_us = total_us % 1000000ULL;

    if (delta_sec > 0) {
        uint32_t old_sec = rtc_server_sec_of_year;
        rtc_server_sec_of_year += delta_sec;
        ESP_LOGI(TAG,
                 "Advanced RTC server_sec_of_year by %u sec (extra_us=%llu): %u -> %u",
                 (unsigned)delta_sec,
                 (unsigned long long)extra_us,
                 (unsigned)old_sec,
                 (unsigned)rtc_server_sec_of_year);
    }

    if (now_us >= rem_us) {
        s_server_sec_last_update_local_us = now_us - rem_us;
    } else {
        s_server_sec_last_update_local_us = now_us;
    }
}

bool lwm2m_client_get_server_sec_of_year(uint32_t *sec_of_year_out)
{
    if (!sec_of_year_out || !rtc_server_sec_of_year_valid) {
        return false;
    }

    *sec_of_year_out = rtc_server_sec_of_year;
    return true;
}

/* NVS namespace for LwM2M bootstrap credentials */
#define NVS_NAMESPACE_LWM2M "lwm2m_boot"
#define NVS_KEY_SERVER_URI "server_uri"
#define NVS_KEY_IDENTITY "identity"
#define NVS_KEY_PSK "psk"
#define NVS_KEY_PSK_LEN "psk_len"
#define NVS_KEY_BOOTSTRAPPED "bootstrapped"
//#define LWM2M_DEFAULT_SERVER_URI "coap://192.168.100.1:5683"

#ifdef ESP_PLATFORM
static bool s_lwm2m_reboot_scheduled = false;

static void lwm2m_reboot_task(void *arg)
{
    (void)arg;

    ESP_LOGW(TAG, "LwM2M reboot callback invoked; scheduling restart");

    /* Allow execute response/logs to flush first. */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Wait briefly for in-flight LwM2M traffic to drain. */
    const TickType_t wait_step = pdMS_TO_TICKS(100);
    const TickType_t wait_timeout = pdMS_TO_TICKS(5000);
    TickType_t waited = 0;
    while (!lwm2m_is_idle_for_sleep() && waited < wait_timeout) {
        vTaskDelay(wait_step);
        waited += wait_step;
    }

#ifdef CONFIG_ENABLE_MM_HALOW
    if (wifi_prov_get_connection_type() == LWM2M_CONN_TYPE_HALOW) {
        ESP_LOGI(TAG, "LwM2M reboot: shutting down HaLow before restart");
        wifi_prov_shutdown_halow_for_restart();
    }
#endif

    ESP_LOGW(TAG, "LwM2M reboot: restarting now");
    esp_restart();
}

static void lwm2m_reboot_cb(void)
{
    if (s_lwm2m_reboot_scheduled) {
        return;
    }

    if (xTaskCreate(lwm2m_reboot_task, "lwm2m_reboot", 3072, NULL, 5, NULL) == pdPASS) {
        s_lwm2m_reboot_scheduled = true;
    } else {
        ESP_LOGW(TAG, "Failed to schedule reboot task; restarting immediately");
        esp_restart();
    }
}

static void lwm2m_factory_reset_cb(void)
{
    ESP_LOGW(TAG, "Factory reset callback invoked; erasing NVS and rebooting");
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
    }
    esp_restart();
}
#endif

/**
 * Save LwM2M bootstrap credentials to NVS
 * @param server_uri Server URI (e.g., "coaps://192.168.0.100:5684")
 * @param identity PSK identity
 * @param psk Pre-shared key (binary)
 * @param psk_len Length of PSK
 * @return ESP_OK on success, error code otherwise
 */
static UNUSED_ATTR esp_err_t lwm2m_save_bootstrap_to_nvs(const char *server_uri, const char *identity, 
                                               const uint8_t *psk, size_t psk_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    ESP_LOGI(TAG, "💾 Saving bootstrap credentials to NVS...");
    ESP_LOGI(TAG, "   Server URI: %s", server_uri);
    ESP_LOGI(TAG, "   Identity: %s", identity);
    ESP_LOGI(TAG, "   PSK length: %zu bytes", psk_len);
    
    err = nvs_open(NVS_NAMESPACE_LWM2M, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }
    
    // Save server URI
    err = nvs_set_str(nvs_handle, NVS_KEY_SERVER_URI, server_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save server URI: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save identity
    err = nvs_set_str(nvs_handle, NVS_KEY_IDENTITY, identity);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save identity: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save PSK
    err = nvs_set_blob(nvs_handle, NVS_KEY_PSK, psk, psk_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save PSK: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save PSK length
    err = nvs_set_u32(nvs_handle, NVS_KEY_PSK_LEN, (uint32_t)psk_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save PSK length: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Mark as bootstrapped
    err = nvs_set_u8(nvs_handle, NVS_KEY_BOOTSTRAPPED, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save bootstrap flag: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "✅ Bootstrap credentials saved to NVS successfully");
    return ESP_OK;
}

/**
 * Load LwM2M bootstrap credentials from NVS
 * @param server_uri Buffer to store server URI
 * @param uri_size Size of server_uri buffer
 * @param identity Buffer to store identity
 * @param identity_size Size of identity buffer
 * @param psk Buffer to store PSK
 * @param psk_size Size of PSK buffer (input), actual PSK length (output)
 * @return ESP_OK if credentials found, ESP_ERR_NVS_NOT_FOUND if not bootstrapped
 */
static UNUSED_ATTR esp_err_t lwm2m_load_bootstrap_from_nvs(char *server_uri, size_t uri_size,
                                                 char *identity, size_t identity_size,
                                                 uint8_t *psk, size_t *psk_size)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    uint8_t bootstrapped = 0;
    
    err = nvs_open(NVS_NAMESPACE_LWM2M, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No bootstrap credentials found in NVS (first boot)");
        } else {
            ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        }
        return err;
    }
    
    // Check if bootstrapped
    err = nvs_get_u8(nvs_handle, NVS_KEY_BOOTSTRAPPED, &bootstrapped);
    if (err != ESP_OK || bootstrapped != 1) {
        nvs_close(nvs_handle);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    
    // Load server URI
    err = nvs_get_str(nvs_handle, NVS_KEY_SERVER_URI, server_uri, &uri_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load server URI: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Load identity
    err = nvs_get_str(nvs_handle, NVS_KEY_IDENTITY, identity, &identity_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load identity: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Load PSK length first
    uint32_t saved_psk_len = 0;
    err = nvs_get_u32(nvs_handle, NVS_KEY_PSK_LEN, &saved_psk_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load PSK length: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    if (saved_psk_len > *psk_size) {
        ESP_LOGE(TAG, "PSK buffer too small: need %lu, have %zu", saved_psk_len, *psk_size);
        nvs_close(nvs_handle);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Load PSK
    *psk_size = saved_psk_len;
    err = nvs_get_blob(nvs_handle, NVS_KEY_PSK, psk, psk_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load PSK: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "📂 Loaded bootstrap credentials from NVS:");
    ESP_LOGI(TAG, "   Server URI: %s", server_uri);
    ESP_LOGI(TAG, "   Identity: %s", identity);
    ESP_LOGI(TAG, "   PSK length: %zu bytes", *psk_size);
    
    return ESP_OK;
}

/**
 * Clear bootstrap credentials from NVS (force re-bootstrap)
 * @return ESP_OK on success
 */
static UNUSED_ATTR esp_err_t lwm2m_clear_bootstrap_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(NVS_NAMESPACE_LWM2M, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    nvs_erase_all(nvs_handle);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "🗑️  Cleared bootstrap credentials from NVS");
    return ESP_OK;
}

/* Force bootstrap flag to false so client starts unauthenticated */
static esp_err_t lwm2m_mark_not_bootstrapped(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_LWM2M, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace for bootstrap flag: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(nvs_handle, NVS_KEY_BOOTSTRAPPED, 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear bootstrap flag: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Bootstrap flag set to false (unauthenticated start)");
    }

    nvs_close(nvs_handle);
    return err;
}

static bool lwm2m_is_valid_server_uri(const char *uri)
{
    if (!uri || uri[0] == '\0') {
        return false;
    }

    return (strncmp(uri, "coap://", 7) == 0) ||
           (strncmp(uri, "coaps://", 8) == 0);
}

esp_err_t lwm2m_client_set_server_uri_override(const char *server_uri)
{
    if (!lwm2m_is_valid_server_uri(server_uri)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_LWM2M, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open LwM2M NVS for URI override: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_SERVER_URI, server_uri);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        strncpy(rtc_lwm2m_server_uri, server_uri, sizeof(rtc_lwm2m_server_uri) - 1);
        rtc_lwm2m_server_uri[sizeof(rtc_lwm2m_server_uri) - 1] = '\0';
        ESP_LOGI(TAG, "Applied LwM2M server URI override: %s", server_uri);
    } else {
        ESP_LOGW(TAG, "Failed to apply LwM2M URI override: %s", esp_err_to_name(err));
    }

    return err;
}

static esp_err_t lwm2m_load_server_uri_from_nvs(char *server_uri, size_t uri_size)
{
    if (!server_uri || uri_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_LWM2M, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "LwM2M NVS namespace not found, server URI unavailable in NVS");
            return ESP_ERR_NVS_NOT_FOUND;
        }

        ESP_LOGW(TAG, "LwM2M NVS namespace unavailable for server URI: %s", esp_err_to_name(err));
        return err;
    }

    size_t required_size = uri_size;
    err = nvs_get_str(nvs_handle, NVS_KEY_SERVER_URI, server_uri, &required_size);
    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "LwM2M server URI key missing in NVS");
            return ESP_ERR_NVS_NOT_FOUND;
        }

        ESP_LOGW(TAG, "LwM2M server URI not found in NVS: %s", esp_err_to_name(err));
        return err;
    }

    if (!lwm2m_is_valid_server_uri(server_uri)) {
        ESP_LOGW(TAG, "LwM2M server URI in NVS is empty or invalid: %s", server_uri);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t lwm2m_build_uri_from_gateway_ip(char *out_uri, size_t out_uri_len)
{
    if (!out_uri || out_uri_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char gateway_ipv4[16] = {0};
    esp_err_t gw_err = wifi_prov_get_active_gateway_ipv4(gateway_ipv4, sizeof(gateway_ipv4));
    if (gw_err != ESP_OK) {
        return gw_err;
    }

    int written = snprintf(out_uri,
                           out_uri_len,
                           "coap://%s:5683",
                           gateway_ipv4);
    if (written <= 0 || (size_t)written >= out_uri_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static float s_temperature_c = 0.0f; /* updated via public setter */
/* serialNumber is referenced by wakaama registration logic, needs external linkage */
char serialNumber[64] = {0};
uint8_t public_key[64] = {0};
uint8_t private_key[64] = {0};
uint8_t vendor_public_key[32] = {0};
size_t public_key_len = 0;
size_t private_key_len = 0;
char psk_key[64] = "";  /* empty -> no DTLS credentials */
char server[128] = {0};
#define LWM2M_MAX_OPTIONAL_HW 12
static lwm2m_object_t *s_optional_objects[LWM2M_MAX_OPTIONAL_HW];
static size_t s_optional_count = 0;

#define LWM2M_OBJ_COUNT 9
lwm2m_object_t *lwm2m_obj_array[LWM2M_OBJ_COUNT] = {0};  /* Security, Server, Device, Vendor/Test placeholders, Gateway, MQTT, Firmware */

static lwm2m_object_t **s_lwm2m_objects = NULL;
static size_t s_lwm2m_obj_count = 0;
static lwm2m_object_t *s_security_obj = NULL;
static UNUSED_ATTR lwm2m_object_t *s_server_obj = NULL;
static lwm2m_object_t *s_device_obj = NULL;
static UNUSED_ATTR uint8_t rx_buffer[4096];
static RTC_DATA_ATTR UNUSED_ATTR struct timeval sleep_enter_time;
static void (*s_lwm2m_ready_callback)(void) = NULL;  // Callback for when LwM2M is ready
static SemaphoreHandle_t s_lwm2m_mutex = NULL;       // Serializes LwM2M core access
static volatile bool s_lwm2m_io_in_progress = false;
static volatile TickType_t s_last_lwm2m_io_tick = 0;
static SemaphoreHandle_t s_sample_send_ack_sem = NULL;
static volatile uint32_t s_sample_send_wait_seq = 0;
static volatile bool s_sample_send_waiting = false;
static volatile bool s_sample_send_acked = false;
static TickType_t s_last_lwm2m_step_error_log_tick = 0;
static volatile bool s_lwm2m_socket_reinit_requested = false;
static volatile bool s_lwm2m_stop_requested = false;
static volatile bool s_lwm2m_stop_without_deregister_requested = false;
static volatile bool s_lwm2m_task_running = false;
static TaskHandle_t s_lwm2m_task_handle = NULL;
static bool s_internal_optional_objects_registered = false;
static bool lwm2m_object_already_registered(lwm2m_object_t *const *objects, size_t count, const lwm2m_object_t *candidate)
{
    if (!objects || !candidate) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        const lwm2m_object_t *existing = objects[i];
        if (!existing) {
            continue;
        }
        if (existing == candidate || existing->objID == candidate->objID) {
            return true;
        }
    }

    return false;
}

static bool lwm2m_append_unique_object(lwm2m_object_t **objects, size_t capacity, size_t *count, lwm2m_object_t *candidate, const char *source)
{
    if (!objects || !count || !candidate) {
        return false;
    }

    if (*count >= capacity) {
        ESP_LOGW(TAG, "LwM2M object list full while appending obj=%u from %s", (unsigned)candidate->objID, source ? source : "unknown");
        return false;
    }

    if (lwm2m_object_already_registered((lwm2m_object_t *const *)objects, *count, candidate)) {
        ESP_LOGW(TAG, "Skipping duplicate LwM2M object obj=%u from %s", (unsigned)candidate->objID, source ? source : "unknown");
        return false;
    }

    objects[(*count)++] = candidate;
    return true;
}

#ifndef CONFIG_LWM2M_SLEEP_IDLE_GRACE_MS
#define CONFIG_LWM2M_SLEEP_IDLE_GRACE_MS 200
#endif

// Forward declaration of callback function
static void gateway_device_update_callback(uint32_t device_id, uint16_t new_instance_id);
static void gateway_device_delete_callback(uint32_t device_id, uint16_t instance_id);
static void gateway_psk_write_callback(uint32_t device_id, uint16_t instance_id, const char *identity, const uint8_t *psk, size_t psk_length, const char *server);

static inline void mark_lwm2m_io_activity(void)
{
    s_last_lwm2m_io_tick = xTaskGetTickCount();
}

static void lwm2m_sample_send_callback(lwm2m_context_t *contextP, lwm2m_transaction_t *transacP, void *message)
{
    (void)contextP;

    uint32_t callback_seq = (uint32_t)(uintptr_t)(transacP ? transacP->userData : NULL);
    bool done = false;
    bool acked = false;

    if (message != NULL && transacP != NULL) {
        coap_packet_t *packet = (coap_packet_t *)message;

        // Block1 upload in progress. Do not signal completion yet.
        if (packet->code == COAP_231_CONTINUE) {
            ESP_LOGI(TAG,
                     "Sample send in progress (2.31 Continue), waiting for final response: seq=%lu mid=%u",
                     (unsigned long)callback_seq,
                     (unsigned)transacP->mID);
        } else {
            done = true;
            acked = (packet->code == COAP_201_CREATED ||
                     packet->code == COAP_202_DELETED ||
                     packet->code == COAP_204_CHANGED ||
                     packet->code == COAP_205_CONTENT);
            ESP_LOGI(TAG,
                     "Sample send final response: seq=%lu mid=%u code=0x%02X acked=%u",
                     (unsigned long)callback_seq,
                     (unsigned)transacP->mID,
                     (unsigned)packet->code,
                     (unsigned)acked);
        }
    } else {
        done = true;
        ESP_LOGW(TAG, "Sample send callback without response packet (timeout/reset likely): seq=%lu",
                 (unsigned long)callback_seq);
    }

    if (done && s_sample_send_waiting && callback_seq == s_sample_send_wait_seq) {
        s_sample_send_acked = acked;
        if (s_sample_send_ack_sem) {
            xSemaphoreGive(s_sample_send_ack_sem);
        }
    }
}

static void lwm2m_reset_server_sessions(lwm2m_context_t *ctx)
{
    if (!ctx) {
        return;
    }

    for (lwm2m_server_t *server = ctx->serverList; server != NULL; server = server->next) {
        if (server->sessionH != NULL) {
            lwm2m_close_connection(server->sessionH, ctx->userData);
            server->sessionH = NULL;
        }
        server->status = STATE_DEREGISTERED;
        server->registration = 0;
        ESP_LOGI(TAG, "Reset LwM2M server session: shortID=%u secInst=%u", server->shortID, server->secObjInstID);
    }

    for (lwm2m_server_t *bs = ctx->bootstrapServerList; bs != NULL; bs = bs->next) {
        if (bs->sessionH != NULL) {
            lwm2m_close_connection(bs->sessionH, ctx->userData);
            bs->sessionH = NULL;
        }
        bs->status = STATE_DEREGISTERED;
        bs->registration = 0;
        ESP_LOGI(TAG, "Reset bootstrap session: shortID=%u secInst=%u", bs->shortID, bs->secObjInstID);
    }
}

static esp_err_t lwm2m_reopen_udp_socket(udp_socket_wrapper_t **udp_sock)
{
    if (!udp_sock) {
        return ESP_ERR_INVALID_ARG;
    }

    if (*udp_sock) {
        udp_socket_close(*udp_sock);
        *udp_sock = NULL;
    }

    if (client_data.lwm2mH) {
        lwm2m_reset_server_sessions(client_data.lwm2mH);
    }

    if (client_data.connList != NULL) {
        connection_free(client_data.connList);
        client_data.connList = NULL;
    }

    udp_socket_wrapper_t *new_sock = udp_socket_create(LOCAL_PORT, client_data.addressFamily, s_connection_type);
    if (!new_sock || new_sock->sock_fd < 0) {
        return ESP_FAIL;
    }

    int flags = lwip_fcntl(new_sock->sock_fd, F_GETFL, 0);
    lwip_fcntl(new_sock->sock_fd, F_SETFL, flags | O_NONBLOCK);

    *udp_sock = new_sock;
    client_data.sock = new_sock->sock_fd;

    if (client_data.lwm2mH) {
        client_data.lwm2mH->state = STATE_INITIAL;
        ESP_LOGI(TAG, "Forced LwM2M state to INITIAL after UDP socket reinit (refresh server list before register)");
    }

    mark_lwm2m_io_activity();
    ESP_LOGI(TAG, "Reinitialized LwM2M UDP socket after network reset, fd=%d", new_sock->sock_fd);
    return ESP_OK;
}

static void register_internal_optional_objects_once(void)
{
    if (s_internal_optional_objects_registered) {
        return;
    }

    lwm2m_object_t *firmware_obj = get_object_firmware();
    if (firmware_obj) {
        if (s_optional_count < LWM2M_MAX_OPTIONAL_HW) {
            s_optional_objects[s_optional_count++] = firmware_obj;
            ESP_LOGI(TAG, "Added Firmware Update object to LwM2M object list");
        } else {
            ESP_LOGW(TAG, "Optional object list full, cannot add Firmware Update object");
            free_object_firmware(firmware_obj);
        }
    } else {
        ESP_LOGW(TAG, "Failed to create Firmware Update object; OTA disabled for this session");
    }

    lwm2m_object_t *log_obj = get_object_log();
    if (log_obj) {
        if (s_optional_count < LWM2M_MAX_OPTIONAL_HW) {
            s_optional_objects[s_optional_count++] = log_obj;
            ESP_LOGI(TAG, "Added Log object to LwM2M object list");
        } else {
            ESP_LOGW(TAG, "Optional object list full, cannot add Log object");
            free_object_log(log_obj);
        }
    } else {
        ESP_LOGW(TAG, "Failed to create Log object; log streaming disabled");
    }

    lwm2m_object_t *sample_obj = get_sample_object();
    if (sample_obj) {
        if (s_optional_count < LWM2M_MAX_OPTIONAL_HW) {
            s_optional_objects[s_optional_count++] = sample_obj;
            ESP_LOGI(TAG, "Added Sample Config object (version, enabled, rates, sleep, sensors)");
        } else {
            ESP_LOGW(TAG, "Optional object list full, cannot add Sample Config object");
        }
    } else {
        ESP_LOGW(TAG, "Failed to create Sample Config object");
    }

    lwm2m_object_t *script_obj = get_script_object();
    if (script_obj) {
        if (s_optional_count < LWM2M_MAX_OPTIONAL_HW) {
            s_optional_objects[s_optional_count++] = script_obj;
            ESP_LOGI(TAG, "Added Script object (per-script instance IDs, persisted in driver_nvs)");
        } else {
            ESP_LOGW(TAG, "Optional object list full, cannot add Script object");
        }
    } else {
        ESP_LOGW(TAG, "Failed to create Script object");
    }
    
    lwm2m_object_t *i2c_obj = get_i2c_object();
    if (i2c_obj) {
        i2c_instance_cfg_t i2c0 = {
            .enabled = false,
            .open_state = false,
            .i2c_address = 0,
            .mode = 0,
            .stats_window_ms = 0,
            .tx_rate = 0,
            .rx_rate = 0,
            .rx_buffer_size = 4096,
            .tx_pin = 19,
            .rx_pin = 20,
        };

        if (i2c_object_set_instance(0, &i2c0) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to configure I2C interface instance");
        } else if (s_optional_count < LWM2M_MAX_OPTIONAL_HW) {
            s_optional_objects[s_optional_count++] = i2c_obj;
            ESP_LOGI(TAG, "Added I2C interface object (instance 0)");
            interface_bridge_register();
        } else {
            ESP_LOGW(TAG, "Optional object list full, cannot add I2C object");
        }
    } else {
        ESP_LOGW(TAG, "Failed to create I2C interface object");
    }

    lwm2m_object_t *uart_obj = get_uart_object();
    if (uart_obj) {
        uart_instance_cfg_t uart0 = {
            .enabled = false,
            .open_state = false,
            .baudrate = 115200,
            .mode = 0,
            .stats_window_ms = 0,
            .tx_rate = 0,
            .rx_rate = 0,
            .rx_buffer_size = 4096,
            .tx_pin = 19,
            .rx_pin = 20,
        };

        if (uart_object_set_instance(0, &uart0) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to configure UART interface instance");
        } else if (s_optional_count < LWM2M_MAX_OPTIONAL_HW) {
            s_optional_objects[s_optional_count++] = uart_obj;
            ESP_LOGI(TAG, "Added UART interface object (GPIO19/20)");
            interface_bridge_register();
        } else {
            ESP_LOGW(TAG, "Optional object list full, cannot add UART object");
        }
    } else {
        ESP_LOGW(TAG, "Failed to create UART interface object");
    }

    lwm2m_object_t *rs485_obj = get_rs485_object();
    if (rs485_obj) {
        rs485_instance_cfg_t rs485 = {
            .enabled = false,
            .open_state = false,
            .baudrate = 115200,
            .modbus_unit_id = 1,
            .mode = 0,
            .stats_window_ms = 0,
            .tx_rate = 0,
            .rx_rate = 0,
            .rx_buffer_size = 4096,
            .tx_pin = 17,
            .rx_pin = 18,
        };

        if (rs485_object_set_instance(0, &rs485) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to configure RS485 interface instance");
        } else if (s_optional_count < LWM2M_MAX_OPTIONAL_HW) {
            s_optional_objects[s_optional_count++] = rs485_obj;
            ESP_LOGI(TAG, "Added RS485 interface object (GPIO17/18)");
        } else {
            ESP_LOGW(TAG, "Optional object list full, cannot add RS485 object");
        }
    } else {
        ESP_LOGW(TAG, "Failed to create RS485 interface object");
    }



    s_internal_optional_objects_registered = true;
}


/* Internal helpers */
static UNUSED_ATTR void save_security_info_to_rtc(const char *uri, const char *identity, size_t identity_len, const char *psk, size_t psk_len)
{
    if (uri) {
        strncpy(rtc_lwm2m_server_uri, uri, sizeof(rtc_lwm2m_server_uri) - 1);
        rtc_lwm2m_server_uri[sizeof(rtc_lwm2m_server_uri) - 1] = '\0';
    }
    if (identity) {
        memcpy(rtc_lwm2m_identity, identity, MIN(identity_len, sizeof(rtc_lwm2m_identity)));
    }
    if (psk) {
        memcpy(rtc_lwm2m_psk, psk, MIN(psk_len, sizeof(rtc_lwm2m_psk)));
    }
}

static void client_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Client task started");
    s_lwm2m_task_running = true;
    s_lwm2m_stop_requested = false;

    udp_socket_wrapper_t *udp_sock = NULL;
    lwm2m_context_t *client_handle = NULL;

    bool wdt_registered = false;
    if (s_enable_lwm2m_task_wdt) {
        if (esp_task_wdt_add(NULL) == ESP_OK) {
            wdt_registered = true;
            esp_task_wdt_reset();
        } else {
            ESP_LOGW(TAG, "Failed to register LwM2M task with watchdog");
        }
    }
    
    ESP_LOGI(TAG, "Network stack ready, proceeding with LwM2M initialization");
    
    // Parse server URI: strip scheme/port for DNS to avoid blocking watchdog
    const char *uri = server;
    const char *host = uri;
    char host_buf[64] = {0};
    char port_buf[8] = "5683";
    if (strncmp(uri, "coap://", 7) == 0) {
        host = uri + 7;
    } else if (strncmp(uri, "coaps://", 8) == 0) {
        host = uri + 8;
        strncpy(port_buf, "5684", sizeof(port_buf) - 1);
        port_buf[sizeof(port_buf) - 1] = '\0';
    }

    const char *colon = strchr(host, ':');
    size_t host_len = colon ? (size_t)(colon - host) : strnlen(host, sizeof(host_buf) - 1);
    if (host_len >= sizeof(host_buf)) host_len = sizeof(host_buf) - 1;
    memcpy(host_buf, host, host_len);
    host_buf[host_len] = '\0';
    if (colon) {
        strncpy(port_buf, colon + 1, sizeof(port_buf) - 1);
        port_buf[sizeof(port_buf) - 1] = '\0';
    }

    char resolved_ip[64] = {0};
    struct in_addr addr;
    if (inet_aton(host_buf, &addr) != 0) {
        strncpy(resolved_ip, host_buf, sizeof(resolved_ip) - 1);
        ESP_LOGI(TAG, "Server IP: %s", resolved_ip);
    } else {
        ESP_LOGI(TAG, "Resolving hostname: %s", host_buf);
        struct addrinfo hints = {0};
        struct addrinfo *res = NULL;
        hints.ai_family = AF_INET;
        int err = getaddrinfo(host_buf, NULL, &hints, &res);
        if (err == 0 && res) {
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
            strncpy(resolved_ip, inet_ntoa(ipv4->sin_addr), sizeof(resolved_ip) - 1);
            ESP_LOGI(TAG, "DNS resolved: %s -> %s", host_buf, resolved_ip);
            freeaddrinfo(res);
        } else {
            ESP_LOGE(TAG, "DNS resolution failed: %s (err=%d)", host_buf, err);
            strncpy(resolved_ip, host_buf, sizeof(resolved_ip) - 1);
        }
    }
    
    if (wdt_registered) {
        esp_task_wdt_reset();
    }
    
    // Initialize client_data address family
    client_data.addressFamily = AF_INET;
    

    /* Serial number is selected during start(); only guard empty values here. */
    if (serialNumber[0] == '\0') {
        uint8_t mac[6] = {0};
        if (esp_efuse_mac_get_default(mac) == ESP_OK) {
            snprintf(serialNumber, sizeof(serialNumber), "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            snprintf(serialNumber, sizeof(serialNumber), "ESP32DEVICE");
        }
        ESP_LOGW(TAG, "Factory serial unavailable, using MAC-based endpoint: %s", serialNumber);
    }



    // Create security and server objects
    char psk_identity[96] = {0};
    snprintf(psk_identity, sizeof(psk_identity), "%s", serialNumber);
    const uint8_t *psk_bytes = NULL;
    size_t psk_len = 0;
    if (private_key_len > 0) {
        psk_bytes = private_key;
        psk_len = private_key_len;
        ESP_LOGI(TAG, "Using factory private key as DTLS PSK (%u bytes)", (unsigned)psk_len);
    } else {
        psk_len = strlen(psk_key);
        if (psk_len > 0) {
            psk_bytes = (const uint8_t *)psk_key;
            ESP_LOGI(TAG, "Using configured text PSK (%u bytes)", (unsigned)psk_len);
        }
    }

    register_internal_optional_objects_once();

    /* Core + optional objects */
    lwm2m_object_t *gateway_obj = NULL;

    size_t total_slots = 3 + s_optional_count;
    if (gateway_obj) total_slots++;
    s_lwm2m_objects = calloc(total_slots, sizeof(lwm2m_object_t *));
    if (!s_lwm2m_objects) {
        ESP_LOGE(TAG, "Failed to allocate object array (count=%u)", (unsigned)total_slots);
        goto task_cleanup;
    }

    size_t obj_idx = 0;
    s_security_obj = get_security_object(1,
                                         server,
                                         psk_len > 0 ? psk_identity : NULL,
                                         psk_len > 0 ? (const char *)psk_bytes : NULL,
                                         psk_len,
                                         false);
    s_server_obj = get_server_object(1, "U", 300, false);
    s_device_obj = get_object_device();

    if (!s_security_obj || !s_server_obj || !s_device_obj) {
        ESP_LOGE(TAG, "Failed to create mandatory LwM2M objects");
        goto create_fail;
    }

    /* Wire factory-reset execute to platform handler.
     * This fork currently exposes only factory-reset callback registration. */
    lwm2m_device_set_factory_reset_cb(lwm2m_factory_reset_cb);
    ESP_LOGI(TAG, "Registered LwM2M factory reset callback");

    (void)lwm2m_append_unique_object(s_lwm2m_objects, total_slots, &obj_idx, s_security_obj, "mandatory/security");
    (void)lwm2m_append_unique_object(s_lwm2m_objects, total_slots, &obj_idx, s_server_obj, "mandatory/server");
    (void)lwm2m_append_unique_object(s_lwm2m_objects, total_slots, &obj_idx, s_device_obj, "mandatory/device");

    for (size_t i = 0; i < s_optional_count; ++i) {
        if (!s_optional_objects[i]) {
            ESP_LOGW(TAG, "Optional object %u is NULL, skipping", (unsigned)i);
            continue;
        }
        (void)lwm2m_append_unique_object(s_lwm2m_objects, total_slots, &obj_idx, s_optional_objects[i], "optional");
    }

    if (gateway_obj) {
        (void)lwm2m_append_unique_object(s_lwm2m_objects, total_slots, &obj_idx, gateway_obj, "gateway");
    }

    s_lwm2m_obj_count = obj_idx;

    if (s_lwm2m_obj_count < 3) {
        ESP_LOGE(TAG, "LwM2M object list incomplete after dedupe (count=%u)", (unsigned)s_lwm2m_obj_count);
        goto task_cleanup;
    }

    ESP_LOGI(TAG, "Registered LwM2M objects (%u):", (unsigned)s_lwm2m_obj_count);
    for (size_t i = 0; i < s_lwm2m_obj_count; ++i) {
        lwm2m_object_t *obj = s_lwm2m_objects[i];
        if (!obj) continue;
        ESP_LOGI(TAG, "  - Obj %u", (unsigned)obj->objID);
    }

    /* Populate global object pointers for BLE helpers (indices align with gateway build) */
    memset(lwm2m_obj_array, 0, sizeof(lwm2m_obj_array));
    lwm2m_obj_array[0] = s_security_obj;
    lwm2m_obj_array[1] = s_server_obj;
    lwm2m_obj_array[2] = s_device_obj;
    /* Index 5 = Gateway object */
    if (gateway_obj) {
        lwm2m_obj_array[5] = gateway_obj;
    }

    goto objects_ready;

create_fail:
    goto task_cleanup;

objects_ready:
    ESP_LOGI(TAG, "Creating UDP socket on port %s with address family %d, connection type: %d", 
             LOCAL_PORT, client_data.addressFamily, s_connection_type);

    udp_sock = udp_socket_create(LOCAL_PORT, client_data.addressFamily, s_connection_type);
    if (!udp_sock || udp_sock->sock_fd < 0) {
        ESP_LOGE(TAG, "Failed to open UDP socket");
        goto task_cleanup;
    }
    ESP_LOGI(TAG, "UDP socket created successfully, fd=%d", udp_sock->sock_fd);
    
    client_data.sock = udp_sock->sock_fd;
    int flags = lwip_fcntl(client_data.sock, F_GETFL, 0);
    lwip_fcntl(client_data.sock, F_SETFL, flags | O_NONBLOCK);
    
    ESP_LOGI(TAG, "Initializing LwM2M client...");
    if (wdt_registered) {
        esp_task_wdt_reset();
    }
    client_data.securityObjP = s_security_obj;
    client_handle = lwm2m_init(&client_data);
    if (!client_handle) {
        ESP_LOGE(TAG, "Failed to initialize LwM2M client");
        goto task_cleanup;
    }
    if (wdt_registered) {
        esp_task_wdt_reset();
    }
    ESP_LOGI(TAG, "LwM2M client initialized successfully");
    
    client_data.lwm2mH = client_handle;
    char endpoint_name[96] = {0};

    snprintf(endpoint_name, sizeof(endpoint_name), "%s", serialNumber);


    ESP_LOGI(TAG, "Configuring LwM2M client with endpoint: %s", endpoint_name);
    if (lwm2m_configure(client_handle, endpoint_name, NULL, NULL, s_lwm2m_obj_count, s_lwm2m_objects) != 0) {
        ESP_LOGE(TAG, "lwm2m_configure failed");
        goto task_cleanup;
    }
    ESP_LOGI(TAG, "LwM2M client configured successfully");
    // Populate Device object mandatory resources
    device_add_instance(s_device_obj, 0);
    device_update_instance_string(s_device_obj, 0, 2, serialNumber);
    
    static uint8_t rx_buffer[4096];
    int inactivity_counter = 0;
    // Inactivity limit for connection monitoring (currently unused)
    // const int inactivity_limit = 40;
    
    if (wdt_registered) {
        esp_task_wdt_reset();
    }
    lwm2m_client_state_t last_state = client_handle->state;
    ESP_LOGI(TAG, "LwM2M loop start with initial state=%d", last_state);
    if (rtc_server_sec_of_year_valid) {
        ESP_LOGI(TAG, "Restored RTC server_sec_of_year=%u", (unsigned)rtc_server_sec_of_year);
    }
    s_server_sec_last_update_local_us = (uint64_t)esp_timer_get_time();
    s_last_ota_checked_server_version[0] = '\0';
    s_last_ota_checked_needed = false;

    while (1) {
        bool ota_trigger_pending = false;
        char ota_target_version[64] = {0};

        if (s_lwm2m_stop_requested) {
            ESP_LOGI(TAG, "LwM2M stop requested, shutting down client task");
            break;
        }

        if (wdt_registered) {
            esp_task_wdt_reset();
        }

        if (s_lwm2m_socket_reinit_requested) {
            if (!s_lwm2m_mutex || xSemaphoreTake(s_lwm2m_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                esp_err_t reinit_err = lwm2m_reopen_udp_socket(&udp_sock);
                if (reinit_err != ESP_OK) {
                    ESP_LOGW(TAG, "Deferred UDP socket reinit failed: %s", esp_err_to_name(reinit_err));
                }
                s_lwm2m_socket_reinit_requested = false;
                if (s_lwm2m_mutex) {
                    xSemaphoreGive(s_lwm2m_mutex);
                }
            } else {
                ESP_LOGW(TAG, "LwM2M mutex busy; delaying UDP socket reinit");
            }
        }
        
        if (s_lwm2m_mutex && xSemaphoreTake(s_lwm2m_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            time_t tv = lwm2m_gettime();
            s_lwm2m_io_in_progress = true;
            int step_result = lwm2m_step(client_handle, &tv);
            s_lwm2m_io_in_progress = false;
            if (step_result != COAP_NO_ERROR) {
                TickType_t now_tick = xTaskGetTickCount();
                if ((now_tick - s_last_lwm2m_step_error_log_tick) >= pdMS_TO_TICKS(5000)) {
                    ESP_LOGW(TAG, "lwm2m_step error=%d state=%d, forcing registration retry", step_result, client_handle->state);
                    s_last_lwm2m_step_error_log_tick = now_tick;
                }
                client_handle->state = STATE_REGISTER_REQUIRED;
            }
            if (client_handle->state == STATE_READY) {
                log_object_process(client_handle);
                if (lwm2m_registration_server_sec_of_year_is_valid(client_handle)) {
                    uint32_t sec_of_year = lwm2m_registration_server_sec_of_year(client_handle);
                    if (!rtc_server_sec_of_year_valid || rtc_server_sec_of_year != sec_of_year) {
                        rtc_server_sec_of_year = sec_of_year;
                        rtc_server_sec_of_year_valid = 1;
                        s_server_sec_last_update_local_us = (uint64_t)esp_timer_get_time();
                        ESP_LOGI(TAG, "Updated RTC server_sec_of_year=%u", (unsigned)rtc_server_sec_of_year);
                    }
                }

                if (lwm2m_registration_ota_info_is_valid(client_handle)) {
                    bool ota_needed = lwm2m_registration_ota_needed(client_handle);
                    const char *server_version = lwm2m_registration_server_version(client_handle);
                    if (server_version == NULL) {
                        server_version = "";
                    }

                    if ((s_last_ota_checked_needed != ota_needed) ||
                        (strncmp(s_last_ota_checked_server_version,
                                 server_version,
                                 sizeof(s_last_ota_checked_server_version) - 1) != 0)) {
                        s_last_ota_checked_needed = ota_needed;
                        strncpy(s_last_ota_checked_server_version,
                                server_version,
                                sizeof(s_last_ota_checked_server_version) - 1);
                        s_last_ota_checked_server_version[sizeof(s_last_ota_checked_server_version) - 1] = '\0';

                        if (ota_needed && server_version[0] != '\0') {
                            strncpy(ota_target_version, server_version, sizeof(ota_target_version) - 1);
                            ota_target_version[sizeof(ota_target_version) - 1] = '\0';
                            ota_trigger_pending = true;
                        }
                    }
                }
            }
            xSemaphoreGive(s_lwm2m_mutex);
        } else if (s_lwm2m_mutex) {
            ESP_LOGW(TAG, "LwM2M mutex busy; skipping loop iteration");
        }

        if (ota_trigger_pending) {
            esp_err_t ota_err = lwm2m_client_trigger_pull_ota_for_version(ota_target_version);
            if (ota_err == ESP_OK) {
                ESP_LOGI(TAG,
                         "Registration OTA trigger accepted from READY check (target_version=%s)",
                         ota_target_version);
            } else {
                ESP_LOGW(TAG,
                         "Registration OTA trigger from READY check failed err=%s (target_version=%s)",
                         esp_err_to_name(ota_err),
                         ota_target_version);
            }
        }
        
        // Log state changes
        if (client_handle->state != last_state) {
            ESP_LOGI(TAG, "Client state changed: %d -> %d", last_state, client_handle->state);
            
            if (client_handle->state == STATE_READY && s_lwm2m_ready_callback != NULL) {
                ESP_LOGI(TAG, "Triggering LwM2M ready callback...");
                s_lwm2m_ready_callback();
            }
            
            last_state = client_handle->state;
        }
        
        struct sockaddr_storage source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = udp_socket_recv(udp_sock, rx_buffer, sizeof(rx_buffer), (struct sockaddr *)&source_addr, &socklen);
        if (len > 0) {
            s_lwm2m_io_in_progress = true;
            if (s_lwm2m_mutex && xSemaphoreTake(s_lwm2m_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                connection_handle_packet(client_data.connList, rx_buffer, len);
                xSemaphoreGive(s_lwm2m_mutex);
            } else if (s_lwm2m_mutex) {
                ESP_LOGW(TAG, "LwM2M mutex busy; dropping inbound packet");
            }
            s_lwm2m_io_in_progress = false;
            mark_lwm2m_io_activity();
            inactivity_counter = 0;
        } else if (client_handle->state == STATE_READY) {
            inactivity_counter++;
            test_data_t *test_data = s_device_obj ? (test_data_t *)s_device_obj->userData : NULL;
            if (test_data && test_data->test_integer != (int)s_temperature_c) {
                ESP_LOGI(TAG, "Temperature changed, updating resource to %.2f", s_temperature_c);
                test_data->test_integer = (int)s_temperature_c;
                lwm2m_uri_t uri = {.objectId = 3442, .instanceId = 0, .resourceId = 120};
                lwm2m_resource_value_changed(client_handle, &uri);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }

task_cleanup:
    s_lwm2m_socket_reinit_requested = false;

    if (client_data.lwm2mH) {
        if (!s_lwm2m_mutex || xSemaphoreTake(s_lwm2m_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            lwm2m_close(client_data.lwm2mH);
            if (s_lwm2m_mutex) {
                xSemaphoreGive(s_lwm2m_mutex);
            }
        } else {
            ESP_LOGW(TAG, "Timeout taking LwM2M mutex during close; proceeding with cleanup");
        }
        client_data.lwm2mH = NULL;
    }

    if (client_data.connList) {
        connection_free(client_data.connList);
        client_data.connList = NULL;
    }

    if (udp_sock) {
        udp_socket_close(udp_sock);
        udp_sock = NULL;
    }

    if (s_security_obj) {
        clean_security_object(s_security_obj);
        lwm2m_free(s_security_obj);
        s_security_obj = NULL;
    }
    if (s_server_obj) {
        clean_server_object(s_server_obj);
        lwm2m_free(s_server_obj);
        s_server_obj = NULL;
    }
    if (s_device_obj) {
        free_object_device(s_device_obj);
        s_device_obj = NULL;
    }

    if (s_lwm2m_objects) {
        free(s_lwm2m_objects);
        s_lwm2m_objects = NULL;
    }
    s_lwm2m_obj_count = 0;
    memset(lwm2m_obj_array, 0, sizeof(lwm2m_obj_array));
    client_data.sock = -1;
    s_lwm2m_io_in_progress = false;
    mark_lwm2m_io_activity();
    s_lwm2m_task_running = false;
    s_lwm2m_stop_requested = false;
    s_lwm2m_stop_without_deregister_requested = false;
    s_lwm2m_task_handle = NULL;

    if (wdt_registered) {
        (void)esp_task_wdt_delete(NULL);
    }

    vTaskDelete(NULL);
}

void lwm2m_client_set_temperature(float temp_celsius)
{
    s_temperature_c = temp_celsius;
}

void lwm2m_client_add_object(lwm2m_object_t *obj)
{
    if (!obj) {
        ESP_LOGW(TAG, "Attempted to add NULL LwM2M object, ignoring");
        return;
    }

    if (s_optional_count >= LWM2M_MAX_OPTIONAL_HW) {
        ESP_LOGW(TAG, "Optional object list full (%u), ignoring addition", (unsigned)LWM2M_MAX_OPTIONAL_HW);
        return;
    }

    if (lwm2m_object_already_registered((lwm2m_object_t *const *)s_optional_objects, s_optional_count, obj)) {
        ESP_LOGW(TAG, "Ignoring duplicate optional object obj=%u", (unsigned)obj->objID);
        return;
    }

    s_optional_objects[s_optional_count++] = obj;
}

esp_err_t lwm2m_client_start(lwm2m_connection_type_t connection_type)
{
    if (s_lwm2m_task_running || s_lwm2m_task_handle != NULL) {
        ESP_LOGW(TAG, "LwM2M client task already running, start request ignored");
        return ESP_OK;
    }

    /* Store connection type for use in client task */
    s_connection_type = connection_type;

    bool factory_empty_server_host = false;
    factory_data_config_t factory_cfg = {0};
    esp_err_t factory_cfg_err = factory_data_load(&factory_cfg);
    if (factory_cfg_err == ESP_OK &&
        factory_cfg.present &&
        factory_cfg.lwm2m_host[0] == '\0') {
        factory_empty_server_host = true;
    }

    if (factory_cfg_err == ESP_OK &&
        factory_cfg.present &&
        factory_cfg.serial_number[0] != '\0') {
        strncpy(serialNumber, factory_cfg.serial_number, sizeof(serialNumber) - 1);
        serialNumber[sizeof(serialNumber) - 1] = '\0';
        ESP_LOGI(TAG, "Using factory serial number for endpoint: %s", serialNumber);
    }

    memset(private_key, 0, sizeof(private_key));
    private_key_len = 0;
    if (factory_cfg_err == ESP_OK && factory_cfg.present && factory_cfg.device_private_key_len > 0) {
        if (factory_cfg.device_private_key_len > sizeof(private_key)) {
            ESP_LOGE(TAG,
                     "Factory private key too large (%u > %u)",
                     (unsigned)factory_cfg.device_private_key_len,
                     (unsigned)sizeof(private_key));
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(private_key, factory_cfg.device_private_key, factory_cfg.device_private_key_len);
        private_key_len = factory_cfg.device_private_key_len;
        ESP_LOGI(TAG,
                 "Factory private key detected; enabling DTLS PSK mode (%u bytes)",
                 (unsigned)private_key_len);
    }

    if (serialNumber[0] == '\0') {
        uint8_t mac[6] = {0};
        if (esp_efuse_mac_get_default(mac) == ESP_OK) {
            snprintf(serialNumber, sizeof(serialNumber), "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            snprintf(serialNumber, sizeof(serialNumber), "ESP32DEVICE");
        }
        ESP_LOGW(TAG, "Factory serial unavailable, using MAC-based endpoint: %s", serialNumber);
    }

    memset(server, 0, sizeof(server));
    if (factory_empty_server_host) {
        esp_err_t gw_uri_err = lwm2m_build_uri_from_gateway_ip(server, sizeof(server));
        if (gw_uri_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "Factory LwM2M host is empty and gateway IP is unavailable; cannot build server URI (%s)",
                     esp_err_to_name(gw_uri_err));
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "Factory LwM2M host empty; using gateway-derived server URI: %s", server);
    } else {
        esp_err_t server_uri_err = lwm2m_load_server_uri_from_nvs(server, sizeof(server));
        if (server_uri_err != ESP_OK || !lwm2m_is_valid_server_uri(server)) {
            esp_err_t gw_uri_err = lwm2m_build_uri_from_gateway_ip(server, sizeof(server));
            if (gw_uri_err != ESP_OK) {
                ESP_LOGE(TAG,
                         "LwM2M server URI unavailable from NVS and gateway IP is unavailable; cannot build server URI (nvs=%s, gateway=%s)",
                         esp_err_to_name(server_uri_err),
                         esp_err_to_name(gw_uri_err));
                return ESP_ERR_INVALID_STATE;
            }

            ESP_LOGW(TAG,
                     "LwM2M server URI unavailable/invalid in NVS (err=%s); using gateway-derived server URI: %s",
                     esp_err_to_name(server_uri_err),
                     server);
        } else {
            ESP_LOGI(TAG, "Loaded LwM2M server URI from NVS: %s", server);
        }
    }

    if (private_key_len > 0) {
        const char *scheme_end = strstr(server, "://");
        if (!scheme_end) {
            ESP_LOGE(TAG, "Invalid server URI while enforcing CoAPS: %s", server);
            return ESP_ERR_INVALID_RESPONSE;
        }

        const char *authority_start = scheme_end + 3;
        const char *path_start = strchr(authority_start, '/');
        const char *authority_end = path_start ? path_start : (server + strlen(server));
        size_t authority_len = (size_t)(authority_end - authority_start);
        if (authority_len == 0) {
            ESP_LOGE(TAG, "Server URI authority empty while enforcing CoAPS: %s", server);
            return ESP_ERR_INVALID_RESPONSE;
        }

        char host_only[96] = {0};
        if (authority_start[0] == '[') {
            const char *closing = strchr(authority_start, ']');
            if (!closing || closing >= authority_end) {
                ESP_LOGE(TAG, "Invalid IPv6 server URI while enforcing CoAPS: %s", server);
                return ESP_ERR_INVALID_RESPONSE;
            }
            size_t host_len = (size_t)(closing - authority_start + 1); /* Keep brackets */
            if (host_len >= sizeof(host_only)) {
                ESP_LOGE(TAG, "Server host too long while enforcing CoAPS");
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(host_only, authority_start, host_len);
            host_only[host_len] = '\0';
        } else {
            const char *colon = memchr(authority_start, ':', authority_len);
            const char *host_end = colon ? colon : authority_end;
            size_t host_len = (size_t)(host_end - authority_start);
            if (host_len == 0 || host_len >= sizeof(host_only)) {
                ESP_LOGE(TAG, "Server host invalid while enforcing CoAPS");
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(host_only, authority_start, host_len);
            host_only[host_len] = '\0';
        }

        const char *path = path_start ? path_start : "";
        int written = snprintf(server, sizeof(server), "coaps://%s:5684%s", host_only, path);
        if (written <= 0 || (size_t)written >= sizeof(server)) {
            ESP_LOGE(TAG, "Failed to build secure LwM2M URI");
            return ESP_ERR_INVALID_SIZE;
        }

        ESP_LOGI(TAG,
                 "Factory private key present: enforcing secure LwM2M URI %s",
                 server);
    }

    /* Ensure bootstrap flag is false at init so we don't use saved DTLS creds */
    (void)lwm2m_mark_not_bootstrapped();

    if (s_lwm2m_mutex == NULL) {
        s_lwm2m_mutex = xSemaphoreCreateMutex();
        if (s_lwm2m_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create LwM2M mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    
    /* Log connection type */
    const char *conn_type_str = "Unknown";
    if (connection_type == LWM2M_CONN_TYPE_WIFI) conn_type_str = "WiFi";
    else if (connection_type == LWM2M_CONN_TYPE_HALOW) conn_type_str = "HaLow";
    else if (connection_type == LWM2M_CONN_TYPE_LORA) conn_type_str = "LoRa";
    
    ESP_LOGI(TAG, "Starting LwM2M client with connection type: %s", conn_type_str);
    
    /* Configure DTLS logging before client task starts */
    /* Debug level emits full hexdumps (very slow for images) and can starve WDT; keep at WARN. */
    dtls_set_log_level(DTLS_LOG_WARN);
    
    /* Check available heap before creating task */
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "Free heap: %lu bytes, Minimum free heap: %lu bytes", (unsigned long)free_heap, (unsigned long)min_free_heap);
    
    const uint32_t candidate_stack_words[] = {24576, 20480, 16384, 12288};
    for (size_t i = 0; i < (sizeof(candidate_stack_words) / sizeof(candidate_stack_words[0])); i++) {
        uint32_t stack_words = candidate_stack_words[i];
        ESP_LOGI(TAG, "Creating LwM2M client task with %lu-byte stack...", (unsigned long)stack_words);
        BaseType_t ret = xTaskCreate(client_task, "client_lwm2m", stack_words, NULL, 5, &s_lwm2m_task_handle);
        if (ret == pdPASS) {
            ESP_LOGI(TAG, "LwM2M client task created successfully");
            return ESP_OK;
        }

        s_lwm2m_task_handle = NULL;
        ESP_LOGW(TAG,
                 "Failed to create LwM2M client task with %lu-byte stack (free heap: %lu)",
                 (unsigned long)stack_words,
                 (unsigned long)esp_get_free_heap_size());
    }

    ESP_LOGE(TAG, "Failed to create LwM2M client task with all stack candidates");
    return ESP_ERR_NO_MEM;
}

esp_err_t lwm2m_client_stop(uint32_t timeout_ms)
{
    if (!s_lwm2m_task_running && s_lwm2m_task_handle == NULL) {
        ESP_LOGI(TAG, "LwM2M client already stopped");
        s_lwm2m_stop_without_deregister_requested = false;
        return ESP_OK;
    }

    s_lwm2m_stop_without_deregister_requested = false;
    s_lwm2m_stop_requested = true;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (s_lwm2m_task_running || s_lwm2m_task_handle != NULL) {
        if (timeout_ms > 0 && xTaskGetTickCount() >= deadline) {
            ESP_LOGW(TAG, "Timeout stopping LwM2M client task");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "LwM2M client stopped");
    return ESP_OK;
}

esp_err_t lwm2m_client_stop_silent(uint32_t timeout_ms)
{
    ESP_LOGW(TAG, "lwm2m_client_stop_silent() requested; performing normal deregister");
    return lwm2m_client_stop(timeout_ms);
}

// Gateway statistics helper functions
void lwm2m_update_gateway_rx_stats(uint64_t bytes) {
    // Gateway object disabled; keep log for debug.
    ESP_LOGD(TAG, "RX stats (stub): %llu bytes", (unsigned long long)bytes);
}

void lwm2m_update_gateway_tx_stats(uint64_t bytes) {
    // Gateway object disabled; keep log for debug.
    ESP_LOGD(TAG, "TX stats (stub): %llu bytes", (unsigned long long)bytes);
}

void lwm2m_set_gateway_status(const char* status) {
    // Gateway object disabled; keep log for debug.
    ESP_LOGD(TAG, "Gateway status (stub): %s", status ? status : "NULL");
}

void lwm2m_update_connected_devices_count(void) {
    // Gateway object disabled; no device tracking.
    ESP_LOGD(TAG, "Connected devices count update (stub)");
}

void lwm2m_update_active_sessions(int32_t session_count) {
    // Gateway object disabled; keep log for debug.
    ESP_LOGD(TAG, "Active sessions (stub): %ld", session_count);
}

void lwm2m_trigger_registration_update(void) {
    if (client_data.lwm2mH && client_data.lwm2mH->state == STATE_READY) {
        ESP_LOGI(TAG, "Triggering registration update due to object changes");
        s_lwm2m_io_in_progress = true;
        int res = lwm2m_update_registration(client_data.lwm2mH, 0, true);
        s_lwm2m_io_in_progress = false;
        mark_lwm2m_io_activity();
        if (res != COAP_NO_ERROR) {
            ESP_LOGW(TAG, "Registration update failed with error: %d", res);
        } else {
            ESP_LOGI(TAG, "Registration update triggered successfully");
        }
    } else {
        ESP_LOGW(TAG, "Cannot trigger registration update - client not ready (state: %d)", 
                 client_data.lwm2mH ? client_data.lwm2mH->state : -1);
    }
}

esp_err_t lwm2m_client_trigger_pull_ota_for_version(const char *server_version)
{
    char server_uri[128] = {0};
    char read_uri[256] = {0};
    char host_only[96] = {0};
    const char *scheme_end;
    const char *host_start;
    const char *path_start;
    const char *authority_end;
    size_t authority_len;
    size_t host_len;
    int written;
    lwm2m_data_t write_data;
    uint8_t write_result;
    uint8_t exec_result;
    const uint16_t fw_instance_id = 0;
    const uint16_t fw_res_package_uri = 1;
    const uint16_t fw_res_update = 2;

    if (client_data.lwm2mH == NULL) {
        ESP_LOGW(TAG, "Cannot trigger pull OTA: LwM2M context not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    lwm2m_object_t *firmware_obj = NULL;
    for (size_t i = 0; i < s_optional_count; ++i) {
        lwm2m_object_t *obj = s_optional_objects[i];
        if (obj && obj->objID == LWM2M_FIRMWARE_UPDATE_OBJECT_ID) {
            firmware_obj = obj;
            break;
        }
    }

    if (firmware_obj == NULL) {
        ESP_LOGE(TAG, "Cannot trigger pull OTA: Firmware object is not available");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (server[0] == '\0') {
        ESP_LOGE(TAG, "Cannot trigger pull OTA: init-selected server URI is empty");
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(server_uri, server, sizeof(server_uri) - 1);
    server_uri[sizeof(server_uri) - 1] = '\0';

    scheme_end = strstr(server_uri, "://");
    if (!scheme_end) {
        ESP_LOGE(TAG, "Invalid server URI for pull OTA: %s", server_uri);
        return ESP_ERR_INVALID_RESPONSE;
    }

    host_start = scheme_end + 3;
    if (host_start[0] == '\0') {
        ESP_LOGE(TAG, "Invalid server URI host for pull OTA: %s", server_uri);
        return ESP_ERR_INVALID_RESPONSE;
    }

    path_start = strchr(host_start, '/');
    authority_end = path_start ? path_start : (server_uri + strlen(server_uri));
    authority_len = (size_t)(authority_end - host_start);
    if (authority_len == 0 || authority_len >= sizeof(host_only)) {
        ESP_LOGE(TAG, "Server URI authority invalid for pull OTA: %s", server_uri);
        return ESP_ERR_INVALID_SIZE;
    }

    if (host_start[0] == '[') {
        const char *closing = strchr(host_start, ']');
        if (closing == NULL || closing > authority_end) {
            ESP_LOGE(TAG, "Invalid IPv6 host in server URI for pull OTA: %s", server_uri);
            return ESP_ERR_INVALID_RESPONSE;
        }
        host_len = (size_t)(closing - (host_start + 1));
        if (host_len == 0 || host_len >= sizeof(host_only)) {
            ESP_LOGE(TAG, "Invalid IPv6 host length in server URI for pull OTA: %s", server_uri);
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(host_only, host_start + 1, host_len);
        host_only[host_len] = '\0';
    } else {
        const char *colon = memchr(host_start, ':', authority_len);
        const char *host_end = colon ? colon : authority_end;
        host_len = (size_t)(host_end - host_start);
        if (host_len == 0 || host_len >= sizeof(host_only)) {
            ESP_LOGE(TAG, "Invalid host length in server URI for pull OTA: %s", server_uri);
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(host_only, host_start, host_len);
        host_only[host_len] = '\0';
    }

    if (server_version && server_version[0] != '\0') {
        written = snprintf(read_uri,
                           sizeof(read_uri),
                           "http://%s/firmware/%s.bin",
                           host_only,
                           server_version);
    } else {
        written = snprintf(read_uri, sizeof(read_uri), "http://%s/firmware", host_only);
    }
    if (written <= 0 || (size_t)written >= sizeof(read_uri)) {
        ESP_LOGE(TAG, "Failed to build pull OTA read URI from server URI: %s", server_uri);
        return ESP_ERR_INVALID_SIZE;
    }

    memset(&write_data, 0, sizeof(write_data));
    write_data.id = fw_res_package_uri;
    write_data.type = LWM2M_TYPE_STRING;
    write_data.value.asBuffer.buffer = (uint8_t *)read_uri;
    write_data.value.asBuffer.length = strlen(read_uri);

    if (firmware_obj->writeFunc == NULL || firmware_obj->executeFunc == NULL) {
        ESP_LOGE(TAG, "Firmware object missing write/execute callbacks");
        return ESP_ERR_NOT_SUPPORTED;
    }

    write_result = firmware_obj->writeFunc(client_data.lwm2mH,
                                           fw_instance_id,
                                           1,
                                           &write_data,
                                           firmware_obj,
                                           LWM2M_WRITE_REPLACE_RESOURCES);
    if (write_result != COAP_204_CHANGED) {
        ESP_LOGE(TAG, "Pull OTA package URI write rejected: result=%u uri=%s", write_result, read_uri);
        return ESP_FAIL;
    }

    exec_result = firmware_obj->executeFunc(client_data.lwm2mH,
                                            fw_instance_id,
                                            fw_res_update,
                                            NULL,
                                            0,
                                            firmware_obj);
    if (exec_result == COAP_204_CHANGED) {
        ESP_LOGI(TAG,
                 "Client-initiated pull OTA read started from LwM2M server: %s (resume by OTA position, source=%s)",
                 read_uri,
                 "lwm2m_init");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Client-initiated pull OTA execute rejected: result=%u uri=%s", exec_result, read_uri);
    return ESP_FAIL;
}

esp_err_t lwm2m_client_trigger_pull_ota(void)
{
    return lwm2m_client_trigger_pull_ota_for_version(NULL);
}

void lwm2m_request_socket_reinit(void)
{
    s_lwm2m_socket_reinit_requested = true;
    ESP_LOGI(TAG, "Requested LwM2M UDP socket reinitialization");
}

void lwm2m_update_device_rssi(uint16_t instance_id, int rssi) {
    (void)instance_id;
    (void)rssi;
    ESP_LOGD(TAG, "Connectivity monitoring removed; RSSI update skipped");
}

void lwm2m_update_device_link_quality(uint16_t instance_id, int link_quality) {
    (void)instance_id;
    (void)link_quality;
    ESP_LOGD(TAG, "Connectivity monitoring removed; link quality update skipped");
}

// Callback function for gateway object to update device instance_id
static UNUSED_ATTR void gateway_device_update_callback(uint32_t device_id, uint16_t new_instance_id)
{
    (void)device_id;
    (void)new_instance_id;
    ESP_LOGD(TAG, "Gateway object removed; update callback ignored");
}


/**
 * @brief Convert hex string to byte array
 * @param hex_str Input hex string (e.g., "48656c6c6f")
 * @param hex_len Length of hex string
 * @param out_bytes Output byte array
 * @param out_max_len Maximum size of output buffer
 * @return Number of bytes written, or -1 on error
 */
static UNUSED_ATTR int hex_string_to_bytes(const char *hex_str, size_t hex_len, uint8_t *out_bytes, size_t out_max_len)
{
    if (!hex_str || !out_bytes || hex_len == 0) {
        return -1;
    }
    
    // Hex string must have even length (2 chars per byte)
    if (hex_len % 2 != 0) {
        ESP_LOGW(TAG, "Hex string has odd length (%zu), not a valid hex encoding", hex_len);
        return -1;
    }
    
    size_t byte_count = hex_len / 2;
    if (byte_count > out_max_len) {
        ESP_LOGE(TAG, "Output buffer too small: need %zu bytes, have %zu", byte_count, out_max_len);
        return -1;
    }
    
    for (size_t i = 0; i < byte_count; i++) {
        char hi = hex_str[i * 2];
        char lo = hex_str[i * 2 + 1];
        
        // Convert hex char to value (0-15)
        int hi_val = -1, lo_val = -1;
        if (hi >= '0' && hi <= '9') hi_val = hi - '0';
        else if (hi >= 'a' && hi <= 'f') hi_val = hi - 'a' + 10;
        else if (hi >= 'A' && hi <= 'F') hi_val = hi - 'A' + 10;
        
        if (lo >= '0' && lo <= '9') lo_val = lo - '0';
        else if (lo >= 'a' && lo <= 'f') lo_val = lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F') lo_val = lo - 'A' + 10;
        
        if (hi_val < 0 || lo_val < 0) {
            ESP_LOGE(TAG, "Invalid hex character at position %zu: '%c%c'", i * 2, hi, lo);
            return -1;
        }
        
        out_bytes[i] = (hi_val << 4) | lo_val;
    }
    
    return byte_count;
}

// Callback function for gateway object to delete device
static UNUSED_ATTR void gateway_device_delete_callback(uint32_t device_id, uint16_t instance_id)
{
    (void)device_id;
    (void)instance_id;
    ESP_LOGD(TAG, "Gateway object removed; delete callback ignored");
}

// Callback function for gateway object PSK write from server
static UNUSED_ATTR void gateway_psk_write_callback(uint32_t device_id, uint16_t instance_id, const char *identity, const uint8_t *psk, size_t psk_length, const char *server)
{
    (void)device_id;
    (void)instance_id;
    (void)identity;
    (void)psk;
    (void)psk_length;
    (void)server;
    ESP_LOGD(TAG, "Gateway object removed; PSK write ignored");
}

/* Check if LwM2M client is initialized and ready to handle operations */
bool lwm2m_is_ready(void)
{
    return (client_data.lwm2mH != NULL && client_data.lwm2mH->state == STATE_READY);
}

bool lwm2m_is_idle_for_sleep(void)
{
    if (s_lwm2m_io_in_progress) return false;

    if (s_lwm2m_mutex) {
        if (xSemaphoreTake(s_lwm2m_mutex, 0) != pdTRUE) {
            return false;
        }
        xSemaphoreGive(s_lwm2m_mutex);
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t last = s_last_lwm2m_io_tick;
    TickType_t idle_ticks = now - last;
    TickType_t grace_ticks = pdMS_TO_TICKS(CONFIG_LWM2M_SLEEP_IDLE_GRACE_MS);
    return idle_ticks >= grace_ticks;
}

bool lwm2m_has_inflight_activity(void)
{
    if (s_lwm2m_io_in_progress || s_sample_send_waiting) {
        return true;
    }

    if (s_lwm2m_mutex) {
        if (xSemaphoreTake(s_lwm2m_mutex, 0) != pdTRUE) {
            return true;
        }
        xSemaphoreGive(s_lwm2m_mutex);
    }

    return false;
}

bool lwm2m_is_ota_in_progress(void)
{
    return firmware_object_is_update_in_progress();
}

bool lwm2m_cancel_ota_in_progress(void)
{
    return firmware_object_request_cancel_update();
}

struct _lwm2m_context_* get_lwm2m_context(void)
{
    return client_data.lwm2mH;
}

void lwm2m_set_ready_callback(void (*callback)(void))
{
    s_lwm2m_ready_callback = callback;
    ESP_LOGI(TAG, "LwM2M ready callback %s", callback ? "registered" : "unregistered");
}

/* Send LwM2M message to server proactively using lwm2m_send() */
void lwm2m_send_device_notification(uint16_t gateway_instance_id)
{
#ifndef LWM2M_VERSION_1_0
    (void)gateway_instance_id;
    ESP_LOGD(TAG, "Gateway object removed; notification skipped");
#else
    ESP_LOGW(TAG, "lwm2m_send() requires LWM2M 1.1+, using registration update instead");
    lwm2m_trigger_registration_update();
#endif
}

/* Update test object opaque bytes (camera image) and trigger value changed notification */
esp_err_t lwm2m_update_test_opaque(const uint8_t* data, size_t len)
{
    (void)data;
    (void)len;
    ESP_LOGW(TAG, "Test object removed; opaque update not supported");
    return ESP_ERR_NOT_SUPPORTED;
}

/* Send binary image data to /image CoAP endpoint */
esp_err_t lwm2m_send_image_to_endpoint(const uint8_t* data, size_t len)
{
    if (!data || len == 0) {
        ESP_LOGE(TAG, "Invalid image data parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if LwM2M context is initialized
    if (!client_data.lwm2mH) {
        ESP_LOGE(TAG, "LwM2M context not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_lwm2m_mutex) {
        ESP_LOGE(TAG, "LwM2M mutex not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lwm2m_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout acquiring LwM2M mutex for image send");
        return ESP_ERR_TIMEOUT;
    }
    
    // Send binary data to /image endpoint using custom CoAP POST
    // Content type 42 = application/octet-stream
    int result = lwm2m_send_coap_post(client_data.lwm2mH, "image", data, len, 42);
    mark_lwm2m_io_activity();

    xSemaphoreGive(s_lwm2m_mutex);
    
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to send image to /image endpoint (error: %d)", result);
        
        // Map error codes to ESP error codes
        switch (result) {
            case -1: return ESP_ERR_INVALID_ARG;
            case -2: return ESP_ERR_INVALID_STATE;
            case -3: return ESP_ERR_NOT_FOUND;
            case -4: return ESP_ERR_NO_MEM;
            default: return ESP_FAIL;
        }
    }
    
    ESP_LOGI(TAG, "📤 Sent %zu bytes to /image CoAP endpoint", len);
    return ESP_OK;
}

esp_err_t lwm2m_send_sample_data_with_ack(const uint8_t * const *sample_lines,
                                          const size_t *sample_line_lens,
                                          size_t sample_line_count,
                                          uint32_t ack_timeout_ms,
                                          bool *acked)
{
    if (sample_line_count > 0 && (!sample_lines || !sample_line_lens)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sample_line_count > SAMPLE_DATA_FIELD_MAX_LINES) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < sample_line_count; i++) {
        if (!sample_lines[i] || sample_line_lens[i] > SAMPLE_DATA_FIELD_LINE_MAX_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (acked) {
        *acked = false;
    }

    if (!client_data.lwm2mH || client_data.lwm2mH->state != STATE_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_lwm2m_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_sample_send_ack_sem) {
        s_sample_send_ack_sem = xSemaphoreCreateBinary();
        if (!s_sample_send_ack_sem) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_lwm2m_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout acquiring LwM2M mutex for sample send");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t set_err = lwm2m_sample_set_data_field_multi(sample_lines, sample_line_lens, sample_line_count);
    if (set_err != ESP_OK) {
        xSemaphoreGive(s_lwm2m_mutex);
        return set_err;
    }

    lwm2m_uri_t uris[4] = {
        {
            .objectId = LWM2M_OBJ_SAMPLE,
            .instanceId = 0,
            .resourceId = RES_SAMPLE_CONFIG_VERSION,
#ifndef LWM2M_VERSION_1_0
            .resourceInstanceId = LWM2M_MAX_ID,
#endif
        },
        {
            .objectId = LWM2M_OBJ_SAMPLE,
            .instanceId = 0,
            .resourceId = RES_SAMPLE_SERVER_ALIGNED_UPTIME_MS,
#ifndef LWM2M_VERSION_1_0
            .resourceInstanceId = LWM2M_MAX_ID,
#endif
        },
        {
            .objectId = LWM2M_OBJ_SAMPLE,
            .instanceId = 0,
            .resourceId = RES_SAMPLE_UPTIME_DIFFERENCE_MS,
#ifndef LWM2M_VERSION_1_0
            .resourceInstanceId = LWM2M_MAX_ID,
#endif
        },
        {
            .objectId = LWM2M_OBJ_SAMPLE,
            .instanceId = 0,
            .resourceId = RES_SAMPLE_DATA_FIELD,
#ifndef LWM2M_VERSION_1_0
            .resourceInstanceId = LWM2M_MAX_ID,
#endif
        },
    };

    uint32_t seq = s_sample_send_wait_seq + 1;
    if (seq == 0) {
        seq = 1;
    }
    s_sample_send_wait_seq = seq;
    s_sample_send_waiting = true;
    s_sample_send_acked = false;

    while (xSemaphoreTake(s_sample_send_ack_sem, 0) == pdTRUE) {
    }

    s_lwm2m_io_in_progress = true;
    int send_rc = lwm2m_send(client_data.lwm2mH,
                             0,
                             uris,
                             sizeof(uris) / sizeof(uris[0]),
                             lwm2m_sample_send_callback,
                             (void *)(uintptr_t)seq);
    s_lwm2m_io_in_progress = false;
    mark_lwm2m_io_activity();
    xSemaphoreGive(s_lwm2m_mutex);

    if (send_rc != COAP_NO_ERROR) {
        s_sample_send_waiting = false;
        ESP_LOGW(TAG, "lwm2m_send(sample data) failed: %d", send_rc);
        return ESP_FAIL;
    }

    TickType_t wait_ticks = pdMS_TO_TICKS(ack_timeout_ms > 0 ? ack_timeout_ms : 5000);
    if (xSemaphoreTake(s_sample_send_ack_sem, wait_ticks) != pdTRUE) {
        s_sample_send_waiting = false;
        ESP_LOGW(TAG, "Timed out waiting for sample send ACK after %lu ms", (unsigned long)(ack_timeout_ms > 0 ? ack_timeout_ms : 5000));
        return ESP_ERR_TIMEOUT;
    }

    s_sample_send_waiting = false;
    if (acked) {
        *acked = s_sample_send_acked;
    }

    if (!s_sample_send_acked) {
        ESP_LOGW(TAG, "Sample send callback returned without ACK confirmation");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sample data send ACK confirmed");
    return ESP_OK;
}

