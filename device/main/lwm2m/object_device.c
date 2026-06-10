/*******************************************************************************
 *
 * Copyright (c) 2013, 2014 Intel Corporation and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v20.html
 * The Eclipse Distribution License is available at
 *    http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    David Navarro, Intel Corporation - initial API and implementation
 *    domedambrosio - Please refer to git log
 *    Fabien Fleutot - Please refer to git log
 *    Axel Lorente - Please refer to git log
 *    Bosch Software Innovations GmbH - Please refer to git log
 *    Pascal Rieux - Please refer to git log
 *    Scott Bertin, AMETEK, Inc. - Please refer to git log
 *
 *******************************************************************************/

/*
 Copyright (c) 2013, 2014 Intel Corporation

 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

     * Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
     * Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.
     * Neither the name of Intel Corporation nor the names of its contributors
       may be used to endorse or promote products derived from this software
       without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 THE POSSIBILITY OF SUCH DAMAGE.

 David Navarro <david.navarro@intel.com>

*/

/*
 * This object supports multiple instances and is mandatory to all LWM2M device as it describe the object such as its
 * manufacturer, model, etc...
 */

#include "liblwm2m.h"
#include "lwm2mclient.h"
#include "lwm2m/object_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef ESP_PLATFORM
/* Exported by main driver code and backed by RTC_DATA_ATTR state. */
extern bool lwm2m_client_get_server_sec_of_year(uint32_t *sec_of_year_out);
#endif

// Logging helper: use ESP-IDF logging if available, fallback to printf otherwise
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#define DEVICE_LOGI(fmt, ...) ESP_LOGI("LWM2M_DEVICE", fmt, ##__VA_ARGS__)
#else
#define DEVICE_LOGI(fmt, ...) do { printf("[LWM2M_DEVICE] " fmt "\n", ##__VA_ARGS__); } while (0)
#endif


#ifdef ESP_PLATFORM
#if defined(CONFIG_LWM2M_DEVICE_MANUFACTURER)
#define PRV_MANUFACTURER      CONFIG_LWM2M_DEVICE_MANUFACTURER
#else
#define PRV_MANUFACTURER      "Open Mobile Alliance"
#endif

#if defined(CONFIG_LWM2M_DEVICE_MODEL)
#define PRV_MODEL_NUMBER      CONFIG_LWM2M_DEVICE_MODEL
#else
#define PRV_MODEL_NUMBER      "Lightweight M2M Client"
#endif

#if defined(CONFIG_LWM2M_DEVICE_FIRMWARE_VERSION)
#define PRV_FIRMWARE_VERSION  CONFIG_LWM2M_DEVICE_FIRMWARE_VERSION
#else
#define PRV_FIRMWARE_VERSION  "1.0"
#endif
#else
#define PRV_MANUFACTURER      "Open Mobile Alliance"
#define PRV_MODEL_NUMBER      "Lightweight M2M Client"
#define PRV_FIRMWARE_VERSION  "1.0"
#endif

#define PRV_SERIAL_NUMBER     "345000123"
#define PRV_POWER_SOURCE_1    1
#define PRV_POWER_SOURCE_2    5
#define PRV_POWER_VOLTAGE_1   3800
#define PRV_POWER_VOLTAGE_2   5000
#define PRV_POWER_CURRENT_1   125
#define PRV_POWER_CURRENT_2   900
#define PRV_BATTERY_LEVEL     100
#define PRV_MEMORY_FREE       15
#define PRV_ERROR_CODE        0
#define PRV_TIME_ZONE         "Europe/Berlin"
#define PRV_BINDING_MODE      "U"

#define PRV_TLV_BUFFER_SIZE 128
#define PRV_SECONDS_PER_DAY 86400
#define PRV_SECONDS_PER_YEAR (365 * PRV_SECONDS_PER_DAY)

#ifdef ESP_PLATFORM
#define PRV_BATTERY_ADC_CH         ADC_CHANNEL_3
#define PRV_BATTERY_ADC_ATTEN      ADC_ATTEN_DB_12
#define PRV_BATTERY_ADC_SAMPLES    10
#define PRV_BATTERY_MIN_MV         1800
#define PRV_BATTERY_MAX_MV         3000

typedef struct
{
    bool initialized;
    bool calibrated;
    adc_oneshot_unit_handle_t unit_handle;
    adc_cali_handle_t cali_handle;
} device_battery_adc_t;

static device_battery_adc_t s_battery_adc = {0};

static void prv_battery_adc_init(void)
{
    if (s_battery_adc.initialized)
    {
        return;
    }

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_2,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    if (adc_oneshot_new_unit(&init_cfg, &s_battery_adc.unit_handle) != ESP_OK)
    {
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = PRV_BATTERY_ADC_ATTEN,
    };

    if (adc_oneshot_config_channel(s_battery_adc.unit_handle, PRV_BATTERY_ADC_CH, &chan_cfg) != ESP_OK)
    {
        adc_oneshot_del_unit(s_battery_adc.unit_handle);
        memset(&s_battery_adc, 0, sizeof(s_battery_adc));
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_2,
            .chan = PRV_BATTERY_ADC_CH,
            .atten = PRV_BATTERY_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_battery_adc.cali_handle) == ESP_OK)
        {
            s_battery_adc.calibrated = true;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!s_battery_adc.calibrated)
    {
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_2,
            .atten = PRV_BATTERY_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_battery_adc.cali_handle) == ESP_OK)
        {
            s_battery_adc.calibrated = true;
        }
    }
#endif

    s_battery_adc.initialized = true;
}

static int32_t prv_read_battery_mv(void)
{
    int64_t sum_mv = 0;
    int samples = 0;
    int i;

    prv_battery_adc_init();
    if (!s_battery_adc.initialized)
    {
        return -1;
    }

    for (i = 0; i < PRV_BATTERY_ADC_SAMPLES; i++)
    {
        int raw = 0;
        int mv = 0;
        esp_err_t err;

        do
        {
            err = adc_oneshot_read(s_battery_adc.unit_handle, PRV_BATTERY_ADC_CH, &raw);
        } while (err == ESP_ERR_INVALID_STATE);

        if (err != ESP_OK)
        {
            continue;
        }

        if (s_battery_adc.calibrated)
        {
            if (adc_cali_raw_to_voltage(s_battery_adc.cali_handle, raw, &mv) != ESP_OK)
            {
                continue;
            }
        }
        else
        {
            mv = (raw * 3300) / 4095;
        }

        sum_mv += mv;
        samples++;
    }

    if (samples <= 0)
    {
        return -1;
    }

    /* Hardware divider reports roughly half battery voltage. */
    return (int32_t)((sum_mv / samples) * 2);
}

static int64_t prv_battery_level_from_mv(int32_t battery_mv)
{
    int32_t voltage_mv;

    if (battery_mv <= 0)
    {
        return PRV_BATTERY_LEVEL;
    }

    voltage_mv = battery_mv / 2;
    if (voltage_mv <= PRV_BATTERY_MIN_MV)
    {
        return (voltage_mv < 1000) ? 0 : 1;
    }

    if (voltage_mv >= PRV_BATTERY_MAX_MV)
    {
        return 100;
    }

    return (int64_t)((voltage_mv - PRV_BATTERY_MIN_MV) * 100 /
                     (PRV_BATTERY_MAX_MV - PRV_BATTERY_MIN_MV));
}
#endif

// Device instance data structure
typedef struct _device_instance_
{
    struct _device_instance_ * next;   // matches lwm2m_list_t
    uint16_t instanceId;               // matches lwm2m_list_t
    int64_t battery_level;
    int64_t free_memory;
    int64_t error;
    int64_t time;
    char manufacturer[64];
    char model_number[64];
    char serial_number[32];
    char time_offset[8];
    char firmware_version[64];
    char software_version[64];
} device_instance_t;

static void (*s_factory_reset_cb)(void) = NULL;
static void (*s_reboot_cb)(void) = NULL;
void lwm2m_device_set_reboot_cb(void (*cb)(void)) {
    s_reboot_cb = cb;
}
void lwm2m_device_set_factory_reset_cb(void (*cb)(void)) {
    s_factory_reset_cb = cb;
}

#ifdef ESP_PLATFORM
static void prv_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    DEVICE_LOGI("Executing deferred reboot via esp_restart()");
    esp_restart();
    vTaskDelete(NULL);
}
#endif

// Resource Id's:
#define RES_O_MANUFACTURER          0
#define RES_O_MODEL_NUMBER          1
#define RES_O_SERIAL_NUMBER         2
#define RES_O_FIRMWARE_VERSION      3
#define RES_M_REBOOT                4
#define RES_O_FACTORY_RESET         5
#define RES_O_AVL_POWER_SOURCES     6
#define RES_O_POWER_SOURCE_VOLTAGE  7
#define RES_O_POWER_SOURCE_CURRENT  8
#define RES_O_BATTERY_LEVEL         9
#define RES_O_MEMORY_FREE           10
#define RES_M_ERROR_CODE            11
#define RES_O_RESET_ERROR_CODE      12
#define RES_O_CURRENT_TIME          13
#define RES_O_UTC_OFFSET            14
#define RES_O_TIMEZONE              15
#define RES_M_BINDING_MODES         16
// since TS 20141126-C:
#define RES_O_DEVICE_TYPE           17
#define RES_O_HARDWARE_VERSION      18
#define RES_O_SOFTWARE_VERSION      19
#define RES_O_BATTERY_STATUS        20
#define RES_O_MEMORY_TOTAL          21


// basic check that the time offset value is at ISO 8601 format
// bug: +12:30 is considered a valid value by this function
static int prv_check_time_offset(char * buffer,
                                 int length)
{
    int min_index;

    if (length != 3 && length != 5 && length != 6) return 0;
    if (buffer[0] != '-' && buffer[0] != '+') return 0;
    switch (buffer[1])
    {
    case '0':
        if (buffer[2] < '0' || buffer[2] > '9') return 0;
        break;
    case '1':
        if (buffer[2] < '0' || (buffer[0] == '-' && buffer[2] > '2') || (buffer[0] == '+' && buffer[2] > '4')) return 0;
        break;
    default:
        return 0;
    }
    switch (length)
    {
    case 3:
        return 1;
    case 5:
        min_index = 3;
        break;
    case 6:
        if (buffer[3] != ':') return 0;
        min_index = 4;
        break;
    default:
        // never happen
        return 0;
    }
    if (buffer[min_index] < '0' || buffer[min_index] > '5') return 0;
    if (buffer[min_index+1] < '0' || buffer[min_index+1] > '9') return 0;

    return 1;
}

static uint8_t prv_set_value(lwm2m_data_t * dataP,
                             device_instance_t * devDataP,
                             lwm2m_context_t *contextP)
{
    lwm2m_data_t * subTlvP;
    size_t count;
    size_t i;
    // a simple switch structure is used to respond at the specified resource asked
    switch (dataP->id)
    {
    case RES_O_MANUFACTURER:
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
        lwm2m_data_encode_string(devDataP->manufacturer, dataP);
        DEVICE_LOGI("inst=%u READ MANUFACTURER=%s", devDataP->instanceId, devDataP->manufacturer);
        return COAP_205_CONTENT;

    case RES_O_MODEL_NUMBER:
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
        lwm2m_data_encode_string(devDataP->model_number, dataP);
        DEVICE_LOGI("inst=%u READ MODEL_NUMBER=%s", devDataP->instanceId, devDataP->model_number);
        return COAP_205_CONTENT;

    case RES_O_SERIAL_NUMBER:
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
        /* No per-instance serial number field in struct, use constant */
        lwm2m_data_encode_string(devDataP->serial_number, dataP);
        DEVICE_LOGI("inst=%u READ SERIAL_NUMBER=%s", devDataP->instanceId, devDataP->serial_number);
        return COAP_205_CONTENT;

    case RES_O_FIRMWARE_VERSION:
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
        lwm2m_data_encode_string(devDataP->firmware_version, dataP);
        DEVICE_LOGI("inst=%u READ FIRMWARE_VERSION=%s", devDataP->instanceId, devDataP->firmware_version);
        return COAP_205_CONTENT;

    case RES_O_SOFTWARE_VERSION:
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
        lwm2m_data_encode_string(devDataP->software_version, dataP);
        DEVICE_LOGI("inst=%u READ SOFTWARE_VERSION=%s", devDataP->instanceId, devDataP->software_version);
        return COAP_205_CONTENT;

    case RES_M_REBOOT:
        return COAP_405_METHOD_NOT_ALLOWED;

    case RES_O_FACTORY_RESET:
        return COAP_405_METHOD_NOT_ALLOWED;

    case RES_O_AVL_POWER_SOURCES: 
    {
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE)
        {
            count = dataP->value.asChildren.count;
            subTlvP = dataP->value.asChildren.array;
        }
        else
        {
            count = 2;
            subTlvP = lwm2m_data_new(count);
            for (i = 0; i < count; i++) subTlvP[i].id = i;
            lwm2m_data_encode_instances(subTlvP, count, dataP);
        }

        for (i = 0; i < count; i++)
        {
            switch (subTlvP[i].id)
            {
            case 0:
                lwm2m_data_encode_int(PRV_POWER_SOURCE_1, subTlvP + i);
                DEVICE_LOGI("inst=%u READ POWER_SOURCE[0]=%d", devDataP->instanceId, PRV_POWER_SOURCE_1);
                break;
            case 1:
                lwm2m_data_encode_int(PRV_POWER_SOURCE_2, subTlvP + i);
                DEVICE_LOGI("inst=%u READ POWER_SOURCE[1]=%d", devDataP->instanceId, PRV_POWER_SOURCE_2);
                break;
            default:
                return COAP_404_NOT_FOUND;
            }
        }

        return COAP_205_CONTENT;
    }

    case RES_O_POWER_SOURCE_VOLTAGE:
    {
        int32_t battery_mv = -1;
    #ifdef ESP_PLATFORM
        battery_mv = prv_read_battery_mv();
    #endif

        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE)
        {
            count = dataP->value.asChildren.count;
            subTlvP = dataP->value.asChildren.array;
        }
        else
        {
            count = 2;
            subTlvP = lwm2m_data_new(count);
            for (i = 0; i < count; i++) subTlvP[i].id = i;
            lwm2m_data_encode_instances(subTlvP, count, dataP);
        }

        for (i = 0; i < count; i++)
        {
            switch (subTlvP[i].id)
            {
            case 0:
                lwm2m_data_encode_int((battery_mv > 0) ? battery_mv : PRV_POWER_VOLTAGE_1, subTlvP + i);
                DEVICE_LOGI("inst=%u READ POWER_VOLTAGE[0]=%d", devDataP->instanceId,
                            (battery_mv > 0) ? battery_mv : PRV_POWER_VOLTAGE_1);
                break;
            case 1:
                lwm2m_data_encode_int(PRV_POWER_VOLTAGE_2, subTlvP + i);
                DEVICE_LOGI("inst=%u READ POWER_VOLTAGE[1]=%d", devDataP->instanceId, PRV_POWER_VOLTAGE_2);
                break;
            default:
                return COAP_404_NOT_FOUND;
            }
        }

        return COAP_205_CONTENT;
    }

    case RES_O_POWER_SOURCE_CURRENT:
    {
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE)
        {
            count = dataP->value.asChildren.count;
            subTlvP = dataP->value.asChildren.array;
        }
        else
        {
            count = 2;
            subTlvP = lwm2m_data_new(count);
            for (i = 0; i < count; i++) subTlvP[i].id = i;
            lwm2m_data_encode_instances(subTlvP, count, dataP);
        }

        for (i = 0; i < count; i++)
        {
            switch (subTlvP[i].id)
            {
            case 0:
                lwm2m_data_encode_int(PRV_POWER_CURRENT_1, subTlvP + i);
                DEVICE_LOGI("inst=%u READ POWER_CURRENT[0]=%d", devDataP->instanceId, PRV_POWER_CURRENT_1);
                break;
            case 1:
                lwm2m_data_encode_int(PRV_POWER_CURRENT_2, subTlvP + i);
                DEVICE_LOGI("inst=%u READ POWER_CURRENT[1]=%d", devDataP->instanceId, PRV_POWER_CURRENT_2);
                break;
            default:
                return COAP_404_NOT_FOUND;
            }
        }

        return COAP_205_CONTENT;
    }

    case RES_O_BATTERY_LEVEL:
    {
        int32_t battery_mv = -1;
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
#ifdef ESP_PLATFORM
        battery_mv = prv_read_battery_mv();
        if (battery_mv > 0)
        {
            devDataP->battery_level = prv_battery_level_from_mv(battery_mv);
        }
#endif
        lwm2m_data_encode_int(devDataP->battery_level, dataP);
        DEVICE_LOGI("inst=%u READ BATTERY_LEVEL=%lld", devDataP->instanceId, (long long)devDataP->battery_level);
        return COAP_205_CONTENT;
    }

    case RES_O_MEMORY_FREE:
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
        lwm2m_data_encode_int(devDataP->free_memory, dataP);
        DEVICE_LOGI("inst=%u READ MEMORY_FREE=%lld", devDataP->instanceId, (long long)devDataP->free_memory);
        return COAP_205_CONTENT;

    case RES_M_ERROR_CODE:
    {
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE)
        {
            count = dataP->value.asChildren.count;
            subTlvP = dataP->value.asChildren.array;
        }
        else
        {
            count = 1;
            subTlvP = lwm2m_data_new(count);
            for (i = 0; i < count; i++) subTlvP[i].id = i;
            lwm2m_data_encode_instances(subTlvP, count, dataP);
        }

        for (i = 0; i < count; i++)
        {
            switch (subTlvP[i].id)
            {
            case 0:
                lwm2m_data_encode_int(devDataP->error, subTlvP + i);
                DEVICE_LOGI("inst=%u READ ERROR_CODE=%lld", devDataP->instanceId, (long long)devDataP->error);
                break;
            default:
                return COAP_404_NOT_FOUND;
            }
        }

        return COAP_205_CONTENT;
    }        
    case RES_O_RESET_ERROR_CODE:
        return COAP_405_METHOD_NOT_ALLOWED;

    case RES_O_CURRENT_TIME:
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
        {
            time_t now = time(NULL);
            int64_t nowVal = (int64_t)now + devDataP->time;

#ifdef ESP_PLATFORM
            uint32_t rtc_server_sec_of_year = 0;
            if (lwm2m_client_get_server_sec_of_year(&rtc_server_sec_of_year))
            {
                struct tm now_tm;
                if (gmtime_r(&now, &now_tm) != NULL)
                {
                    int64_t local_sec_of_year =
                        ((int64_t)now_tm.tm_yday * PRV_SECONDS_PER_DAY) +
                        ((int64_t)now_tm.tm_hour * 3600) +
                        ((int64_t)now_tm.tm_min * 60) +
                        (int64_t)now_tm.tm_sec;
                    int64_t delta = (int64_t)rtc_server_sec_of_year - local_sec_of_year;

                    if (delta > (PRV_SECONDS_PER_YEAR / 2))
                    {
                        delta -= PRV_SECONDS_PER_YEAR;
                    }
                    else if (delta < -(PRV_SECONDS_PER_YEAR / 2))
                    {
                        delta += PRV_SECONDS_PER_YEAR;
                    }

                    nowVal = (int64_t)now + delta;
                }
            }
            else
#endif
            if (contextP != NULL && lwm2m_registration_server_sec_of_year_is_valid(contextP))
            {
                uint32_t server_sec_of_year = lwm2m_registration_server_sec_of_year(contextP);
                struct tm now_tm;
                if (gmtime_r(&now, &now_tm) != NULL)
                {
                    int64_t local_sec_of_year =
                        ((int64_t)now_tm.tm_yday * PRV_SECONDS_PER_DAY) +
                        ((int64_t)now_tm.tm_hour * 3600) +
                        ((int64_t)now_tm.tm_min * 60) +
                        (int64_t)now_tm.tm_sec;
                    int64_t delta = (int64_t)server_sec_of_year - local_sec_of_year;

                    if (delta > (PRV_SECONDS_PER_YEAR / 2))
                    {
                        delta -= PRV_SECONDS_PER_YEAR;
                    }
                    else if (delta < -(PRV_SECONDS_PER_YEAR / 2))
                    {
                        delta += PRV_SECONDS_PER_YEAR;
                    }

                    nowVal = (int64_t)now + delta;
                }
            }

            lwm2m_data_encode_int(nowVal, dataP);
            DEVICE_LOGI("inst=%u READ CURRENT_TIME=%lld", devDataP->instanceId, (long long)nowVal);
        }
        return COAP_205_CONTENT;

    case RES_O_UTC_OFFSET:
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
        lwm2m_data_encode_string(devDataP->time_offset, dataP);
        DEVICE_LOGI("inst=%u READ UTC_OFFSET=%s", devDataP->instanceId, devDataP->time_offset);
        return COAP_205_CONTENT;

    case RES_O_TIMEZONE:
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
        lwm2m_data_encode_string(PRV_TIME_ZONE, dataP);
        DEVICE_LOGI("inst=%u READ TIMEZONE=%s", devDataP->instanceId, PRV_TIME_ZONE);
        return COAP_205_CONTENT;
      
    case RES_M_BINDING_MODES:
        if (dataP->type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
        lwm2m_data_encode_string(PRV_BINDING_MODE, dataP);
        DEVICE_LOGI("inst=%u READ BINDING_MODES=%s", devDataP->instanceId, PRV_BINDING_MODE);
        return COAP_205_CONTENT;

    default:
        return COAP_404_NOT_FOUND;
    }
}

static uint8_t prv_device_read(lwm2m_context_t *contextP,
                               uint16_t instanceId,
                               int * numDataP,
                               lwm2m_data_t ** dataArrayP,
                               lwm2m_object_t * objectP)
{
    uint8_t result;
    int i;
    device_instance_t * targetP;

    /* unused parameter */
    (void)contextP;

    // Find the requested instance
    targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (NULL == targetP)
    {
        return COAP_404_NOT_FOUND;
    }

    // is the server asking for the full object ?
    if (*numDataP == 0)
    {
        uint16_t resList[] = {
                RES_O_MANUFACTURER,
                RES_O_MODEL_NUMBER,
                RES_O_SERIAL_NUMBER,
                RES_O_FIRMWARE_VERSION,
                RES_O_SOFTWARE_VERSION,
                //E: RES_M_REBOOT,
                //E: RES_O_FACTORY_RESET,
                RES_O_AVL_POWER_SOURCES,
                RES_O_POWER_SOURCE_VOLTAGE,
                RES_O_POWER_SOURCE_CURRENT,
                RES_O_BATTERY_LEVEL,
                RES_O_MEMORY_FREE,
                RES_M_ERROR_CODE,
                //E: RES_O_RESET_ERROR_CODE,
                RES_O_CURRENT_TIME,
                RES_O_UTC_OFFSET,
                RES_O_TIMEZONE,
                RES_M_BINDING_MODES
        };
        int nbRes = sizeof(resList)/sizeof(uint16_t);

        *dataArrayP = lwm2m_data_new(nbRes);
        if (*dataArrayP == NULL) return COAP_500_INTERNAL_SERVER_ERROR;
        *numDataP = nbRes;
        for (i = 0 ; i < nbRes ; i++)
        {
            (*dataArrayP)[i].id = resList[i];
        }
    }

    i = 0;
    do
    {
        result = prv_set_value((*dataArrayP) + i, targetP, contextP);
        i++;
    } while (i < *numDataP && result == COAP_205_CONTENT);

    return result;
}

static uint8_t prv_device_discover(lwm2m_context_t *contextP,
                                   uint16_t instanceId,
                                   int * numDataP,
                                   lwm2m_data_t ** dataArrayP,
                                   lwm2m_object_t * objectP)
{
    uint8_t result;
    int i;
    device_instance_t * targetP;

    /* unused parameter */
    (void)contextP;

    // Find the requested instance
    targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (NULL == targetP)
    {
        return COAP_404_NOT_FOUND;
    }

    result = COAP_205_CONTENT;

    // is the server asking for the full object ?
    if (*numDataP == 0)
    {
        uint16_t resList[] = {
            RES_O_MANUFACTURER,
            RES_O_MODEL_NUMBER,
            RES_O_SERIAL_NUMBER,
            RES_O_FIRMWARE_VERSION,
            RES_O_SOFTWARE_VERSION,
            RES_M_REBOOT,
            RES_O_FACTORY_RESET,
            RES_O_AVL_POWER_SOURCES,
            RES_O_POWER_SOURCE_VOLTAGE,
            RES_O_POWER_SOURCE_CURRENT,
            RES_O_BATTERY_LEVEL,
            RES_O_MEMORY_FREE,
            RES_M_ERROR_CODE,
            RES_O_RESET_ERROR_CODE,
            RES_O_CURRENT_TIME,
            RES_O_UTC_OFFSET,
            RES_O_TIMEZONE,
            RES_M_BINDING_MODES
        };
        int nbRes = sizeof(resList) / sizeof(uint16_t);

        *dataArrayP = lwm2m_data_new(nbRes);
        if (*dataArrayP == NULL) return COAP_500_INTERNAL_SERVER_ERROR;
        *numDataP = nbRes;
        for (i = 0; i < nbRes; i++)
        {
            (*dataArrayP)[i].id = resList[i];
        }
    }
    else
    {
        for (i = 0; i < *numDataP && result == COAP_205_CONTENT; i++)
        {
            switch ((*dataArrayP)[i].id)
            {
            case RES_O_MANUFACTURER:
            case RES_O_MODEL_NUMBER:
            case RES_O_SERIAL_NUMBER:
            case RES_O_FIRMWARE_VERSION:
            case RES_O_SOFTWARE_VERSION:
            case RES_M_REBOOT:
            case RES_O_FACTORY_RESET:
            case RES_O_AVL_POWER_SOURCES:
            case RES_O_POWER_SOURCE_VOLTAGE:
            case RES_O_POWER_SOURCE_CURRENT:
            case RES_O_BATTERY_LEVEL:
            case RES_O_MEMORY_FREE:
            case RES_M_ERROR_CODE:
            case RES_O_RESET_ERROR_CODE:
            case RES_O_CURRENT_TIME:
            case RES_O_UTC_OFFSET:
            case RES_O_TIMEZONE:
            case RES_M_BINDING_MODES:
                break;
            default:
                result = COAP_404_NOT_FOUND;
            }
        }
    }

    return result;
}

static uint8_t prv_device_write(lwm2m_context_t *contextP,
                                uint16_t instanceId,
                                int numData,
                                lwm2m_data_t * dataArray,
                                lwm2m_object_t * objectP,
                                lwm2m_write_type_t writeType)
{
    int i;
    uint8_t result;
    device_instance_t * targetP;

    /* unused parameter */
    (void)contextP;

    // All write types are treated the same here
    (void)writeType;

    // Find the requested instance
    targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (NULL == targetP)
    {
        return COAP_404_NOT_FOUND;
    }

    i = 0;

    do
    {
        /* No multiple instance resources */
        if (dataArray[i].type == LWM2M_TYPE_MULTIPLE_RESOURCE)
        {
            result = COAP_404_NOT_FOUND;
            continue;
        }

        switch (dataArray[i].id)
        {
        case RES_O_CURRENT_TIME:
            if (1 == lwm2m_data_decode_int(dataArray + i, &targetP->time))
            {
                targetP->time -= time(NULL);
                result = COAP_204_CHANGED;
            }
            else
            {
                result = COAP_400_BAD_REQUEST;
            }
            break;

        case RES_O_UTC_OFFSET:
            if (1 == prv_check_time_offset((char*)dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length))
            {
                strncpy(targetP->time_offset, (char*)dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length);
                targetP->time_offset[dataArray[i].value.asBuffer.length] = 0;
                result = COAP_204_CHANGED;
            }
            else
            {
                result = COAP_400_BAD_REQUEST;
            }
            break;

        case RES_O_TIMEZONE:
            //ToDo IANA TZ Format
            result = COAP_501_NOT_IMPLEMENTED;
            break;

        // Added support for updating writable numeric resources on a per-instance basis
        case RES_O_BATTERY_LEVEL:
        {
            int64_t value;
            if (1 == lwm2m_data_decode_int(dataArray + i, &value))
            {
                if (value >= 0 && value <= 100)
                {
                    targetP->battery_level = value;
                    result = COAP_204_CHANGED;
                }
                else
                {
                    result = COAP_400_BAD_REQUEST; // out of allowed range
                }
            }
            else
            {
                result = COAP_400_BAD_REQUEST; // decode failure
            }
        }
            break;
        case RES_M_ERROR_CODE:
            if (1 == lwm2m_data_decode_int(dataArray + i, &targetP->error))
            {
                result = COAP_204_CHANGED;
            }
            else
            {
                result = COAP_400_BAD_REQUEST;
            }
            break;
        case RES_O_MEMORY_FREE:
        {
            int64_t value;
            if (1 == lwm2m_data_decode_int(dataArray + i, &value))
            {
                if (value >= 0)
                {
                    targetP->free_memory = value;
                    result = COAP_204_CHANGED;
                }
                else
                {
                    result = COAP_400_BAD_REQUEST; // negative memory value
                }
            }
            else
            {
                result = COAP_400_BAD_REQUEST; // decode failure
            }
        }
            break;
            
        default:
            result = COAP_405_METHOD_NOT_ALLOWED;
        }

        i++;
    } while (i < numData && result == COAP_204_CHANGED);

    return result;
}

static uint8_t prv_device_execute(lwm2m_context_t *contextP,
                                  uint16_t instanceId,
                                  uint16_t resourceId,
                                  uint8_t * buffer,
                                  int length,
                                  lwm2m_object_t * objectP)
{
    device_instance_t * targetP;
    
    /* unused parameter */
    (void)contextP;

    // Find the requested instance
    targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (NULL == targetP)
    {
        return COAP_404_NOT_FOUND;
    }

    // Log execute parameters if provided
    if (length > 0 && buffer != NULL) {
        DEVICE_LOGI("Execute with %d bytes of parameters", length);
        // Parameters are optional for LwM2M execute operations
    }

    switch (resourceId)
    {
    case RES_M_REBOOT:
        fprintf(stdout, "\n\t REBOOT");
        if (length > 0 && buffer != NULL) {
            fprintf(stdout, " (with params: %.*s)", length, (char*)buffer);
        }
        fprintf(stdout, "\r\n\n");
        if (s_reboot_cb) {
            DEVICE_LOGI("Reboot execute received; invoking callback");
            s_reboot_cb();
        } else {
#ifdef ESP_PLATFORM
            BaseType_t task_created = xTaskCreate(
                prv_reboot_task,
                "lwm2m_reboot",
                2048,
                NULL,
                tskIDLE_PRIORITY + 1,
                NULL);
            if (task_created == pdPASS) {
                DEVICE_LOGI("Reboot callback not set; scheduled deferred esp_restart() fallback");
            } else {
                g_reboot = 1;
                DEVICE_LOGI("Reboot callback not set; failed to schedule reboot task, falling back to g_reboot flag");
            }
#else
            g_reboot = 1;
            DEVICE_LOGI("Reboot callback not set; falling back to legacy g_reboot flag");
#endif
        }
        return COAP_204_CHANGED;
    case RES_O_FACTORY_RESET:
        fprintf(stdout, "\n\t FACTORY RESET");
        if (length > 0 && buffer != NULL) {
            fprintf(stdout, " (with params: %.*s)", length, (char*)buffer);
        }
        fprintf(stdout, "\r\n\n");
        if (s_factory_reset_cb) {
            DEVICE_LOGI("Factory reset execute received; invoking callback");
            s_factory_reset_cb();
        } else {
            DEVICE_LOGI("Factory reset callback not set; ignoring request");
        }
        return COAP_204_CHANGED;
    case RES_O_RESET_ERROR_CODE:
        fprintf(stdout, "\n\t RESET ERROR CODE");
        if (length > 0 && buffer != NULL) {
            fprintf(stdout, " (with params: %.*s)", length, (char*)buffer);
        }
        fprintf(stdout, "\r\n\n");
        targetP->error = 0;
        return COAP_204_CHANGED;
    default:
        return COAP_405_METHOD_NOT_ALLOWED;
    }
}

void display_device_object(lwm2m_object_t * object)
{
    device_instance_t * instanceP = (device_instance_t *)object->instanceList;
    fprintf(stdout, "  /%u: Device object:\r\n", object->objID);
    while (NULL != instanceP)
    {
        fprintf(stdout, "    Instance %u: time: %lld, time_offset: %s, battery: %lld, memory: %lld, error: %lld\r\n",
                instanceP->instanceId,
                (long long) instanceP->time, 
                instanceP->time_offset,
                (long long) instanceP->battery_level,
                (long long) instanceP->free_memory,
                (long long) instanceP->error);
        instanceP = instanceP->next;
    }
}

lwm2m_object_t *get_object_device(void) {
    /*
     * The get_object_device function create the object itself and return a pointer to the structure that represent it.
     */
    lwm2m_object_t * deviceObj;

    deviceObj = (lwm2m_object_t *)lwm2m_malloc(sizeof(lwm2m_object_t));

    if (NULL != deviceObj)
    {
        memset(deviceObj, 0, sizeof(lwm2m_object_t));

        /*
         * It assigns his unique ID
         * The 3 is the standard ID for the mandatory object "Object device".
         */
        deviceObj->objID = LWM2M_DEVICE_OBJECT_ID;

        /*
         * Initialize with empty instance list - instances will be added via device_add_instance
         */
        deviceObj->instanceList = NULL;
        
        /*
         * And the private function that will access the object.
         * Those function will be called when a read/write/execute query is made by the server. In fact the library don't need to
         * know the resources of the object, only the server does.
         */
        deviceObj->readFunc     = prv_device_read;
        deviceObj->discoverFunc = prv_device_discover;
        deviceObj->writeFunc    = prv_device_write;
        deviceObj->executeFunc  = prv_device_execute;
        deviceObj->userData     = NULL;  // No global user data needed anymore
    }

    return deviceObj;
}

void free_object_device(lwm2m_object_t * objectP)
{
    device_instance_t * instanceP;
    
    while (NULL != objectP->instanceList)
    {
        instanceP = (device_instance_t *)objectP->instanceList;
        objectP->instanceList = objectP->instanceList->next;
        lwm2m_free(instanceP);
    }

    lwm2m_free(objectP);
}

uint8_t device_change(lwm2m_data_t * dataArray,
                      lwm2m_object_t * objectP,
                      uint16_t instanceId)
{
    uint8_t result;
    device_instance_t * targetP;

    // Find the requested instance
    targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (NULL == targetP)
    {
        return COAP_404_NOT_FOUND;
    }

    switch (dataArray->id)
    {
    case RES_O_BATTERY_LEVEL:
            {
                int64_t value;
                if (1 == lwm2m_data_decode_int(dataArray, &value))
                {
                    if ((0 <= value) && (100 >= value))
                    {
                        targetP->battery_level = value;
                        result = COAP_204_CHANGED;
                    }
                    else
                    {
                        result = COAP_400_BAD_REQUEST;
                    }
                }
                else
                {
                    result = COAP_400_BAD_REQUEST;
                }
            }
            break;
        case RES_M_ERROR_CODE:
            if (1 == lwm2m_data_decode_int(dataArray, &targetP->error))
            {
                result = COAP_204_CHANGED;
            }
            else
            {
                result = COAP_400_BAD_REQUEST;
            }
            break;
        case RES_O_MEMORY_FREE:
            if (1 == lwm2m_data_decode_int(dataArray, &targetP->free_memory))
            {
                result = COAP_204_CHANGED;
            }
            else
            {
                result = COAP_400_BAD_REQUEST;
            }
            break;
        default:
            result = COAP_405_METHOD_NOT_ALLOWED;
            break;
        }
    
    return result;
}

// Add a new device instance with specified instanceId
uint8_t device_add_instance(lwm2m_object_t * objectP, uint16_t instanceId)
{
    device_instance_t * targetP;
    
    // Check if instance already exists
    targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (NULL != targetP)
    {
        return COAP_406_NOT_ACCEPTABLE;  // Instance already exists
    }
    
    // Create new instance
    targetP = (device_instance_t *)lwm2m_malloc(sizeof(device_instance_t));
    if (NULL == targetP)
    {
        return COAP_500_INTERNAL_SERVER_ERROR;
    }
    
    memset(targetP, 0, sizeof(device_instance_t));
    targetP->instanceId = instanceId;
    
    // Initialize with default values
    targetP->battery_level = PRV_BATTERY_LEVEL;
    targetP->free_memory = PRV_MEMORY_FREE;
    targetP->error = PRV_ERROR_CODE;
    targetP->time = 0;
    strncpy(targetP->manufacturer, PRV_MANUFACTURER, sizeof(targetP->manufacturer)-1);
    targetP->manufacturer[sizeof(targetP->manufacturer)-1] = '\0';
    strncpy(targetP->model_number, PRV_MODEL_NUMBER, sizeof(targetP->model_number)-1);
    targetP->model_number[sizeof(targetP->model_number)-1] = '\0';
    strcpy(targetP->time_offset, "+01:00");
    strncpy(targetP->firmware_version, PRV_FIRMWARE_VERSION, sizeof(targetP->firmware_version)-1);
    targetP->firmware_version[sizeof(targetP->firmware_version)-1] = '\0';
    strncpy(targetP->software_version, PRV_FIRMWARE_VERSION, sizeof(targetP->software_version)-1);
    targetP->software_version[sizeof(targetP->software_version)-1] = '\0';
    
    // Add to instance list
    objectP->instanceList = LWM2M_LIST_ADD(objectP->instanceList, targetP);
    
    return COAP_201_CREATED;
}

// Remove a device instance
uint8_t device_remove_instance(lwm2m_object_t * objectP, uint16_t instanceId)
{
    device_instance_t * targetP;
    
    // Find and remove the instance
    objectP->instanceList = lwm2m_list_remove(objectP->instanceList, instanceId, (lwm2m_list_t **)&targetP);
    
    if (NULL == targetP)
    {
        return COAP_404_NOT_FOUND;
    }
    
    lwm2m_free(targetP);
    return COAP_202_DELETED;
}

// Update instance value with validation
uint8_t device_update_instance_value(lwm2m_object_t * objectP, uint16_t instanceId, uint16_t resourceId, int64_t value)
{
    device_instance_t * targetP;
    
    // Find the requested instance
    targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (NULL == targetP)
    {
        return COAP_404_NOT_FOUND;
    }
    
    switch (resourceId)
    {
    case RES_O_BATTERY_LEVEL:
        if ((0 <= value) && (100 >= value))
        {
            targetP->battery_level = value;
            return COAP_204_CHANGED;
        }
        return COAP_400_BAD_REQUEST;
        
    case RES_M_ERROR_CODE:
        targetP->error = value;
        return COAP_204_CHANGED;
        
    case RES_O_MEMORY_FREE:
        if (value >= 0)
        {
            targetP->free_memory = value;
            return COAP_204_CHANGED;
        }
        return COAP_400_BAD_REQUEST;
        
    case RES_O_CURRENT_TIME:
        targetP->time = value;
        return COAP_204_CHANGED;
        
    default:
        return COAP_405_METHOD_NOT_ALLOWED;
    }
}

// Update instance string value (for time offset)
uint8_t device_update_instance_string(lwm2m_object_t * objectP, uint16_t instanceId, uint16_t resourceId, const char* value)
{
    device_instance_t * targetP;
    
    // Find the requested instance
    targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (NULL == targetP)
    {
        return COAP_404_NOT_FOUND;
    }
    
    switch (resourceId)
    {
    case RES_O_MANUFACTURER:
        if (value != NULL && strlen(value) < sizeof(targetP->manufacturer))
        {
            strcpy(targetP->manufacturer, value);
            return COAP_204_CHANGED;
        }
        return COAP_400_BAD_REQUEST;
    case RES_O_MODEL_NUMBER:
        if (value != NULL && strlen(value) < sizeof(targetP->model_number))
        {
            strcpy(targetP->model_number, value);
            return COAP_204_CHANGED;
        }
        return COAP_400_BAD_REQUEST;
    case RES_O_SERIAL_NUMBER:
        if (value != NULL && strlen(value) < sizeof(targetP->serial_number))
        {
            strcpy(targetP->serial_number, value);
            return COAP_204_CHANGED;
        }
        return COAP_400_BAD_REQUEST;
    case RES_O_FIRMWARE_VERSION:
        if (value != NULL && strlen(value) < sizeof(targetP->firmware_version))
        {
            strcpy(targetP->firmware_version, value);
            return COAP_204_CHANGED;
        }
        return COAP_400_BAD_REQUEST;
    case RES_O_SOFTWARE_VERSION:
        if (value != NULL && strlen(value) < sizeof(targetP->software_version))
        {
            strcpy(targetP->software_version, value);
            return COAP_204_CHANGED;
        }
        return COAP_400_BAD_REQUEST;
    case RES_O_UTC_OFFSET:
        if (value != NULL && strlen(value) < sizeof(targetP->time_offset))
        {
            if (prv_check_time_offset((char*)value, strlen(value)))
            {
                strcpy(targetP->time_offset, value);
                return COAP_204_CHANGED;
            }
        }
        return COAP_400_BAD_REQUEST;
        
    default:
        return COAP_405_METHOD_NOT_ALLOWED;
    }
}

// Helper getters for external code wanting single-resource reads without crafting LwM2M read transactions
int device_get_battery_level(lwm2m_object_t * objectP, uint16_t instanceId, int64_t *out)
{
    device_instance_t * targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (!targetP || !out) return -1;
    *out = targetP->battery_level;
    return 0;
}

int device_get_free_memory(lwm2m_object_t * objectP, uint16_t instanceId, int64_t *out)
{
    device_instance_t * targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (!targetP || !out) return -1;
    *out = targetP->free_memory;
    return 0;
}

int device_get_error_code(lwm2m_object_t * objectP, uint16_t instanceId, int64_t *out)
{
    device_instance_t * targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (!targetP || !out) return -1;
    *out = targetP->error;
    return 0;
}

int device_get_firmware_version(lwm2m_object_t * objectP, uint16_t instanceId, char *buffer, size_t bufLen)
{
    device_instance_t * targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (!targetP || !buffer || bufLen == 0) return -1;
    strncpy(buffer, targetP->firmware_version, bufLen - 1);
    buffer[bufLen - 1] = '\0';
    return 0;
}

int device_set_firmware_version(lwm2m_object_t * objectP, uint16_t instanceId, const char *fw)
{
    device_instance_t * targetP = (device_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (!targetP || !fw) return -1;
    if (strlen(fw) >= sizeof(targetP->firmware_version)) return -2; // too long
    strcpy(targetP->firmware_version, fw);
    return 0;
}

int device_read_battery_mv(int32_t *out_mv)
{
    if (!out_mv) {
        return -1;
    }

#ifdef ESP_PLATFORM
    *out_mv = prv_read_battery_mv();
#else
    *out_mv = PRV_POWER_VOLTAGE_1;
#endif

    return 0;
}

int device_calculate_battery_level_from_mv(int32_t battery_mv, int32_t *out_level)
{
    if (!out_level) {
        return -1;
    }

#ifdef ESP_PLATFORM
    *out_level = (int32_t)prv_battery_level_from_mv(battery_mv);
#else
    (void)battery_mv;
    *out_level = PRV_BATTERY_LEVEL;
#endif

    return 0;
}
