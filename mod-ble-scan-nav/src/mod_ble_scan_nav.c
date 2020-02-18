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
// Keep history between scans of this number
#define MAX_BLE_TOSCAN  (16)     
// Max number we send up (the 'best' rssi ones)
#define MAX_BLE_TOSEND MYNEWT_VAL(MOD_BLE_MAXIBS_NAV)
// how long till we remove them out of history? For navigation, we keep no history between scans generally, as backend deals with history
#define MAX_BEACON_TIMEOUT_SECS (60)


static struct {
    void* wbleCtx;
    uint8_t maxNavPerUL;
    uint8_t bleErrorMask;
    ibeacon_data_t iblist[MAX_BLE_TOSCAN];
    ibeacon_data_t bestiblist[MAX_BLE_TOSEND];
    uint8_t uuid[UUID_SZ];
} _ctx;     // inited to 0 by definition

/** callback fns from BLE generic package */
static void ble_cb(WBLE_EVENT_t e, ibeacon_data_t* ib) {
    switch(e) {
        case WBLE_COMM_FAIL: {
            log_debug("MBN: comm nok");
            _ctx.bleErrorMask |= EM_BLE_COMM_FAIL;
            break;
        }
        case WBLE_COMM_OK: {
            log_debug("MBN: comm ok");
            // Scan selecting only majors between 0x0000 and 0x00FF ie short range
            wble_scan_start(_ctx.wbleCtx, _ctx.uuid, (BLE_TYPE_NAV<<8), ((BLE_TYPE_NAV<<8)+0xFF), MAX_BLE_TOSCAN, &_ctx.iblist[0]);
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
    // Get max BLEs, validate value is ok to avoid issues...
    CFMgr_getOrAddElementCheckRangeUINT8(CFG_UTIL_KEY_BLE_MAX_NAV_PER_UL, &_ctx.maxNavPerUL, 1, MAX_BLE_TOSEND);

    // no errors yet
    _ctx.bleErrorMask = 0;

    // and tell ble to go with a callback to tell me when its got something
    wble_start(_ctx.wbleCtx, ble_cb);
    // Return the scan time
    uint32_t bleScanTimeMS = 3000;
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_BLE_SCAN_TIME_MS, &bleScanTimeMS, 1000, 60000);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_UUID, &_ctx.uuid, UUID_SZ);

    return bleScanTimeMS;
}

static void stop() {
    // Done BLE, go idle
    wble_scan_stop(_ctx.wbleCtx);
    // only keep ones we saw this scan
    wble_resetList(_ctx.wbleCtx, MAX_BEACON_TIMEOUT_SECS);
    // and power down 
    wble_stop(_ctx.wbleCtx);
}
static void off() {

}
static void deepsleep() {

}

static bool getData(APP_CORE_UL_t* ul) {
    // we have knowledge of 2 types of ibeacons
    // - 'fixed navigation' type (sparsely deployed, we shouldn't see many, only send up best rssi ones)
    // - 'mobile tag' type : may congregate in areas so we see a lot of them. In this case, we do in/out notifications
        // Check if table is full.
    int nActive = wble_getNbIBActive(_ctx.wbleCtx,0);
    log_debug("MBN: proc %d active BLE", nActive);
    if (nActive==MAX_BLE_TOSCAN) {
        _ctx.bleErrorMask |= EM_BLE_TABLE_FULL;        
    }

    // This module is concerned with the fixed navigation ones - we sent up a short 'best rsssi' list every time
    // Get list of ibs in order into this array please
    int nbSent = wble_getSortedIBList(_ctx.wbleCtx, MAX_BLE_TOSEND, _ctx.bestiblist);
    if (nbSent>0) {
        if (nbSent>_ctx.maxNavPerUL) {
            nbSent = _ctx.maxNavPerUL;      // can limit to less than the max
        }
        // put it into UL if possible
        uint8_t* vp = app_core_msg_ul_addTLgetVP(ul, APP_CORE_UL_BLE_CURR,nbSent*5);
        if (vp!=NULL) {
            for(int i=0;i<nbSent;i++) {
                *vp++ = (_ctx.bestiblist[i].major & 0xff);
                // no point in sending up MSB of major, not used in id
//                *vp++ = ((_ctx.bestiblist[i].major >> 8) & 0xff);
                *vp++ = (_ctx.bestiblist[i].minor & 0xff);
                *vp++ = ((_ctx.bestiblist[i].minor >> 8) & 0xff);
                *vp++ = _ctx.bestiblist[i].rssi;
                *vp++ = _ctx.bestiblist[i].extra;
            }
        }
        // Set a global flag so gps knows we saw 'indoor' type localisation stuff
        // TODO
    } else {
        // add empty TLV to signal we scanned but didnt see them
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_BLE_CURR, 0, NULL);
    }
        // If error like tracking list is full and we failed to see a enter/exit guy, flag it up...
    if (_ctx.bleErrorMask!=0) {
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_BLE_ERRORMASK, 1, &_ctx.bleErrorMask);
    }

    log_info("MBN:UL saw %d sent best %d err %02x", wble_getNbIBActive(_ctx.wbleCtx, 0), nbSent, _ctx.bleErrorMask);
//    return (nbSent>0);
    return true;        // always gotta send UL as 'no BLEs seen' is also important!
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
void mod_ble_scan_nav_init(void) {
    // _ctx initied to 0 by definition (bss). Set any non-0 defaults here
    _ctx.maxNavPerUL = MAX_BLE_TOSEND;

    // initialise access
    _ctx.wbleCtx = wble_mgr_init(MYNEWT_VAL(MOD_BLE_UART), MYNEWT_VAL(MOD_BLE_UART_BAUDRATE), MYNEWT_VAL(MOD_BLE_PWRIO), MYNEWT_VAL(MOD_BLE_UART_SELECT));

    // hook app-core for ble scan - serialised as competing for UART
    AppCore_registerModule("BLE-SCAN-NAV", APP_MOD_BLE_SCAN_NAV, &_api, EXEC_SERIAL);
//    log_debug("MB:mod-ble-scan-nav inited");
}
