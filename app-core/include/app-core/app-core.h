#ifndef H_APP_CORE_H
#define H_APP_CORE_H

#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// message definitions for uplink and downlink
#define APP_CORE_MSG_MAX_SZ (250)
// 1st 2 bytes are header, then TLV blocks (1 byte T, 1byte L, n bytes V)
// byte 0 : b0-3 : lastDLId, b4-5 : protocol version, b6 : config stock, b7 : even parity bit
// byte 1 : length of following TLV section
typedef struct {
    uint8_t raw[APP_CORE_MSG_MAX_SZ];
    uint8_t currentOffset;
} APP_CORE_UL_t;

// Core api for modules to implement
typedef void (*APP_MOD_START_FN_t)();
typedef void (*APP_MOD_STOP_FN_t)();
typedef void (*APP_MOD_SLEEP_FN_t)();
typedef void (*APP_MOD_DEEPSLEEP_FN_t)();
typedef void (*APP_MOD_GETULDATA_FN_t)(APP_CORE_UL_t* ul);
typedef struct {
    APP_MOD_START_FN_t startCB;
    APP_MOD_STOP_FN_t stopCB;
    APP_MOD_SLEEP_FN_t sleepCB;
    APP_MOD_DEEPSLEEP_FN_t deepsleepCB;
    APP_MOD_GETULDATA_FN_t getULDataCB;
} APP_CORE_API_t;

typedef enum { APP_MOD_ENV, APP_MOD_GPS, APP_MOD_BLE_SCAN, ADD_MOD_BLE_IB } APP_MOD_ID_t;

// core api for modules
bool AppCore_registerModule(APP_MOD_ID_t id, APP_CORE_API_t mcbs);
uint32_t AppCore_getTimeToNextUL();
bool AppCore_forceUL();

// app core TLV tags for UL
typedef enum { APP_CORE_VERSION=0, APP_CORE_UPTIME, APP_CORE_CONFIG,
    APP_CORE_ENV_TEMP, APP_CORE_ENV_PRESSURE, APP_CORE_ENV_HUMIDITY, APP_CORE_ENV_LIGHT, 
    APP_CORE_BLE_CURR, APP_CORE_BLE_NEW, APP_CORE_BLE_LEFT, 
    APP_CORE_GPS
} APP_CORE_UL_TAGS;
// app core TLV tags for DL
typedef enum { APP_CORE_UTCTIME, 
    APP_CORE_ACTIONS, APP_CORE_FOTA, APP_CORE_SET_CONFIG, APP_CORE_GET_CONFIG,
    APP_CORE_ACTIVE_MODS
} APP_CORE_DL_TAGS;

#ifdef __cplusplus
}
#endif

#endif  /* H_APP_CORE_H */
