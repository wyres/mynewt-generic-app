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

// Define this to get devaddress as remote contact id, rather than major/minor
//#define SEND_DEVADDR    1

#ifdef SEND_DEVADDR
#define PROX_ENTER_UL_SZ (8)
#define PROX_EXIT_UL_SZ (7)
#define PROX_ENTER_TAG (APP_CORE_UL_BLE_PROX_ENTER)
#define PROX_EXIT_TAG (APP_CORE_UL_BLE_PROX_EXIT)
#else 
#define PROX_ENTER_UL_SZ (5)
#define PROX_EXIT_UL_SZ (4)
#define PROX_ENTER_TAG (APP_CORE_UL_BLE_ENTER)
#define PROX_EXIT_TAG (APP_CORE_UL_BLE_EXIT)
#endif

#define COUNT_UL_SZ (2)
#define TL_HDR_UL_SZ (2)

// Max ibeacons we track in the scan history. We give ourselves some space over the defined limit to deal with the 'exit' timeouts.
#define MAX_BLE_TRACKED (MYNEWT_VAL(MOD_BLE_MAXIBS_TAG_INZONE)+10)
// Max ibeacons we sent up of navigation type (MSB major = 0x00)
#define MAX_NAV (5)

static struct {
    void* wbleCtx;
    uint8_t exitTimeoutMins;
    uint8_t contactSignifTimeMins;
    int8_t contactSignifRSSI;
    uint8_t maxContactsPerUL;
    ibeacon_data_t iblist[MAX_BLE_TRACKED];
    ibeacon_data_t navIBList[MAX_NAV];      // list of 'best' navigation beacons currently
    uint8_t nbNav;
    uint8_t bleErrorMask;
    uint8_t nbULRepeats;
    uint8_t uuid[UUID_SZ];
//    uint8_t cborbuf[MAX_BLE_ENTER*6];
} _ctx;

/** callback fns from BLE generic package */
static void ble_cb(WBLE_EVENT_t e, void* d) {
    switch(e) {
        case WBLE_COMM_FAIL: {
            log_debug("MBP: comm nok");
            _ctx.bleErrorMask |= EM_BLE_COMM_FAIL;
            break;
        }
        case WBLE_COMM_OK: {
            log_debug("MBP: comm ok");
            // Scan for both PROXIMITY and navigation beacons (sadly this means we get all the guys in between too but life...)
            // Note that request for scan should not impact ibeaconning (v2 BLE can do both in parallel)
            wble_scan_start(_ctx.wbleCtx, _ctx.uuid, (BLE_TYPE_NAV<<8), (BLE_TYPE_PROXIMITY<<8) + 0xFF, MAX_BLE_TRACKED, &_ctx.iblist[0]);
            break;
        }
        case WBLE_SCAN_RX_IB: {
            //ibeacon_data_t* ib = (ibeacon_data_t*)d;
//            log_debug("MBT:ib %d:%d rssi %d", ib->major, ib->minor, ib->rssi);
            // wble mgr fills in / updates the list we gave it
            break;
        }
        default: {
            log_debug("MBP cb %d", e);
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
    int8_t txpower = -20;
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

    int nbContactCurrent=0;     // how many 'proximity' type guys currently near me
    int nbContactNew=0;         // How many are 'new' contacts (ie > X mins of being there)
    int nbContactEnd=0;         // how many that were there are no longer there?

    uint32_t now = TMMgr_getRelTimeSecs();
    
    // nav beacon list is emptied before processing
    _ctx.nbNav = 0;

    // Check if table is full.
    int nActive = wble_getNbIBActive(_ctx.wbleCtx,0);
    log_debug("MBP: %d BLE", nActive);
    if (nActive==MAX_BLE_TRACKED) {
        _ctx.bleErrorMask |= EM_BLE_TABLE_FULL;        
    }
    // for each one in the list (now updated), check its type, flagging new ones, doing the count, etc
    for(int i=0;i<MAX_BLE_TRACKED;i++) {
        if (_ctx.iblist[i].lastSeenAt>0) {      // its a valid entry
            uint8_t bletype = (_ctx.iblist[i].major & 0xff00) >> 8;
            if (bletype==BLE_TYPE_PROXIMITY) {
                nbContactCurrent++;     // count how many are around me
                if (_ctx.iblist[i].new && ((now-_ctx.iblist[i].firstSeenAt) > (_ctx.contactSignifTimeMins*60))) {
                    // Been seen for long enough to count as a contact (and not yet sent?)
                    nbContactNew++;
                } else if ((now-_ctx.iblist[i].lastSeenAt)>(_ctx.exitTimeoutMins*60)) {
                    // is he timed out (exited)? [note only check once his 'newness' has been sent to backend]
                    // was he a proper 'contact' ie was present for the minimum time? (and hence notified)
                    if ((now-_ctx.iblist[i].firstSeenAt) > (_ctx.contactSignifTimeMins*60)) {
                        // Yes, and now he's not present (and we'll 'delete' him by resetting his lastSeenAt once sent up)
                        nbContactEnd++;     // processing is done once he's been in UL
                    } else {
                        // no, and now he's gone, so can just remove him from the list (don't tell about 'exit' of non-contacts)
                        _ctx.iblist[i].lastSeenAt = 0;
                    }
                } else {
                    // If the RSSI is 'too low' then delete from list
                    // TODO : do we mean any individual rx is too low, or the mean rssi is too low, or all the rx rssis are too low??
                }
            } else if (bletype==BLE_TYPE_NAV) {
                // fine gonna pick the best 3
                // if not up to max size, just add to list
                if (_ctx.nbNav<MAX_NAV) {
                    // Only copy the bits we need for UL
                    _ctx.navIBList[_ctx.nbNav].major = _ctx.iblist[i].major;
                    _ctx.navIBList[_ctx.nbNav].minor = _ctx.iblist[i].minor;
                    _ctx.navIBList[_ctx.nbNav].rssi = _ctx.iblist[i].rssi;
                    _ctx.navIBList[_ctx.nbNav].extra = _ctx.iblist[i].extra;
                    _ctx.nbNav++;
                } else {
                    // find lowest in list that is lower than the one we're looking at
                    int worstRSSI = _ctx.iblist[i].rssi;
                    int worstRSSIIdx = -1;
                    for(int nvi = 0; nvi < MAX_NAV; nvi++) {
                        if (_ctx.navIBList[nvi].rssi < worstRSSI) {
                            worstRSSI = _ctx.navIBList[nvi].rssi;
                            worstRSSIIdx = nvi;
                        }
                    }
                    if (worstRSSIIdx>=0) {
                        // new guy is better than someone, overwrite him in the list
                        _ctx.navIBList[worstRSSIIdx].major = _ctx.iblist[i].major;
                        _ctx.navIBList[worstRSSIIdx].minor = _ctx.iblist[i].minor;
                        _ctx.navIBList[worstRSSIIdx].rssi = _ctx.iblist[i].rssi;
                        _ctx.navIBList[worstRSSIIdx].extra = _ctx.iblist[i].extra;
                    }
                }
                // and remove nav beacons from main table each time
                _ctx.iblist[i].lastSeenAt = 0;
            } else {
                // ignore. may happen as we scan from nav to prox majors, and this includes other types we don't care about
                log_debug("MBP:remove unex type=%d", bletype);
                // Free up the space
                _ctx.iblist[i].lastSeenAt=0;
            }
        }
    }
    if (_ctx.nbNav>0) {
        // put it into UL if possible
        uint8_t* vp = app_core_msg_ul_addTLgetVP(ul, APP_CORE_UL_BLE_CURR,_ctx.nbNav*5);
        if (vp!=NULL) {
            for(int i=0;i<_ctx.nbNav;i++) {
                *vp++ = (_ctx.navIBList[i].major & 0xff);
                // no point in sending up MSB of major, not used in id
//                *vp++ = ((_ctx.bestiblist[i].major >> 8) & 0xff);
                *vp++ = (_ctx.navIBList[i].minor & 0xff);
                *vp++ = ((_ctx.navIBList[i].minor >> 8) & 0xff);
                *vp++ = _ctx.navIBList[i].rssi;
                *vp++ = _ctx.navIBList[i].extra;
            }
        }
    } else {
        // add empty TLV to signal we scanned but didnt see them
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_BLE_CURR, 0, NULL);
    }

        // tell backend just how many people are around me right now
    uint8_t ctb[2];
    ctb[0] = (BLE_TYPE_PROXIMITY);
    ctb[1] = nbContactCurrent;
    if (app_core_msg_ul_addTLV(ul, APP_CORE_UL_BLE_COUNT, 2, &ctb[0])) {
        log_debug("MBP: prox %d", nbContactCurrent);
    } else {
        // this should not happen if the previous calculations were correct...
        log_debug("MBP: no space in UL for prox count %d",nbContactCurrent);
        _ctx.bleErrorMask |= EM_UL_NOSPACE;
    }

    // Limit numbers in the UL to configured maxes for the lists
    if (nbContactNew>_ctx.maxContactsPerUL) {
        nbContactNew = _ctx.maxContactsPerUL;
    }
    if (nbContactEnd>_ctx.maxContactsPerUL) {
        nbContactEnd = _ctx.maxContactsPerUL;
    }

    // put up to max enter elemnents into UL.
    if (nbContactNew>0) {
        int nbAdded = 0;
        uint8_t* vp = NULL;
        int nbThisUL = 0;
        for(int i=0;i<MAX_BLE_TRACKED && nbAdded<nbContactNew; i++) {
            // If entry is valid, and of type enter/exit, and is new, then...
            if ((_ctx.iblist[i].lastSeenAt>0) 
                    && (((_ctx.iblist[i].major & 0xFF00) >> 8) == BLE_TYPE_PROXIMITY)
                    && _ctx.iblist[i].new) {
                if (nbThisUL <= 0) {
                    // Find space in UL
                    int bytesInUL = app_core_msg_ul_remainingSz(ul);
                    // Check if space for TL and 1 ble at least
                    if (bytesInUL < (TL_HDR_UL_SZ + PROX_ENTER_UL_SZ)) {
                        // move to next message and get size (0=no next!)
                        if ((bytesInUL = app_core_msg_ul_requestNextUL(ul)) <= 0) {
                            // no more messages, sorry
                            log_debug("MBN: unexpected no next UL still got enter %d",(nbContactNew-nbAdded));
                            _ctx.bleErrorMask |= EM_UL_NONEXTUL;
                            break;      // from for, we're done here
                        }
                    }
                    nbThisUL = (bytesInUL-TL_HDR_UL_SZ) / PROX_ENTER_UL_SZ; 
                    if (nbThisUL > (nbContactNew-nbAdded)) {
                        // should always give a >0 answer as nbAdded is never >= nbContactNew here
                        nbThisUL = (nbContactNew-nbAdded);
                        assert(nbThisUL>0);
                    }
                    vp = app_core_msg_ul_addTLgetVP(ul, PROX_ENTER_TAG, nbThisUL*PROX_ENTER_UL_SZ);
                }
                if (vp!=NULL) {
#ifdef SEND_DEVADDR
                    int seenSinceMins = ((now - _ctx.iblist[i].firstSeenAt) / 60);
                    // new format with devAddr/timeSinceEntered/RSSI 
                    memcpy(vp, &_ctx.iblist[i].devaddr[0], DEVADDR_SZ);
                    vp+=DEVADDR_SZ;
                    *vp++ = _ctx.iblist[i].rssi;
                    *vp++ = (seenSinceMins<255 ? seenSinceMins : 255);      // Total time seen in minutes, max'd at 255
#else
                    // add maj/min to UL (number of bytes == ENTER_UL_SZ)
                    *vp++ = (_ctx.iblist[i].major & 0xFF);        // Just LSB of major
                    *vp++ = (_ctx.iblist[i].minor & 0xff);
                    *vp++ = ((_ctx.iblist[i].minor >> 8) & 0xff);
                    *vp++ = _ctx.iblist[i].rssi;
                    *vp++ = _ctx.iblist[i].extra;
#endif
                    
                    // we want to tell backend at least twice per contact
                    _ctx.iblist[i].inULCnt++;  
                    if (_ctx.iblist[i].inULCnt > _ctx.nbULRepeats) {
                        _ctx.iblist[i].new = false;     // we've told the backend several times!
                        _ctx.iblist[i].inULCnt = 0;     // ready for reuse
                    }
                    nbAdded++;
                    nbThisUL--;
                    if (nbAdded>=nbContactNew) {
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
    if (nbContactEnd>0) {
        int nbAdded = 0;
        uint8_t* vp = NULL;
        int nbThisUL = 0;
        for(int i=0;i<MAX_BLE_TRACKED && nbAdded<nbContactEnd; i++) {
            // If a valid entry, and of proximity ble type, and has timed out...
            if ((_ctx.iblist[i].lastSeenAt>0) 
                    && (((_ctx.iblist[i].major & 0xFF00) >> 8) == BLE_TYPE_PROXIMITY) 
                    && ((now-_ctx.iblist[i].lastSeenAt)>(_ctx.exitTimeoutMins*60))) {
                if (nbThisUL <= 0) {
                    // Find space in UL
                    int bytesInUL = app_core_msg_ul_remainingSz(ul);
                    // Check if space for TL and 1 ble at least
                    if (bytesInUL < (TL_HDR_UL_SZ + PROX_EXIT_UL_SZ)) {
                        // move to next message and get size (0=no next!)
                        if ((bytesInUL = app_core_msg_ul_requestNextUL(ul)) <= 0) {
                            // no more messages, sorry
                            log_debug("MBN: unexpected no next UL still got %d",(nbContactEnd-nbAdded));
                            _ctx.bleErrorMask |= EM_UL_NONEXTUL;
                            break;      // from for, we're done here
                        }
                    }
                    nbThisUL = (bytesInUL-TL_HDR_UL_SZ) / PROX_EXIT_UL_SZ; 
                    if (nbThisUL > (nbContactEnd-nbAdded)) {
                        // should always give a >0 answer as nbAdded is never >= nbContactEnd here
                        nbThisUL = (nbContactEnd-nbAdded);
                        assert(nbThisUL>0);
                    }
                    vp = app_core_msg_ul_addTLgetVP(ul, PROX_EXIT_TAG, nbThisUL*PROX_EXIT_UL_SZ);
                }
                if (vp!=NULL) {
                    int seenSinceMins = ((now - _ctx.iblist[i].firstSeenAt) / 60);
#ifdef SEND_DEVADDR
                    // new format with devAddr/timeSinceEntered 
                    memcpy(vp, &_ctx.iblist[i].devaddr[0], DEVADDR_SZ);
                    vp+=DEVADDR_SZ;
                    *vp++ = (seenSinceMins<255 ? seenSinceMins : 255);      // Total time seen in minutes, max'd at 255
#else
                    // add maj/min to UL : must be number of bytes equal to EXIT_UL_SZ
                    *vp++ = (_ctx.iblist[i].major & 0xFF);        // Just LSB of major
                    *vp++ = (_ctx.iblist[i].minor & 0xff);
                    *vp++ = ((_ctx.iblist[i].minor >> 8) & 0xff);
                    *vp++ = (seenSinceMins<255 ? seenSinceMins : 255);      // Total time seen in minutes, max'd at 255
#endif
                    _ctx.iblist[i].inULCnt++;  
                    // TODO Problem here - intermittant reception can mean getting a 'exit' in 1 or 2 UL, but then we rx, so no longer in exit,
                    // but not new, so didn't get an enter.... backend will be confused...
                    if (_ctx.iblist[i].inULCnt > _ctx.nbULRepeats) {
                        // delete from active list
                        _ctx.iblist[i].lastSeenAt=0;
                        _ctx.iblist[i].inULCnt=0;       // reset for next time
                    }
                    log_debug("MBP: %04x:%04x exit, been in %d UL", _ctx.iblist[i].major, _ctx.iblist[i].minor, _ctx.iblist[i].inULCnt);
                    nbAdded++;
                    nbThisUL--;
                    if (nbAdded>=nbContactEnd) {
                        break;      // added all that we're allowed
                    }
                } else {
                    // this should not happen if the previous calculations were correct...
                    log_debug("MBN: unexpected no space in UL for %d",nbThisUL);
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
    log_info("MBp:UL curr %d new %d exit %d err %02x", 
        nbContactCurrent, nbContactNew, nbContactEnd, _ctx.bleErrorMask);
    return (nbContactNew>0 || nbContactEnd>0 || nbContactCurrent>0 || _ctx.bleErrorMask!=0);
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
    _ctx.exitTimeoutMins=4;
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
