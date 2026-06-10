#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "lwm2mclient.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LWM2M_OBJ_SAMPLE 31024

#define RES_SAMPLE_CONFIG_VERSION  0
#define RES_SAMPLE_ENABLED         1
#define RES_SAMPLE_RATE            2
#define RES_SAMPLE_REPORT_RATE     3
#define RES_SAMPLE_SLEEP_MODE      4
#define RES_SAMPLE_SENSOR_UART_I2C 5
#define RES_SAMPLE_SENSOR_RS485    6
#define RES_SAMPLE_DATA_FIELD      10
#define RES_SAMPLE_SEND_ACK_TIMEOUT_MS 11
#define RES_SAMPLE_SEND_RETRY_DELAY_MS 12
#define RES_SAMPLE_SEND_RETRY_COUNT    13
#define RES_SAMPLE_SERVER_ALIGNED_UPTIME_MS 14
#define RES_SAMPLE_UPTIME_DIFFERENCE_MS 15

#define SAMPLE_DATA_FIELD_MAX_LINES 30
#define SAMPLE_DATA_FIELD_LINE_MAX_LEN 350000

lwm2m_object_t *get_sample_object(void);

uint32_t lwm2m_sample_get_config_version(void);
int lwm2m_sample_get_i2c_script_missing(void);
int lwm2m_sample_get_rs485_script_missing(void);
esp_err_t lwm2m_sample_apply_json_config(const uint8_t *json, size_t len);
esp_err_t lwm2m_sample_set_data_field(const char *data, size_t len);
esp_err_t lwm2m_sample_set_data_field_multi(const uint8_t * const *lines,
											const size_t *line_lens,
											size_t line_count);

#ifdef __cplusplus
}
#endif
