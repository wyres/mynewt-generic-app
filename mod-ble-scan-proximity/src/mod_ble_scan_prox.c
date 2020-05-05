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

// BLE SCAN PROX : scan BLE beacons for proximity detection option.
// Note this module is not compatible with any other BLE using module (as it uses it 100% of the time)
#include "os/os.h"
#include "bsp/bsp.h"

#include "wyres-generic/wutils.h"
#include "wyres-generic/configmgr.h"
#include "wyres-generic/timemgr.h"
#include "wyres-generic/wblemgr.h"
#include "cbor.h"
#include "app-core/app_core.h"
#include "app-core/app_msg.h"
#include "mod-ble/mod_ble.h"

#define ENTER_UL_SZ (5)
#define EXIT_UL_SZ (3)
#define COUNT_UL_SZ (2)
#define PRESENCE_HDR_UL_SZ (2)
#define TL_HDR_UL_SZ (2)

// Max ibeacons we track in the scan history. We give ourselves some space over the defined limit to deal with the 'exit' timeouts.
#define MAX_BLE_TRACKED (MYNEWT_VAL(MOD_BLE_MAXIBS_TAG_INZONE)+10)

static struct {
    void* wbleCtx;
    uint8_t exitTimeoutMins;
    uint8_t contactSignifTimeMins;
    int8_t contactSignifRSSI;
    uint8_t maxContactsPerUL;
    ibeacon_data_t iblist[MAX_BLE_TRACKED];
    uint8_t bleErrorMask;
    uint8_t nbULRepeats;
    uint8_t uuid[UUID_SZ];
//    uint8_t cborbuf[MAX_BLE_ENTER*6];
} _ctx;

/** callback fns from BLE generic package */
static void ble_cb(WBLE_EVENT_t e, ibeacon_data_t* ib) {
    switch(e) {
        case WBLE_COMM_FAIL: {
            log_debug("MBT: comm nok");
            _ctx.bleErrorMask |= EM_BLE_COMM_FAIL;
            break;
        }
        case WBLE_COMM_OK: {
            log_debug("MBT: comm ok");
            // Scan for just PROXIMITY type beacon? Future evolution : may also send up navigation beacons
            // Note that request for scan should not impact ibeaconning (v2 BLE can do both in parallel)
            wble_scan_start(_ctx.wbleCtx, _ctx.uuid, (BLE_TYPE_PROXIMITY<<8), (BLE_TYPE_PROXIMITY<<8) + 0xFF, MAX_BLE_TRACKED, &_ctx.iblist[0]);
            break;
        }
        case WBLE_SCAN_RX_IB: {
//            log_debug("MBT:ib %d:%d rssi %d", ib->major, ib->minor, ib->rssi);
            // wble mgr fills in / updates the list we gave it
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
    // When device is inactive this module is not used
    if (!AppCore_isDeviceActive()) {
        return 0;
    }
    // Read config each start() to take into account any changes
    // exit timeout should actually be in function of the delay between scans...
    CFMgr_getOrAddElementCheckRangeUINT8(CFG_UTIL_KEY_BLE_EXIT_TIMEOUT_MINS, &_ctx.exitTimeoutMins, 1, 4*60);
    CFMgr_getOrAddElementCheckRangeUINT8(CFG_UTIL_KEY_BLE_MAX_ENTER_PER_UL, &_ctx.maxContactsPerUL, 1, 255);

    // Allow these config items to be updated all the time
    CFMgr_getOrAddElementCheckRangeUINT8(CFG_UTIL_KEY_BLE_PROX_UL_REPS, &_ctx.nbULRepeats, 1, 10);
    CFMgr_getOrAddElementCheckRangeUINT8(CFG_UTIL_KEY_BLE_PROX_STIME_MINS, &_ctx.contactSignifTimeMins, 1, 60);
    CFMgr_getOrAddElementCheckRangeINT8(CFG_UTIL_KEY_BLE_PROX_SRSSI, &_ctx.contactSignifRSSI, -100, 0);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_UUID, &_ctx.uuid, UUID_SZ);

    // no errors yet
    _ctx.bleErrorMask = 0;
    // start ble to go (may already be running), with a callback to tell me when its comm is ok (may be immediate if already running)
    // Request to scan is sent once comm is ok
    wble_start(_ctx.wbleCtx, ble_cb);

    // Return the scan time (checking config is ok)
    uint32_t bleScanTimeMS = 3000;
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_BLE_SCAN_TIME_MS, &bleScanTimeMS, 1000, 60000);

    return bleScanTimeMS;
}

static void stop() {
    // Done BLE scan
    wble_scan_stop(_ctx.wbleCtx);
    // ibeaconning is normally running, but redo the start each time to get any param changes.
    // Default major/minor are the low 3 bytes from the lora devEUI...
    uint8_t devEUI[8];
    memset(&devEUI[0], 0, 8);       // Ensure all 0s if no deveui available
    CFMgr_getElement(CFG_UTIL_KEY_LORA_DEVEUI, &devEUI[0], 8);

    uint16_t major = (BLE_TYPE_PROXIMITY<<8) + devEUI[5];
    uint16_t minor = (devEUI[6] << 8) + devEUI[7];
    uint16_t interMS = 500;
    int8_t txpower = -10;
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_MAJOR, &major, 2);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_MINOR, &minor, 2);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_PERIOD_MS, &interMS, 2);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_TXPOWER, &txpower, 1);
    // NOte that if uuid in config is all 0, then the default wyres uuid is used for scanning and for ibeaconing
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_UUID, &_ctx.uuid, UUID_SZ);
    // Force major to have good high byte (or we won't detect it!)
    major = (BLE_TYPE_PROXIMITY<<8) + (major & 0xFF);
    // Again, switch to ibeaconning ok directly from scanning
    wble_ibeacon_start(_ctx.wbleCtx, _ctx.uuid, major, minor, 0, interMS, txpower);
}

static void off() {
    // nothing to do
}
static void deepsleep() {
    // nothing to do
}

static bool getData(APP_CORE_UL_t* ul) {
        // When device is inactive this module is not used
    if (!AppCore_isDeviceActive()) {
        return false;
    }

    int nbContact=0;

    uint32_t now = TMMgr_getRelTimeSecs();
    
    // Check if table is full.
    int nActive = wble_getNbIBActive(_ctx.wbleCtx,0);
    log_debug("MBP: proc %d active BLE", nActive);
    if (nActive==MAX_BLE_TRACKED) {
        _ctx.bleErrorMask |= EM_BLE_TABLE_FULL;        
    }
    // for each one in the list (now updated), check its type, flagging new ones, doing the count, etc
    for(int i=0;i<MAX_BLE_TRACKED;i++) {
        if (_ctx.iblist[i].lastSeenAt>0) {      // its a valid entry
            uint8_t bletype = (_ctx.iblist[i].major & 0xff00) >> 8;
            if (bletype==BLE_TYPE_PROXIMITY) {
                // is he timed out (exited)? 
                if ((now-_ctx.iblist[i].lastSeenAt)>(_ctx.exitTimeoutMins*60)) {
                    // Yes, he's not present (and we'll 'delete' him by resetting his lastSeenAt)
                    _ctx.iblist[i].lastSeenAt=0;
                } else {
                    // Been seen for long enough to count as a contact (and not yet sent?)
                    if (_ctx.iblist[i].new && ((now-_ctx.iblist[i].firstSeenAt) > (_ctx.contactSignifTimeMins*60))) {
                        nbContact++;
                    }
                }
            } else {
                // ignore, shouldn't happen as the scanner was told to ignore these guys
                log_warn("MBP:remove unex type=%d", bletype);
                _ctx.bleErrorMask |= EM_BLE_RX_BADMAJ;
                // Free up the space
                _ctx.iblist[i].lastSeenAt=0;
            }
        }
    }
    // Limit numbers in the UL to configured maxes
    if (nbContact>_ctx.maxContactsPerUL) {
        nbContact = _ctx.maxContactsPerUL;
    }

    // put up to max enter elemnents into UL.
    if (nbContact>0) {
        int nbAdded = 0;
        uint8_t* vp = NULL;
        int nbThisUL = 0;
        for(int i=0;i<MAX_BLE_TRACKED && nbAdded<nbContact; i++) {
            // If entry is valid, and of type enter/exit, and is new, then...
            if ((_ctx.iblist[i].lastSeenAt>0) 
                    && (((_ctx.iblist[i].major & 0xFF00) >> 8) == BLE_TYPE_PROXIMITY)
                    && _ctx.iblist[i].new) {
                if (nbThisUL <= 0) {
                    // Find space in UL
                    int bytesInUL = app_core_msg_ul_remainingSz(ul);
                    // Check if space for TL and 1 ble at least
                    if (bytesInUL < (TL_HDR_UL_SZ + ENTER_UL_SZ)) {
                        // move to next message and get size (0=no next!)
                        if ((bytesInUL = app_core_msg_ul_requestNextUL(ul)) <= 0) {
                            // no more messages, sorry
                            log_debug("MBN: unexpected no next UL still got enter %d",(nbContact-nbAdded));
                            _ctx.bleErrorMask |= EM_UL_NONEXTUL;
                            break;      // from for, we're done here
                        }
                    }
                    nbThisUL = (bytesInUL-TL_HDR_UL_SZ) / ENTER_UL_SZ; 
                    if (nbThisUL > (nbContact-nbAdded)) {
                        // should always give a >0 answer as nbAdded is never >= nbEnterToAdd here
                        nbThisUL = (nbContact-nbAdded);
                        assert(nbThisUL>0);
                    }
                    vp = app_core_msg_ul_addTLgetVP(ul, APP_CORE_UL_BLE_ENTER, nbThisUL*ENTER_UL_SZ);
                }
                if (vp!=NULL) {
                    // add maj/min to UL (number of bytes == ENTER_UL_SZ)
                    *vp++ = (_ctx.iblist[i].major & 0xFF);        // Just LSB of major
                    *vp++ = (_ctx.iblist[i].minor & 0xff);
                    *vp++ = ((_ctx.iblist[i].minor >> 8) & 0xff);
                    *vp++ = _ctx.iblist[i].rssi;
                    *vp++ = _ctx.iblist[i].extra;
                    // we want to tell backend at least twice per contact
                    _ctx.iblist[i].newULCnt++;  
                    if (_ctx.iblist[i].newULCnt > _ctx.nbULRepeats) {
                        _ctx.iblist[i].new = false;     // we've told the backend several times!
                    }
                    nbAdded++;
                    nbThisUL--;
                    if (nbAdded>=nbContact) {
                        break;      // added all that we're allowed
                    }

                } else {
                    // this should not happen if the previous calculations were correct...
                    log_debug("MBN: no space in UL for contacts %d",nbThisUL);
                    _ctx.bleErrorMask |= EM_UL_NOSPACE;
                    break;
                }
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
    // If error like tracking list is full and we failed to see a enter/exit guy, flag it up...
    if (_ctx.bleErrorMask!=0) {
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_BLE_ERRORMASK, 1, &_ctx.bleErrorMask);
    }
    log_info("MBT:UL contact %d err %02x", 
        nbContact, _ctx.bleErrorMask);
    return (nbContact>0);
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
void mod_ble_scan_prox_init(void) {
    // _ctx in bss -> set to 0 by default
    // Set non-0 init values (default before config read)
    _ctx.exitTimeoutMins=5;
    _ctx.maxContactsPerUL=50;
    _ctx.nbULRepeats = 2;
    _ctx.contactSignifTimeMins = MYNEWT_VAL(MOD_BLE_PROX_SIGNIF_CONTACT);
    _ctx.contactSignifRSSI = MYNEWT_VAL(MOD_BLE_PROX_SIGNIF_RSSI);
    // initialise access (this is resistant to multiple calls...)
    _ctx.wbleCtx = wble_mgr_init(MYNEWT_VAL(MOD_BLE_UART), MYNEWT_VAL(MOD_BLE_UART_BAUDRATE), MYNEWT_VAL(MOD_BLE_PWRIO), MYNEWT_VAL(MOD_BLE_UART_SELECT));

    // hook app-core for ble scan - serialised as competing for UART. Note we claim we're an ibeaon module
    AppCore_registerModule("BLE-SCAN-PROX", APP_MOD_BLE_IB, &_api, EXEC_SERIAL);
//    log_debug("MB:mod-ble-scan-prox inited");
}
