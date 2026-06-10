// Shared interface object IDs and resource definitions for LwM2M I2C/RS485 objects
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "esp_err.h"
#include "lwm2mclient.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private object IDs */
#define LWM2M_OBJ_I2C   10251
#define LWM2M_OBJ_RS485 10252
#define LWM2M_OBJ_UART  10253
#define LWM2M_OBJ_BLE_INTERFACE 10254
#define LWM2M_OBJ_BLE_DEVICE    10255

/* I2C resource IDs */
#define RES_I2C_TYPE             0
#define RES_I2C_ENABLED          1
#define RES_I2C_OPEN_STATE       2
#define RES_I2C_TX_BYTES         3
#define RES_I2C_RX_BYTES         4
#define RES_I2C_ERROR_COUNT      5
#define RES_I2C_LAST_ERROR       6
#define RES_I2C_ADDRESS          7
#define RES_I2C_MODE             8
#define RES_I2C_RESET_COUNTERS   9
#define RES_I2C_STATS_WINDOW_MS  10
#define RES_I2C_TX_RATE          11
#define RES_I2C_RX_RATE          12
#define RES_I2C_TX_PAYLOAD       13
#define RES_I2C_RX_BUFFER_POS    14
#define RES_I2C_RX_CHUNK         15
#define RES_I2C_RX_BUFFER_SIZE   16
#define RES_I2C_TX_PIN           17
#define RES_I2C_RX_PIN           18
#define RES_I2C_DRIVER           19
#define RES_I2C_OBJECT_IDS       20

/* RS485 (Modbus/UART) resource IDs */
#define RES_RS485_TYPE             0
#define RES_RS485_ENABLED          1
#define RES_RS485_OPEN_STATE       2
#define RES_RS485_TX_BYTES         3
#define RES_RS485_RX_BYTES         4
#define RES_RS485_ERROR_COUNT      5
#define RES_RS485_LAST_ERROR       6
#define RES_RS485_BAUDRATE         7
#define RES_RS485_MODBUS_UNIT_ID   8
#define RES_RS485_MODE             9
#define RES_RS485_RESET_COUNTERS   10
#define RES_RS485_STATS_WINDOW_MS  11
#define RES_RS485_TX_RATE          12
#define RES_RS485_RX_RATE          13
#define RES_RS485_TX_PAYLOAD       14
#define RES_RS485_RX_BUFFER_POS    15
#define RES_RS485_RX_CHUNK         16
#define RES_RS485_RX_BUFFER_SIZE   17
#define RES_RS485_TX_PIN           18
#define RES_RS485_RX_PIN           19
#define RES_RS485_DRIVER           20
#define RES_RS485_OBJECT_IDS       21

/* UART resource IDs */
#define RES_UART_TYPE             0
#define RES_UART_ENABLED          1
#define RES_UART_OPEN_STATE       2
#define RES_UART_TX_BYTES         3
#define RES_UART_RX_BYTES         4
#define RES_UART_ERROR_COUNT      5
#define RES_UART_LAST_ERROR       6
#define RES_UART_BAUDRATE         7
#define RES_UART_MODE             8
#define RES_UART_RESET_COUNTERS   9
#define RES_UART_STATS_WINDOW_MS  10
#define RES_UART_TX_RATE          11
#define RES_UART_RX_RATE          12
#define RES_UART_TX_PAYLOAD       13
#define RES_UART_RX_BUFFER_POS    14
#define RES_UART_RX_CHUNK         15
#define RES_UART_RX_BUFFER_SIZE   16
#define RES_UART_TX_PIN           17
#define RES_UART_RX_PIN           18
#define RES_UART_DRIVER           19
#define RES_UART_OBJECT_IDS       20

typedef enum {
	IFACE_TYPE_UART = 0,
	IFACE_TYPE_I2C = 1,
	IFACE_TYPE_UVC = 2,
	IFACE_TYPE_MODBUS_UART = 3,
	IFACE_TYPE_BLE = 4,
} interface_type_t;

typedef ssize_t (*interface_tx_handler_t)(uint16_t instance_id, const uint8_t *data, size_t len);
typedef ssize_t (*interface_rx_handler_t)(uint16_t instance_id, int32_t pos, uint8_t *out, size_t maxlen, int32_t *advanced);
typedef esp_err_t (*interface_open_handler_t)(uint16_t instance_id, bool open);

typedef struct {
	bool enabled;
	bool open_state;
	uint32_t i2c_address;
	uint32_t mode;
	uint32_t stats_window_ms;
	uint32_t tx_rate;
	uint32_t rx_rate;
	int32_t rx_buffer_size;
	int32_t tx_pin;
	int32_t rx_pin;
} i2c_instance_cfg_t;

typedef struct {
	bool enabled;
	bool open_state;
	uint32_t baudrate;
	uint32_t modbus_unit_id;
	uint32_t mode;
	uint32_t stats_window_ms;
	uint32_t tx_rate;
	uint32_t rx_rate;
	int32_t rx_buffer_size;
	int32_t tx_pin;
	int32_t rx_pin;
} rs485_instance_cfg_t;

typedef struct {
	bool enabled;
	bool open_state;
	uint32_t baudrate;
	uint32_t mode;
	uint32_t stats_window_ms;
	uint32_t tx_rate;
	uint32_t rx_rate;
	int32_t rx_buffer_size;
	int32_t tx_pin;
	int32_t rx_pin;
} uart_instance_cfg_t;

lwm2m_object_t *get_i2c_object(void);
esp_err_t i2c_object_set_instance(uint16_t instance_id, const i2c_instance_cfg_t *cfg);
esp_err_t i2c_object_update_counters(uint16_t instance_id, uint32_t tx_bytes, uint32_t rx_bytes,
									 uint32_t error_count, uint32_t tx_rate, uint32_t rx_rate,
									 const char *last_error);
esp_err_t i2c_object_set_rx_cursor(uint16_t instance_id, int32_t pos);

esp_err_t i2c_object_set_rx_buffer_size(uint16_t instance_id, int32_t size);
void i2c_object_set_handlers(interface_tx_handler_t tx_handler,
							 interface_rx_handler_t rx_handler,
							 interface_open_handler_t open_handler);
esp_err_t i2c_object_get_runtime(uint16_t instance_id, uint32_t *i2c_addr,
					 int32_t *sda_pin, int32_t *scl_pin,
					 int32_t *rx_buffer_size);
esp_err_t i2c_object_update_runtime(uint16_t instance_id, uint32_t i2c_addr,
							 int32_t sda_pin, int32_t scl_pin,
							 int32_t rx_buffer_size);
esp_err_t i2c_object_is_enabled(uint16_t instance_id, bool *enabled);

/* Invoke bridge handlers directly (used by Lua I2C bindings to share the bus) */
esp_err_t i2c_object_invoke_open(uint16_t instance_id, bool open);
ssize_t i2c_object_invoke_tx(uint16_t instance_id, const uint8_t *data, size_t len);
ssize_t i2c_object_invoke_rx(uint16_t instance_id, int32_t pos, uint8_t *out, size_t maxlen, int32_t *advanced);

lwm2m_object_t *get_rs485_object(void);
esp_err_t rs485_object_set_instance(uint16_t instance_id, const rs485_instance_cfg_t *cfg);
esp_err_t rs485_object_update_counters(uint16_t instance_id, uint32_t tx_bytes, uint32_t rx_bytes,
									   uint32_t error_count, uint32_t tx_rate, uint32_t rx_rate,
									   const char *last_error);
esp_err_t rs485_object_set_rx_cursor(uint16_t instance_id, int32_t pos);

esp_err_t rs485_object_set_rx_buffer_size(uint16_t instance_id, int32_t size);
void rs485_object_set_handlers(interface_tx_handler_t tx_handler,
							   interface_rx_handler_t rx_handler,
							   interface_open_handler_t open_handler);
esp_err_t rs485_object_get_runtime(uint16_t instance_id, uint32_t *baudrate, uint32_t *modbus_unit_id,
					 int32_t *tx_pin, int32_t *rx_pin,
					 int32_t *rx_buffer_size);
esp_err_t rs485_object_update_runtime(uint16_t instance_id, uint32_t baudrate, uint32_t modbus_unit_id,
							   int32_t tx_pin, int32_t rx_pin,
							   int32_t rx_buffer_size);
esp_err_t rs485_object_is_enabled(uint16_t instance_id, bool *enabled);
esp_err_t rs485_object_invoke_open(uint16_t instance_id, bool open);
ssize_t rs485_object_invoke_tx(uint16_t instance_id, const uint8_t *data, size_t len);
ssize_t rs485_object_invoke_rx(uint16_t instance_id, int32_t pos, uint8_t *out, size_t maxlen, int32_t *advanced);

lwm2m_object_t *get_uart_object(void);
esp_err_t uart_object_set_instance(uint16_t instance_id, const uart_instance_cfg_t *cfg);
esp_err_t uart_object_update_counters(uint16_t instance_id, uint32_t tx_bytes, uint32_t rx_bytes,
								  uint32_t error_count, uint32_t tx_rate, uint32_t rx_rate,
								  const char *last_error);
esp_err_t uart_object_set_rx_cursor(uint16_t instance_id, int32_t pos);
esp_err_t uart_object_set_rx_buffer_size(uint16_t instance_id, int32_t size);
void uart_object_set_handlers(interface_tx_handler_t tx_handler,
						  interface_rx_handler_t rx_handler,
						  interface_open_handler_t open_handler);
esp_err_t uart_object_get_runtime(uint16_t instance_id, uint32_t *baudrate,
						 int32_t *tx_pin, int32_t *rx_pin,
						 int32_t *rx_buffer_size);
esp_err_t uart_object_is_enabled(uint16_t instance_id, bool *enabled);
esp_err_t uart_object_invoke_open(uint16_t instance_id, bool open);
ssize_t uart_object_invoke_tx(uint16_t instance_id, const uint8_t *data, size_t len);
ssize_t uart_object_invoke_rx(uint16_t instance_id, int32_t pos, uint8_t *out, size_t maxlen, int32_t *advanced);

#ifdef __cplusplus
}
#endif
