/**
 * Copyright 2019 Wyres
 * Licensed under the Apache License, Version 2.0 (the "License"); 
 * you may not use this file except in compliance with the License. 
 * You may obtain a copy of the License at
 *    http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, 
 * software distributed under the License is distributed on 
 * an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, 
 * either express or implied. See the License for the specific 
 * language governing permissions and limitations under the License.
*/
#ifndef H_APP_CORE_H
#define H_APP_CORE_H

#include <inttypes.h>
#include "app_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call from main() after sysinit() to start app core goodness
void app_core_start();

// Core api for modules to implement
typedef uint32_t (*APP_MOD_START_FN_t)();       // returns time required for its operation in ms
typedef void (*APP_MOD_STOP_FN_t)();
typedef void (*APP_MOD_SLEEP_FN_t)();
typedef void (*APP_MOD_DEEPSLEEP_FN_t)();
typedef bool (*APP_MOD_GETULDATA_FN_t)(APP_CORE_UL_t* ul);      // returns true if UL is 'critical',  false if not
typedef struct {
    APP_MOD_START_FN_t startCB;
    APP_MOD_STOP_FN_t stopCB;
    APP_MOD_SLEEP_FN_t sleepCB;
    APP_MOD_DEEPSLEEP_FN_t deepsleepCB;
    APP_MOD_GETULDATA_FN_t getULDataCB;
} APP_CORE_API_t;

// Add module ids here (before the APP_MOD_LAST enum)
typedef enum { APP_MOD_ENV=0, APP_MOD_GPS, APP_MOD_BLE_SCAN, APP_MOD_BLE_IB, APP_MOD_IO, APP_MOD_LORA, APP_MOD_LAST } APP_MOD_ID_t;
// Should module be run in parallel with others, or must it be alone (eg coz using a shared resource like a bus)?
typedef enum { EXEC_PARALLEL, EXEC_SERIAL } APP_MOD_EXEC_t;
// core api for modules
bool AppCore_registerModule(APP_MOD_ID_t id, APP_CORE_API_t* mcbs, APP_MOD_EXEC_t execType);
// Timestamp of last UL (attempted)
uint32_t AppCore_lastULTime();
// Time in ms to next UL in theory
uint32_t AppCore_getTimeToNextUL();
// Go for UL preparation NOW
bool AppCore_forceUL();
// Tell core we're done processing
void AppCore_module_done(APP_MOD_ID_t id);

// app core TLV tags for UL
typedef enum { APP_CORE_VERSION=0, APP_CORE_UPTIME, APP_CORE_CONFIG,
    APP_CORE_ENV_TEMP, APP_CORE_ENV_PRESSURE, APP_CORE_ENV_HUMIDITY, APP_CORE_ENV_LIGHT, 
    APP_CORE_ENV_BATTERY, APP_CORE_ENV_ADC1, APP_CORE_ENV_ADC2, 
    APP_CORE_ENV_NOISE, APP_CORE_ENV_BUTTON, 
    APP_CORE_ENV_MOVE, APP_CORE_ENV_FALL, APP_CORE_ENV_SHOCK, APP_CORE_ENV_ORIENT, 
    APP_CORE_ENV_REBOOT, APP_CORE_ENV_LASTASSERT,
    APP_CORE_BLE_CURR, APP_CORE_BLE_NEW, APP_CORE_BLE_LEFT, 
    APP_CORE_GPS
} APP_CORE_UL_TAGS;
// app core TLV tags for DL
typedef enum { APP_CORE_SET_UTCTIME, 
    APP_CORE_ACTIONS, APP_CORE_FOTA, APP_CORE_SET_CONFIG, APP_CORE_GET_CONFIG,
    APP_CORE_AVAILABLE_MODS
} APP_CORE_DL_TAGS;


// Configuration keys used by core
#define CFG_UTIL_KEY_IDLE_TIME_MOVING_SECS      CFGKEY(CFG_MODULE_APP_CORE, 1)
#define CFG_UTIL_KEY_IDLE_TIME_NOTMOVING_SECS   CFGKEY(CFG_MODULE_APP_CORE, 2)
#define CFG_UTIL_KEY_MODSETUP_TIME_SECS         CFGKEY(CFG_MODULE_APP_CORE, 3)
#define CFG_UTIL_KEY_MODS_ACTIVE_MASK           CFGKEY(CFG_MODULE_APP_CORE, 4)
#define CFG_UTIL_KEY_MAXTIME_UL_SECS            CFGKEY(CFG_MODULE_APP_CORE, 5)

// Configuration keys used by modules - add to end of list
#define CFG_UTIL_KEY_BLE_SCAN_TIME_SECS          CFGKEY(CFG_MODULE_APP_CORE, 128)
#define CFG_UTIL_KEY_GPS_COLD_TIME_SECS          CFGKEY(CFG_MODULE_APP_CORE, 129)
#define CFG_UTIL_KEY_GPS_WARM_TIME_SECS          CFGKEY(CFG_MODULE_APP_CORE, 130)

#ifdef __cplusplus
}
#endif

#endif  /* H_APP_CORE_H */
