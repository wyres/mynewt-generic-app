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
typedef void (*APP_MOD_TIC_FN_t)();            // Callback for the tic registration 
typedef struct {
    APP_MOD_START_FN_t startCB;
    APP_MOD_STOP_FN_t stopCB;
    APP_MOD_SLEEP_FN_t sleepCB;             // may be null if has no sleeping actions to do
    APP_MOD_DEEPSLEEP_FN_t deepsleepCB;     // may be null if has no sleeping actions to do
    APP_MOD_GETULDATA_FN_t getULDataCB;
    APP_MOD_TIC_FN_t ticCB;                 // may be NULL if no ops to do
} APP_CORE_API_t;

// Add module ids here (before the APP_MOD_LAST enum)
typedef enum { APP_MOD_ENV=0, APP_MOD_GPS=1, APP_MOD_BLE_SCAN_NAV=2, APP_MOD_BLE_SCAN_TAGS=3, APP_MOD_BLE_IB=4, APP_MOD_IO=5, APP_MOD_LORA=6, APP_MOD_LAST } APP_MOD_ID_t;
// Should module be run in parallel with others, or must it be alone (eg coz using a shared resource like a bus)?
typedef enum { EXEC_PARALLEL, EXEC_SERIAL } APP_MOD_EXEC_t;
// core api for modules
void AppCore_registerModule(APP_MOD_ID_t id, APP_CORE_API_t* mcbs, APP_MOD_EXEC_t execType);
// is module active?
bool AppCore_getModuleState(APP_MOD_ID_t mid);
// set module active/not active
void AppCore_setModuleState(APP_MOD_ID_t mid, bool active);
// Timestamp of last UL (attempted)
uint32_t AppCore_lastULTime();
// Time in ms to next UL in theory
uint32_t AppCore_getTimeToNextUL();
// Go for UL preparation NOW - optionally with only requested module being run. If -1 then normal data collection.
bool AppCore_forceUL(int reqModule);
// Tell core we're done processing
void AppCore_module_done(APP_MOD_ID_t id);
// Register a DL action handler
void AppCore_registerAction(uint8_t id, ACTIONFN_t cb);
// Find an action handler or NULL
ACTIONFN_t AppCore_findAction(uint8_t id);

// app core TLV tags for UL : 1 byte sized, explicit values assigned, never change already allocated values!
typedef enum { APP_CORE_UL_VERSION=0, APP_CORE_UL_UPTIME=1, APP_CORE_UL_CONFIG=2,
    APP_CORE_UL_ENV_TEMP=3, APP_CORE_UL_ENV_PRESSURE=4, APP_CORE_UL_ENV_HUMIDIT=5, APP_CORE_UL_ENV_LIGHT=6, 
    APP_CORE_UL_ENV_BATTERY=7, APP_CORE_UL_ENV_ADC1=8, APP_CORE_UL_ENV_ADC2=9, 
    APP_CORE_UL_ENV_NOISE=10, APP_CORE_UL_ENV_BUTTON=11, 
    APP_CORE_UL_ENV_MOVE=12, APP_CORE_UL_ENV_FALL=13, APP_CORE_UL_ENV_SHOCK=14, APP_CORE_UL_ENV_ORIENT=15, 
    APP_CORE_UL_ENV_REBOOT=16, APP_CORE_UL_ENV_LASTASSERT=17,
    APP_CORE_UL_BLE_CURR=18, APP_CORE_UL_BLE_ENTER=19, APP_CORE_UL_BLE_EXIT=20, APP_CORE_UL_BLE_COUNT=21,
    APP_CORE_UL_GPS=22
} APP_CORE_UL_TAGS;
// app core TLV tags for DL : 1 byte sized, never change already allocated values! Note some are historic values
// App core has handlers for up to GET_MODS
typedef enum { APP_CORE_DL_REBOOT=1, APP_CORE_DL_SET_CONFIG=2, APP_CORE_DL_GET_CONFIG=3,         
    APP_CORE_DL_SET_UTCTIME=24, APP_CORE_DL_FOTA=25, APP_CORE_DL_GET_MODS=26, APP_CORE_DL_FIX_GPS=11
} APP_CORE_DL_TAGS;


// Configuration keys used by core - add to end of list as required. Never alter already assigned values.
#define CFG_UTIL_KEY_IDLE_TIME_MOVING_SECS      CFGKEY(CFG_MODULE_APP_CORE, 1)
#define CFG_UTIL_KEY_IDLE_TIME_NOTMOVING_MINS   CFGKEY(CFG_MODULE_APP_CORE, 2)
#define CFG_UTIL_KEY_MODSETUP_TIME_SECS         CFGKEY(CFG_MODULE_APP_CORE, 3)
#define CFG_UTIL_KEY_MODS_ACTIVE_MASK           CFGKEY(CFG_MODULE_APP_CORE, 4)
#define CFG_UTIL_KEY_MAXTIME_UL_MINS            CFGKEY(CFG_MODULE_APP_CORE, 5)
#define CFG_UTIL_KEY_DL_ID                      CFGKEY(CFG_MODULE_APP_CORE, 6)
#define CFG_UTIL_KEY_IDLE_TIME_CHECK_SECS       CFGKEY(CFG_MODULE_APP_CORE, 7)

// Configuration keys used by modules - add to end of list as required. Never alter already assigned values.
#define CFG_UTIL_KEY_BLE_SCAN_TIME_MS          CFGKEY(CFG_MODULE_APP_MOD, 1)
#define CFG_UTIL_KEY_GPS_COLD_TIME_SECS          CFGKEY(CFG_MODULE_APP_MOD, 2)
#define CFG_UTIL_KEY_GPS_WARM_TIME_SECS          CFGKEY(CFG_MODULE_APP_MOD, 3)
#define CFG_UTIL_KEY_GPS_POWER_MODE               CFGKEY(CFG_MODULE_APP_MOD, 4)
#define CFG_UTIL_KEY_GPS_FIX_MODE                 CFGKEY(CFG_MODULE_APP_MOD, 5)

#ifdef __cplusplus
}
#endif

#endif  /* H_APP_CORE_H */
