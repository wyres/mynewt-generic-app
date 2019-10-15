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

// BLE SCAN NAV : scan BLE beacons for navigation use (reflects how the scan results are treated/sent)
#include "os/os.h"
#include "bsp/bsp.h"

#include "wyres-generic/wutils.h"
#include "wyres-generic/configmgr.h"
#include "wyres-generic/wblemgr.h"
#include "cbor.h"
#include "app-core/app_core.h"
#include "app-core/app_msg.h"
#include "mod-ble/mod_ble.h"

// How many ibeacons will we deal with?
#define MAX_BLE_CURR MYNEWT_VAL(MOD_BLE_MAXIBS_NAV)
static struct {
    void* wbleCtx;
    ibeacon_data_t iblist[MAX_BLE_CURR];
} _ctx = {
    .wbleCtx=NULL,
};

/** callback fns from BLE generic package */
static void ble_cb(WBLE_EVENT_t e, ibeacon_data_t* ib) {
    switch(e) {
        case WBLE_COMM_FAIL: {
            log_debug("MBN: comm nok");
            break;
        }
        case WBLE_COMM_OK: {
            log_debug("MBN: comm ok");
            // Scan selecting only majors between 0x0000 and 0x00FF ie short range
            wble_scan_start(_ctx.wbleCtx, NULL, (BLE_TYPE_NAV<<8), ((BLE_TYPE_NAV<<8)+0xFF));
            break;
        }
        case WBLE_SCAN_RX_IB: {
//            log_debug("MBN:ib %d:%d rssi %d", ib->major, ib->minor, ib->rssi);
            // just get them all at the end
            break;
        }
        default: {
            log_debug("MBN cb %d", e);
            break;         
        }   
    }
}

// My api functions
static uint32_t start() {
    // and tell ble to go with a callback to tell me when its got something
    wble_start(_ctx.wbleCtx, ble_cb);
    // Return the scan time
    uint32_t bleScanTimeMS = 3000;
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_SCAN_TIME_MS, &bleScanTimeMS, sizeof(uint32_t));

    return bleScanTimeMS;
}

static void stop() {
    // Done BLE, go idle
    wble_scan_stop(_ctx.wbleCtx);
    // and power down 
    wble_stop(_ctx.wbleCtx);
}
static void sleep() {

}
static void deepsleep() {

}

static bool getData(APP_CORE_UL_t* ul) {
    // we have knowledge of 2 types of ibeacons
    // - 'fixed navigation' type (sparsely deployed, we shouldn't see many, only send up best rssi ones)
    // - 'mobile tag' type : may congregate in areas so we see a lot of them. In this case, we do in/out notifications
    // This module is concerned with the fixed navigation ones - we sent up a short 'best rsssi' list every time
    // Get list of ibs in order into this array please
    int nbSent = wble_getSortedIBList(_ctx.wbleCtx, MAX_BLE_CURR, _ctx.iblist);
    if (nbSent>0) {
        // put it into UL if possible
        uint8_t* vp = app_core_msg_ul_addTLgetVP(ul, APP_CORE_UL_BLE_CURR,nbSent*5);
        if (vp!=NULL) {
            for(int i=0;i<nbSent;i++) {
                *vp++ = (_ctx.iblist[i].major & 0xff);
                // no point in sending up MSB of major, not used in id
//                *vp++ = ((_ctx.iblist[i].major >> 8) & 0xff);
                *vp++ = (_ctx.iblist[i].minor & 0xff);
                *vp++ = ((_ctx.iblist[i].minor >> 8) & 0xff);
                *vp++ = _ctx.iblist[i].rssi;
                *vp++ = _ctx.iblist[i].extra;
            }
        }
    }
    log_info("MBN:UL saw %d sent best %d", wble_getNbIB(_ctx.wbleCtx), nbSent);
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
void mod_ble_scan_nav_init(void) {
    // initialise access
    _ctx.wbleCtx = wble_mgr_init(MYNEWT_VAL(MOD_BLE_UART), MYNEWT_VAL(MOD_BLE_UART_BAUDRATE), MYNEWT_VAL(MOD_BLE_PWRIO), MYNEWT_VAL(MOD_BLE_UART_SELECT));

    // hook app-core for ble scan - serialised as competing for UART
    AppCore_registerModule(APP_MOD_BLE_SCAN_NAV, &_api, EXEC_SERIAL);
//    log_debug("MB:mod-ble-scan-nav inited");
}
