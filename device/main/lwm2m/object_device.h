#pragma once

#include <stdint.h>
#include <stddef.h>
#include "lwm2mclient.h"

#ifdef __cplusplus
extern "C" {
#endif

lwm2m_object_t *get_object_device(void);
void free_object_device(lwm2m_object_t *objectP);

int device_get_battery_level(lwm2m_object_t *objectP, uint16_t instanceId, int64_t *out);
int device_get_free_memory(lwm2m_object_t *objectP, uint16_t instanceId, int64_t *out);
int device_get_error_code(lwm2m_object_t *objectP, uint16_t instanceId, int64_t *out);
int device_get_firmware_version(lwm2m_object_t *objectP, uint16_t instanceId, char *buffer, size_t bufLen);
int device_set_firmware_version(lwm2m_object_t *objectP, uint16_t instanceId, const char *fw);

int device_read_battery_mv(int32_t *out_mv);
int device_calculate_battery_level_from_mv(int32_t battery_mv, int32_t *out_level);

#ifdef __cplusplus
}
#endif
