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
#include "os/os.h"
#include "bsp/bsp.h"

#include "wyres-generic/wutils.h"
#include "wyres-generic/configmgr.h"
#include "wyres-generic/wblemgr.h"
#include "app-core/app_core.h"
#include "app-core/app_msg.h"

#define MAX_BLE_CURR (3)

/** callback fns from BLE generic package */
static void ble_cb(WBLE_EVENT_t e, ibeacon_data_t* ib) {
    switch(e) {
        case WBLE_COMM_FAIL: {
            log_debug("ble comm nok");
            break;
        }
        case WBLE_COMM_OK: {
            log_debug("ble comm ok");
            wble_scan_start(NULL);
            break;
        }
        case WBLE_SCAN_RX_IB: {
            log_debug("ib %d:%d rssi %d", ib->major, ib->minor, ib->rssi);
            // just get them all at the end
            break;
        }
        default: {
            log_debug("ble cb %d", e);
            break;         
        }   
    }
}

// My api functions
static uint32_t start() {
    // and tell ble to go with a callback to tell me when its got something
    wble_start(ble_cb);
    // Return the scan time
    uint32_t bleScanTime = 3;
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_SCAN_TIME_SECS, &bleScanTime, sizeof(uint32_t));

    return bleScanTime*1000;
}

static void stop() {
    wble_stop();
}
static void sleep() {

}
static void deepsleep() {

}
static bool getData(APP_CORE_UL_t* ul) {
    // Done BLE, go idle
    wble_scan_stop();
    // Get list of ibs in order into this array please
    ibeacon_data_t iblist[MAX_BLE_CURR];
//    ibeacon_data_t* iblist = wble_getIBList(&nb);
    // Note we only send the best 3 as current
    uint8_t nbSent = wble_getSortedIBList(MAX_BLE_CURR, iblist);
    if (nbSent>0) {
        // Add T+L to UL directly
        ul->payload[ul->sz++] = APP_CORE_BLE_CURR;
        ul->payload[ul->sz++] = nbSent*6;
        for(int i=0;i<nbSent;i++) {
            ul->payload[ul->sz++] = (iblist[i].major & 0xff);
            ul->payload[ul->sz++] = ((iblist[i].major >> 8) & 0xff);
            ul->payload[ul->sz++] = (iblist[i].minor & 0xff);
            ul->payload[ul->sz++] = ((iblist[i].minor >> 8) & 0xff);
            ul->payload[ul->sz++] = iblist[i].rssi;
            ul->payload[ul->sz++] = iblist[i].extra;
        }
    }
    log_debug("done with ble got %d beacons", nbSent);
    return (nbSent>0);
}

static APP_CORE_API_t _api = {
    .startCB = &start,
    .stopCB = &stop,
    .sleepCB = &sleep,
    .deepsleepCB = &deepsleep,
    .getULDataCB = &getData,    
};
// Initialise module
void mod_ble_init(void) {
    // initialise access
    wble_mgr_init(MYNEWT_VAL(MOD_BLE_UART), MYNEWT_VAL(MOD_BLE_PWRIO), MYNEWT_VAL(MOD_BLE_UART_SELECT));

    // hook app-core for ble scan - serialised as competing for UART
    AppCore_registerModule(APP_MOD_BLE_SCAN, &_api, EXEC_SERIAL);
    // for ble ibeacon
    // TODO 
//    log_debug("mod-ble inited");
}
