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

// BLE SCAN NAV : scan BLE beacons for asset tag use (reflects how the scan results are treated/sent)
// Note normally a device has either this module or the scan-nav one enabled, but rarely both...
#include "os/os.h"
#include "bsp/bsp.h"

#include "wyres-generic/wutils.h"
#include "wyres-generic/configmgr.h"
#include "wyres-generic/timeMgr.h"
#include "wyres-generic/wblemgr.h"
#include "cbor.h"
#include "app-core/app_core.h"
#include "app-core/app_msg.h"
#include "mod-ble/mod_ble.h"

#define MAX_BLE_ENTER MYNEWT_VAL(MOD_BLE_MAXIBS_TAG_ENTER)
#define MAX_BLE_EXIT MYNEWT_VAL(MOD_BLE_MAXIBS_TAG_EXIT)
#define MAX_BLE_TRACKED MYNEWT_VAL(MOD_BLE_MAXIBS_TAG_INZONE)

#define BLE_NTYPES ((BLE_TYPE_COUNTABLE_END-BLE_TYPE_COUNTABLE_START)+1)
// don't want these on the stack, and trying to avoid malloc
static struct {
    void* wbleCtx;
    struct {
        ibeacon_data_t ib;
        uint32_t lastSeenAt;
        bool new;
    } iblist[MAX_BLE_TRACKED];
    uint8_t tcount[BLE_NTYPES];
//    uint8_t cborbuf[MAX_BLE_ENTER*6];
} _ctx = {
    .wbleCtx=NULL,
};

static int findIB(uint16_t maj, uint16_t min) {
    for(int i=0;i<MAX_BLE_TRACKED;i++) {
        if ((_ctx.iblist[i].lastSeenAt>0) && _ctx.iblist[i].ib.major==maj && _ctx.iblist[i].ib.minor==min) {
            return i;
        }
    }
    return -1;
}
static int findEmptyIB() {
    for(int i=0;i<MAX_BLE_TRACKED;i++) {
        if (_ctx.iblist[i].lastSeenAt==0) {
            return i;
        }
    }
    return -1;
}

/** callback fns from BLE generic package */
static void ble_cb(WBLE_EVENT_t e, ibeacon_data_t* ib) {
    switch(e) {
        case WBLE_COMM_FAIL: {
            log_debug("MBT: comm nok");
            break;
        }
        case WBLE_COMM_OK: {
            log_debug("MBT: comm ok");
            // Scan for both countable and enter/exit types. Note calculation depends on the BLE_TYPExXX values...
            wble_scan_start(_ctx.wbleCtx, NULL, (BLE_TYPE_COUNTABLE_START<<8), (BLE_TYPE_ENTEREXIT<<8) + 0xFF);
            break;
        }
        case WBLE_SCAN_RX_IB: {
//            log_debug("MBT:ib %d:%d rssi %d", ib->major, ib->minor, ib->rssi);
            // just get them all at the end
            break;
        }
        default: {
            log_debug("MBT cb %d", e);
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
    // - short range 'fixed navigation' type (sparsely deployed, we shouldn't see many, only send up best rssi ones)
    //      - major=0x00xx
    // - long range 'mobile tag' type : may congregate in areas so we see a lot of them.
    // Two sub cases : 
    //      'count only' : major = 0x01xx - 0x7Fxx
    //      'enter/exit' : major = 0x80xx
    // This module deals with the long range types
    // get handle to the full list
    int nb = 0;
    int nbEnter=0;
    int nbExit=0;
    int nbCount=0;
    // No countables seen
    memset(_ctx.tcount, 0, BLE_NTYPES);

    uint32_t now = TMMgr_getTime();
    ibeacon_data_t* iblist = wble_getIBList(_ctx.wbleCtx, &nb);
    // for each one in the new scan list, update the current list, flagging new ones
    for(int i=0;i<nb;i++) {
        uint8_t bletype = (iblist[i].major & 0xff00) >> 8;
        if (bletype==BLE_TYPE_NAV) {
            // ignore, shouldn't happen as the scanner was told to ignore these guys
        } else if (bletype==BLE_TYPE_ENTEREXIT) {
            // exit/enter long range type : if new, put as enter in the outgoing message, if not seen for last Xs, put in the exit list
            int idx = findIB(iblist[i].major, iblist[i].minor);
            if (idx<0) {
                // insert
                idx = findEmptyIB();
                if (idx<0) {
                    // poo
                    log_debug("MBT: no space to add new tag");
                } else {
                    _ctx.iblist[idx].lastSeenAt = now;
                    _ctx.iblist[idx].ib.major = iblist[i].major;
                    _ctx.iblist[idx].ib.minor = iblist[i].minor;
                    _ctx.iblist[idx].ib.rssi = iblist[i].rssi;
                    _ctx.iblist[idx].ib.extra = iblist[i].extra;
                    _ctx.iblist[idx].new = true;        // for UL
                }
            } else {
                // update
                _ctx.iblist[idx].lastSeenAt = now;
                _ctx.iblist[idx].ib.rssi = iblist[i].rssi;
                _ctx.iblist[idx].ib.extra = iblist[i].extra;
            }
        } else if (bletype>=BLE_TYPE_COUNTABLE_START && bletype<=BLE_TYPE_COUNTABLE_END) {
            // countable type : just inc its counter
            int idx = (bletype - BLE_TYPE_COUNTABLE_START);
            // dont wrap the counter. 255==too many to count...
            if (_ctx.tcount[idx]<255) {
                _ctx.tcount[idx]++;
            }
            nbCount++;
        }
    }
    // find those who have not been seen for last X times, and remove them
    // Count them first to allocate space in UL
    for(int i=0;i<MAX_BLE_TRACKED;i++) {
        // lastSeenAt==0 -> unused entry
        if ((_ctx.iblist[i].lastSeenAt>0) && (now-_ctx.iblist[i].lastSeenAt)>5*60*1000) {
            nbExit++;
        }
    }
    if (nbExit>MAX_BLE_EXIT) {
        nbExit = MAX_BLE_EXIT;
    }
    if (nbExit>0) {
        uint8_t* vp = app_core_msg_ul_addTLgetVP(ul, APP_CORE_UL_BLE_EXIT, nbExit*3);
        if (vp!=NULL) {
            int nbInMessage = 0;
            for(int i=0;i<MAX_BLE_TRACKED && nbInMessage<nbExit;i++) {
                if ((_ctx.iblist[i].lastSeenAt>0) && (now-_ctx.iblist[i].lastSeenAt)>5*60*1000) {
                    // add maj/min to UL 
                    *vp++=(_ctx.iblist[i].ib.major & 0xFF);        // Just LSB of major
                    *vp++ = (_ctx.iblist[i].ib.minor & 0xff);
                    *vp++ = ((_ctx.iblist[i].ib.minor >> 8) & 0xff);
                    // delete from active list
                    _ctx.iblist[i].lastSeenAt=0;
                    nbInMessage++;
                }
            }
        }
    }
    // put up to max enter elemnents into UL.
    // Count first (must count all in case left over 'new' ones from last time that didn't fit in UL)
    for(int i=0;i<MAX_BLE_TRACKED;i++) {
        if (_ctx.iblist[i].new) {
            nbEnter++;
        }
    }

    if (nbEnter>MAX_BLE_ENTER) {
        nbEnter = MAX_BLE_ENTER;
    }
    if (nbEnter>0) {
        uint8_t* vp = app_core_msg_ul_addTLgetVP(ul, APP_CORE_UL_BLE_ENTER, nbEnter*5);
        if (vp!=NULL) {
            int nbInMessage = 0;
            for(int i=0;i<MAX_BLE_TRACKED && nbInMessage<nbEnter;i++) {
                if (_ctx.iblist[i].new) {
                    // add maj/min to UL 
                    *vp++=(_ctx.iblist[i].ib.major & 0xFF);        // Just LSB of major
                    *vp++ = (_ctx.iblist[i].ib.minor & 0xff);
                    *vp++ = ((_ctx.iblist[i].ib.minor >> 8) & 0xff);
                    *vp++ = _ctx.iblist[i].ib.rssi;
                    *vp++ = _ctx.iblist[i].ib.extra;
                    _ctx.iblist[i].new = false;
                    nbInMessage++;
                }
            }
        }
    }
    // put in types and counts
    for(int i=0;i<BLE_NTYPES;i++) {
        if (_ctx.tcount[i]>0) {
            uint8_t* vp = app_core_msg_ul_addTLgetVP(ul, APP_CORE_UL_BLE_COUNT, 2);
            if (vp!=NULL) {
                *vp++=(BLE_TYPE_COUNTABLE_START+i);
                *vp++=_ctx.tcount[i];
                log_debug("MBT: countable tags type %d saw %d", BLE_TYPE_COUNTABLE_START+i, _ctx.tcount[i]);
            } else {
                log_debug("MBT: no room in UL for countable tags type %d", BLE_TYPE_COUNTABLE_START+i);
            }
        }
    }

/*    if (nbSent>0) {
        // Build CBOR array block first then add to message (as we don't know its size)
        CborEncoder encoder, blearray;
        cbor_encoder_init(&encoder, _cborbuf, MAX_BLE_CURR*6, 0);
        // its an array of int, in order maj/min delta, rssi/2+23, extra
        cbor_encoder_create_array(&encoder, &blearray, nbSent);
        uint32_t prevMajMin = 0;
        for(int i=0;i<nbSent;i++) {
            uint32_t majMin = (_iblist[i].major << 16) + _iblist[i].minor;
            cbor_encode_int(&blearray, (majMin - prevMajMin));
            prevMajMin = majMin;
            cbor_encode_int(&blearray, (_iblist[i].rssi/2)+23);
            cbor_encode_int(&blearray, _iblist[i].extra);
//            *vp++ = (iblist[i].major & 0xff);
//            *vp++ = ((iblist[i].major >> 8) & 0xff);
//            *vp++ = (iblist[i].minor & 0xff);
//            *vp++ = ((iblist[i].minor >> 8) & 0xff);
//            *vp++ = iblist[i].rssi;
//            *vp++ = iblist[i].extra;
        }
        cbor_encoder_close_container(&encoder, &blearray);
        // how big?
        size_t len = cbor_encoder_get_buffer_size(&encoder, _cborbuf);
        log_debug("MB: cbor len %d instead of %d", len, nbSent*6);
        // put it into UL if possible
        uint8_t* vp = app_core_msg_ul_addTLgetVP(ul, APP_CORE_BLE_CURR,len);
        if (vp!=NULL) {
            memcpy(vp, _cborbuf, len);
        }
    }
*/
    log_info("MBT:UL enter %d exit %d count %d", nbEnter, nbExit, nbCount);
    return (nbEnter>0 || nbExit>0 || nbCount>0);
}

static APP_CORE_API_t _api = {
    .startCB = &start,
    .stopCB = &stop,
    .sleepCB = &sleep,
    .deepsleepCB = &deepsleep,
    .getULDataCB = &getData,    
};
// Initialise module
void mod_ble_scan_tag_init(void) {
    // initialise access (this is resistant to multiple calls...)
    _ctx.wbleCtx = wble_mgr_init(MYNEWT_VAL(MOD_BLE_UART), MYNEWT_VAL(MOD_BLE_PWRIO), MYNEWT_VAL(MOD_BLE_UART_SELECT));

    // hook app-core for ble scan - serialised as competing for UART
    AppCore_registerModule(APP_MOD_BLE_SCAN_TAGS, &_api, EXEC_SERIAL);
//    log_debug("MB:mod-ble-scan-nav inited");
}
