#pragma once

#include <stdbool.h>
#include "lwm2mclient.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LWM2M_LOG_OBJECT_ID
#define LWM2M_LOG_OBJECT_ID 10260
#endif

#define RES_LOG_LINES    0   // R, observable - returns buffered log lines
#define RES_LOG_CLEAR    1   // E - clear buffer
#define RES_LOG_DROPPED  2   // R - number of dropped lines
#define RES_LOG_PENDING  3   // R - number of pending lines
#define RES_LOG_NOTIFY   4   // R - number of notify attempts

lwm2m_object_t *get_object_log(void);
void free_object_log(lwm2m_object_t *objectP);
void log_object_install_hook(void);
void log_object_process(lwm2m_context_t *contextP);

#ifdef __cplusplus
}
#endif
