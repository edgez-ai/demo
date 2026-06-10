/**
 * @file prov.h
 * @brief Platform-agnostic Wi-Fi/Network provisioning interface
 * 
 * This header provides a common interface for network provisioning
 * across different platforms.
 */

#ifndef DRIVER_PROV_H
#define DRIVER_PROV_H

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <stddef.h>
#include <stdbool.h>
#include "lwm2m_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Event bit to signal Wi-Fi connection */
#define WIFI_CONNECTED_EVENT BIT0
/* Event bit to signal network stack is ready for sockets (valid IP acquired) */
#define WIFI_NETWORK_READY_EVENT BIT1

/**
 * @brief Initialize and start Wi-Fi provisioning
 * 
 * This function initializes the Wi-Fi provisioning manager and either:
 * - Starts the provisioning process if the device is not yet provisioned
 * - Connects to the stored Wi-Fi credentials if already provisioned
 * 
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t wifi_prov_init_and_start(void);

/**
 * @brief Wait for Wi-Fi connection
 * 
 * Blocks until the Wi-Fi connection is established
 * 
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t wifi_prov_wait_connected(void);

/**
 * @brief Wait for Wi-Fi connection with timeout
 * 
 * Blocks until the Wi-Fi connection is established or timeout occurs
 * 
 * @param timeout_ms Timeout in milliseconds (0 = no wait, portMAX_DELAY = wait forever)
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT on timeout, error code otherwise
 */
esp_err_t wifi_prov_wait_connected_timeout(uint32_t timeout_ms);

/**
 * @brief Wait until network is ready for sockets (valid IP acquired)
 *
 * For Wi-Fi, this is equivalent to IP_EVENT_STA_GOT_IP.
 * For HaLow, this waits until the real HaLow IP is available.
 *
 * @param timeout_ms Timeout in milliseconds
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT on timeout, error code otherwise
 */
esp_err_t wifi_prov_wait_network_ready_timeout(uint32_t timeout_ms);

/**
 * @brief Get active IPv4 gateway address for current transport
 *
 * In HaLow mode this prefers MMIPAL/cached HaLow IP state, which can be ready
 * before esp_netif reflects the gateway. In Wi-Fi mode this falls back to
 * esp_netif gateway information.
 *
 * @param out_gateway Output buffer for IPv4 gateway string (e.g. "192.168.12.1")
 * @param out_gateway_len Output buffer length
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_FOUND when unavailable, error otherwise
 */
esp_err_t wifi_prov_get_active_gateway_ipv4(char *out_gateway, size_t out_gateway_len);

/**
 * @brief Set startup credentials and preferred connection type from factory config
 *
 * When set before wifi_prov_init_and_start(), provisioning is skipped and the
 * startup flow attempts to connect directly using these credentials.
 *
 * @param ssid Non-empty SSID
 * @param password Non-empty passphrase
 * @param connection_type Preferred transport (Wi-Fi or HaLow)
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t wifi_prov_set_startup_credentials(const char *ssid,
											const char *password,
											lwm2m_connection_type_t connection_type);

/**
 * @brief Prepare provisioning/network stacks for deep sleep
 *
 * Ensures active transport stacks are shut down before deep sleep:
 * - HaLow transceiver (when active)
 * - Wi-Fi STA stack
 * - BLE provisioning service/session
 *
 * Safe to call repeatedly; treats already-stopped states as success.
 *
 * @return esp_err_t ESP_OK on success, or first non-fatal teardown error
 */
esp_err_t wifi_prov_prepare_for_deep_sleep(void);

/**
 * @brief Get the Wi-Fi event group handle
 * 
 * @return EventGroupHandle_t Event group handle for Wi-Fi events
 */
EventGroupHandle_t wifi_prov_get_event_group(void);

/**
 * @brief Check if Wi-Fi is already provisioned
 * 
 * @return true if Wi-Fi credentials are stored in NVS, false otherwise
 */
bool wifi_is_provisioned(void);

/**
 * @brief Check if a BLE provisioning session is currently connected
 *
 * @return true if the provisioning BLE transport is connected, false otherwise
 */
bool wifi_prov_ble_session_active(void);

/**
 * @brief Check if provisioning service is currently active
 *
 * @return true when provisioning manager/session is active, false otherwise
 */
bool wifi_prov_service_active(void);

/**
 * @brief Check if provisioning success happened recently
 *
 * @param window_ms Time window in milliseconds
 * @return true if provisioning succeeded within the given window, false otherwise
 */
bool wifi_prov_recent_success(uint32_t window_ms);

/**
 * @brief Check whether startup has usable credentials without provisioning flow
 *
 * Returns true when any of these are available:
 * - Factory startup override credentials armed in RAM
 * - Persisted Wi-Fi credentials
 * - Persisted HaLow credentials
 *
 * This is intended for early-boot sampling/sleep gates to avoid waiting for
 * interactive provisioning when factory credentials are already present.
 */
bool wifi_prov_has_bootstrap_credentials(void);

/**
 * @brief Start (or restart) BLE provisioning even if device already has credentials
 *
 * Clears existing credentials (if necessary) and advertises BLE provisioning so the
 * user can supply new Wi-Fi information without rebooting.
 */
esp_err_t wifi_prov_start_ble_reprovisioning(void);

/**
 * @brief Get the current connection type (WiFi or HaLow)
 * 
 * @return lwm2m_connection_type_t Connection type used for current connection
 */
lwm2m_connection_type_t wifi_prov_get_connection_type(void);

#ifdef CONFIG_ENABLE_MM_HALOW
/**
 * @brief Global flag to track if lwIP was already initialized by esp_netif_init()
 * 
 * This flag prevents double initialization when halow_init() is called
 * after WiFi stack has already initialized lwIP.
 */
extern bool g_lwip_initialized_by_esp_netif;

/**
 * @brief Shut down the HaLow transceiver before entering sleep
 *
 * Cleanly disconnects from the AP, shuts down the WLAN driver, de-initialises
 * the HAL (pulling RESET_N low), and holds the GPIO state so the module stays
 * powered off during deep sleep.  Safe to call when HaLow was never started.
 */
void wifi_prov_shutdown_halow_for_sleep(void);

/**
 * @brief Shut down the HaLow transceiver before warm reboot
 *
 * Same clean disconnect flow as sleep shutdown, but does NOT leave RESET_N
 * pin hold enabled across restart. Use this before esp_restart().
 */
void wifi_prov_shutdown_halow_for_restart(void);

/**
 * @brief Enter HaLow low-power mode for host light sleep windows
 *
 * Performs a full HaLow shutdown before host light sleep to reduce power draw.
 * The radio is restarted on wake via wifi_prov_exit_halow_light_sleep().
 * Safe to call repeatedly; no-op when HaLow is not the active connection.
 */
void wifi_prov_enter_halow_light_sleep(void);

/**
 * @brief Exit HaLow low-power mode after host wakes from light sleep
 *
 * Re-initializes and starts HaLow using stored credentials after a prior
 * full light-sleep shutdown.
 * Safe to call when light sleep mode was not entered.
 */
esp_err_t wifi_prov_exit_halow_light_sleep(void);

/**
 * @brief Check if HaLow state is ready for entering host light sleep
 *
 * Returns false while HaLow STA reconnect/association is still in progress,
 * so callers can avoid immediately shutting it down again.
 *
 * @return true if safe to enter light sleep now, false if reconnect is active
 */
bool wifi_prov_halow_ready_for_light_sleep(void);

/**
 * @brief Check if HaLow link/network is recovered enough for reporting
 *
 * For HaLow mode this requires STA to be connected and network-ready
 * (valid IP event observed). For non-HaLow modes this returns true.
 *
 * @return true if reporting can proceed, false otherwise
 */
bool wifi_prov_halow_ready_for_report(void);

/**
 * @brief Clear stored HaLow credentials from NVS
 * 
 * This removes the HaLow SSID and password from the dedicated HaLow NVS namespace.
 * After calling this, the device will start WiFi provisioning on next boot.
 * 
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t wifi_prov_clear_halow_credentials(void);

/**
 * @brief Check if HaLow credentials are stored in NVS
 * 
 * @return true if HaLow credentials exist, false otherwise
 */
bool wifi_prov_has_halow_credentials(void);

/**
 * @brief Override HaLow regulatory country code at runtime
 *
 * Must be called before HaLow initialization for deterministic behavior.
 * Expected format is a 2-character ISO code, e.g. "US", "CA", "JP", "AU", "NZ", "EU".
 */
esp_err_t wifi_prov_set_halow_country_code(const char *country_code);

/**
 * @brief Configure startup HaLow mesh profile from factory data
 *
 * Applies only to HaLow startup path when factory credentials are used.
 * When mesh_mode is false, channel/bandwidth hints are ignored.
 */
esp_err_t wifi_prov_set_startup_halow_mesh_profile(bool mesh_mode,
												   uint16_t channel,
												   uint8_t bandwidth_mhz);

/**
 * @brief Example function to start HaLow iperf test after connection
 * 
 * This is a demonstration of how to use the halow_iperf API.
 * Call this after HaLow link is up to start throughput testing.
 * 
 * @param is_server true to start server mode, false for client mode
 * @param use_udp true to use UDP, false for TCP
 * @param server_ip Server IP address (for client mode only)
 */
void halow_start_iperf_example(bool is_server, bool use_udp, const char *server_ip);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_PROV_H */
