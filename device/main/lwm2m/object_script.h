#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "lwm2mclient.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LWM2M_OBJ_SCRIPT 31025

#define RES_SCRIPT_ID       0
#define RES_SCRIPT_SIZE     1
#define RES_SCRIPT_CONTROL  2
#define RES_SCRIPT_CHUNK    3
#define RES_SCRIPT_NAME     4
#define RES_SCRIPT_EXECUTE  5
#define RES_SCRIPT_GLOBAL_BUFFER_SIZE 6
#define RES_SCRIPT_BUFFER_MIME_TYPE   7
#define RES_SCRIPT_GLOBAL_BUFFER_LEN  8
#define RES_SCRIPT_GLOBAL_BUFFER_DATA 9
#define RES_SCRIPT_EXEC_RESULT_JSON   10
#define RES_SCRIPT_VERSION            11

lwm2m_object_t *get_script_object(void);
esp_err_t lwm2m_script_build_aggregate(uint8_t *out_buf, size_t out_buf_size, size_t *out_len);
esp_err_t lwm2m_script_build_aggregate_for_ids(uint8_t *out_buf,
											   size_t out_buf_size,
											   const uint16_t *script_ids,
											   size_t script_id_count,
											   size_t *out_len);
esp_err_t lwm2m_script_build_aggregate_for_names(uint8_t *out_buf,
											 size_t out_buf_size,
											 const char * const *script_names,
											 size_t script_name_count,
											 size_t *out_len);
esp_err_t lwm2m_script_find_id_by_name(const char *script_name, uint16_t *out_id);
bool lwm2m_script_has_instance(uint16_t script_id);
bool lwm2m_script_has_nonempty_instance(uint16_t script_id);

#ifdef __cplusplus
}
#endif
