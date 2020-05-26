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

// BLE IBEACON : config the BLE card to be an ibeacon managed by this firmware
// TODO finish design of this module
#include "os/os.h"
#include "bsp/bsp.h"

#include "wyres-generic/wutils.h"
#include "wyres-generic/configmgr.h"
#include "wyres-generic/wblemgr.h"
#include "cbor.h"
#include "app-core/app_core.h"
#include "app-core/app_msg.h"
#include "mod-ble/mod_ble.h"

static struct {
    void* wbleCtx;
    uint32_t beaconPeriodMS;
    uint8_t UUID[UUID_SZ];
    uint16_t major;
    uint16_t minor;
    int8_t txpower;
} _ctx;

/** callback fns from BLE generic package */
static void ble_cb(WBLE_EVENT_t e, void* d) {
    switch(e) {
        case WBLE_COMM_FAIL: {
            log_debug("MBB: comm nok");
            // We're done : tell app-core to move on
            AppCore_module_done(APP_MOD_BLE_IB);
            break;
        }
        case WBLE_COMM_OK: {
            log_debug("MBB: comm ok");
            wble_ibeacon_start(_ctx.wbleCtx, &_ctx.UUID[0], _ctx.major, _ctx.minor, 0, _ctx.beaconPeriodMS, _ctx.txpower);
            break;
        }
        case WBLE_COMM_IB_RUNNING: {
            log_debug("MBB: ib ok");
            AppCore_module_done(APP_MOD_BLE_IB);
            break;
        }
        default: {
            log_debug("MBB cb %d", e);
            break;         
        }   
    }
}

// My api functions
static uint32_t start() {
    // ibeaconning is normally running, but redo the start each time to get any param changes.
    // Default major/minor are the low 4 bytes from the lora devEUI...
    uint8_t devEUI[8];
    memset(&devEUI[0], 0, 8);       // Ensure all 0s if no deveui available
    CFMgr_getElement(CFG_UTIL_KEY_LORA_DEVEUI, &devEUI[0], 8);

    _ctx.major = (devEUI[4] << 8) + devEUI[5];
    _ctx.minor = (devEUI[6] << 8) + devEUI[7];
    _ctx.beaconPeriodMS = 500;
    _ctx.txpower = -20;

    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_PERIOD_MS, &_ctx.beaconPeriodMS, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_MAJOR, &_ctx.major, sizeof(uint16_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_MINOR, &_ctx.minor, sizeof(uint16_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_TXPOWER, &_ctx.txpower, sizeof(int8_t));
    // NOte that if uuid in config is all 0, then the default wyres uuid is used for scanning and for ibeaconing
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_UUID, &_ctx.UUID, UUID_SZ);

    // Start BLE module if wasn't already (will call me back)
    wble_start(_ctx.wbleCtx, ble_cb);

    return 30000;       // stops when ib setup is done
}

static void stop() {
    // Just leaving it on..
//    wble_stop(_ctx.wbleCtx);
}
static void off() {

}
static void deepsleep() {

}

static bool getData(APP_CORE_UL_t* ul) {
    return false;       // nothing to see here
}

static APP_CORE_API_t _api = {
    .startCB = &start,
    .stopCB = &stop,
    .offCB = &off,
    .deepsleepCB = &deepsleep,
    .getULDataCB = &getData,    
    .ticCB = NULL,    
};
// Initialise module
void mod_ble_ibeacon_init(void) {
    // initialise access
    _ctx.wbleCtx = wble_mgr_init(MYNEWT_VAL(MOD_BLE_UART), MYNEWT_VAL(MOD_BLE_UART_BAUDRATE), MYNEWT_VAL(MOD_BLE_PWRIO), MYNEWT_VAL(MOD_BLE_UARTIO), MYNEWT_VAL(MOD_BLE_UART_SELECT));

    // hook app-core for ble access - serialised as competing for UART
    AppCore_registerModule("BLE-IB", APP_MOD_BLE_IB, &_api, EXEC_SERIAL);
//    log_debug("MBB:mod-ble-ibeacon inited");
}
