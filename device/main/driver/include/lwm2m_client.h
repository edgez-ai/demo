/**
 * @file lwm2m_client.h
 * @brief Platform-agnostic LwM2M client interface
 * 
 * This header provides a common interface for LwM2M client functionality
 * across different platforms.
 */

#ifndef DRIVER_LWM2M_CLIENT_H
#define DRIVER_LWM2M_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Connection type for LwM2M client */
typedef enum {
    LWM2M_CONN_TYPE_WIFI = 0,
    LWM2M_CONN_TYPE_LORA = 1,
    LWM2M_CONN_TYPE_HALOW = 2,
} lwm2m_connection_type_t;

/* Start the LwM2M client task (creates FreeRTOS task internally). */
esp_err_t lwm2m_client_start(lwm2m_connection_type_t connection_type);

/* Stop the LwM2M client task and deinitialize runtime state. */
esp_err_t lwm2m_client_stop(uint32_t timeout_ms);

/* Compatibility stop API: currently performs normal de-registration shutdown. */
esp_err_t lwm2m_client_stop_silent(uint32_t timeout_ms);

/* Update the temperature value exposed via test object (before start or anytime). */
void lwm2m_client_set_temperature(float temp_celsius);

/* Gateway management functions */
void lwm2m_update_gateway_rx_stats(uint64_t bytes);
void lwm2m_update_gateway_tx_stats(uint64_t bytes);
void lwm2m_set_gateway_status(const char* status);
void lwm2m_update_connected_devices_count(void);
void lwm2m_update_active_sessions(int32_t session_count);

/* Trigger registration update to notify server of changes */
void lwm2m_trigger_registration_update(void);

/* Request UDP socket recreation after network interface reset/reinit. */
void lwm2m_request_socket_reinit(void);

/* Persist a validated server URI override for subsequent client starts. */
esp_err_t lwm2m_client_set_server_uri_override(const char *server_uri);

/* Start client-initiated pull OTA read loop from LwM2M server using saved OTA position. */
esp_err_t lwm2m_client_trigger_pull_ota(void);

/* Start client-initiated pull OTA with explicit server firmware version in package URI.
 * URI format: http://{lwm2m_server_ip}/firmware/{server_version}.bin */
esp_err_t lwm2m_client_trigger_pull_ota_for_version(const char *server_version);

/* Connectivity monitoring functions for updating device signal strength */
void lwm2m_update_device_rssi(uint16_t instance_id, int rssi);
void lwm2m_update_device_link_quality(uint16_t instance_id, int link_quality);

/* Forward declarations for LwM2M types - actual definitions in components/wakaama/include/liblwm2m.h */
struct _lwm2m_context_;
struct _lwm2m_object_t;

/* Register an optional LwM2M object prior to starting the client (appended to mandatory objects). */
void lwm2m_client_add_object(struct _lwm2m_object_t *obj);

/* Get LwM2M context pointer - returns struct _lwm2m_context_ * */
struct _lwm2m_context_* get_lwm2m_context(void);

/* Send LwM2M message to server proactively (uses lwm2m_send for LWM2M 1.1+) */
void lwm2m_send_device_notification(uint16_t gateway_instance_id);

/* Send camera image data directly to server using lwm2m_send */
void lwm2m_send_camera_image(void);

/* Check if LwM2M client is initialized and ready to handle operations */
bool lwm2m_is_ready(void);

/* Check if LwM2M has no active TX/RX and has been quiet long enough for sleep entry. */
bool lwm2m_is_idle_for_sleep(void);

/* Check if LwM2M currently has any active in-flight work that should block immediate sleep. */
bool lwm2m_has_inflight_activity(void);

/* Check if firmware OTA task is currently running. */
bool lwm2m_is_ota_in_progress(void);

/* Request cancellation of current firmware OTA task. */
bool lwm2m_cancel_ota_in_progress(void);

/* Advance RTC-cached server_sec_of_year by local elapsed uptime and optional future time. */
void lwm2m_client_advance_server_sec_of_year(uint64_t extra_us);

/* Get current RTC-cached server_sec_of_year; returns false when unavailable. */
bool lwm2m_client_get_server_sec_of_year(uint32_t *sec_of_year_out);

/**
 * @brief Set a callback function to be called when LwM2M reaches STATE_READY
 * 
 * @param callback Function to call when ready (can be NULL to unregister)
 */
void lwm2m_set_ready_callback(void (*callback)(void));

/* Get pointer to test object for camera module integration 
 * Note: test_data_t is defined in components/wakaama/examples/client/lwm2mclient.h
 * Include that header when you need to access test_data_t fields */
struct _lwm2m_object_t* get_test_object_ptr(void);

/* Initialize MQTT client from LwM2M MQTT broker object if available */
esp_err_t lwm2m_init_mqtt_from_object(void);

/* Update test object opaque bytes (camera image) and trigger value changed notification
 * @param data Pointer to image data buffer
 * @param len Length of image data
 * @return ESP_OK on success, ESP_FAIL on error */
esp_err_t lwm2m_update_test_opaque(const uint8_t* data, size_t len);

/* Send binary image data to /image CoAP endpoint
 * @param data Pointer to image data buffer
 * @param len Length of image data
 * @return ESP_OK on success, error code on failure */
esp_err_t lwm2m_send_image_to_endpoint(const uint8_t* data, size_t len);

/* Send sample object data field (multiple opaque lines) using one LwM2M Send and wait for ACK completion.
 * @param sample_lines Array of opaque line buffers
 * @param sample_line_lens Array of line lengths
 * @param sample_line_count Number of lines in this batch
 * @param ack_timeout_ms Maximum time to wait for ACK callback
 * @param acked Optional output set true only when ACK is confirmed
 * @return ESP_OK on ACK success, error code otherwise */
esp_err_t lwm2m_send_sample_data_with_ack(const uint8_t * const *sample_lines,
                                          const size_t *sample_line_lens,
                                          size_t sample_line_count,
                                          uint32_t ack_timeout_ms,
                                          bool *acked);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_LWM2M_CLIENT_H */
