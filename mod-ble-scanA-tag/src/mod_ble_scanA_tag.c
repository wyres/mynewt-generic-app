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
// This code assumes that the scans are continuous (ie app-core idle time is 0)

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

#define BLE_NTYPES ((BLE_TYPE_COUNTABLE_END-BLE_TYPE_COUNTABLE_START)+1)
// don't want these on the stack, and trying to avoid malloc


static struct {
    void* wbleCtx;
    uint8_t exitTimeoutMins;
    uint8_t maxEnterPerUL;
    uint8_t maxExitPerUL;
    uint8_t presenceMinorMSB;
    ibeacon_data_t iblist[MAX_BLE_TRACKED];
    uint8_t bleErrorMask;
    uint8_t tcount[BLE_NTYPES];
    uint8_t uuid[UUID_SZ];
//    uint8_t cborbuf[MAX_BLE_ENTER*6];
} _ctx;
#if 0
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
static bool addOrUpdateList(ibeacon_data_t* ib) {
    int idx = findIB(ib->major, ib->minor);
    if (idx<0) {
        // insert
        idx = findEmptyIB();
        if (idx<0) {
            // poo
            log_debug("MBT: no space to add new tag");
            return false;
        } else {
            _ctx.iblist[idx].lastSeenAt = TMMgr_getRelTimeSecs();
            _ctx.iblist[idx].ib.major = ib->major;
            _ctx.iblist[idx].ib.minor = ib->minor;
            _ctx.iblist[idx].ib.rssi = ib->rssi;
            _ctx.iblist[idx].ib.extra = ib->extra;
            _ctx.iblist[idx].new = true;        // for UL
        }
    } else {
        // update
        _ctx.iblist[idx].lastSeenAt = TMMgr_getRelTimeSecs();
        _ctx.iblist[idx].ib.rssi = ib->rssi;
        _ctx.iblist[idx].ib.extra = ib->extra;
    }
    return true;
}
#endif
/** callback fns from BLE generic package */
static void ble_cb(WBLE_EVENT_t e, void* d) {
    switch(e) {
        case WBLE_COMM_FAIL: {
            log_debug("MBT: comm nok");
            _ctx.bleErrorMask |= EM_BLE_COMM_FAIL;
            break;
        }
        case WBLE_COMM_OK: {
            log_debug("MBT: comm ok");
            // Scan for both countable and enter/exit types. Note calculation of major range depends on the BLE_TYPExXX values being contigous...
            wble_scan_start(_ctx.wbleCtx, _ctx.uuid, (BLE_TYPE_COUNTABLE_START<<8), (BLE_TYPE_PROXIMITY<<8) + 0xFF, MAX_BLE_TRACKED, &_ctx.iblist[0]);
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
    // Read config each start() to take into account any changes
    // exit timeout should actually be in function of the delay between scans...
    CFMgr_getOrAddElementCheckRangeUINT8(CFG_UTIL_KEY_BLE_EXIT_TIMEOUT_MINS, &_ctx.exitTimeoutMins, 1, 4*60);
    CFMgr_getOrAddElementCheckRangeUINT8(CFG_UTIL_KEY_BLE_MAX_ENTER_PER_UL, &_ctx.maxEnterPerUL, 1, 255);
    CFMgr_getOrAddElementCheckRangeUINT8(CFG_UTIL_KEY_BLE_MAX_EXIT_PER_UL, &_ctx.maxExitPerUL, 1, 255);
    CFMgr_getOrAddElementCheckRangeUINT8(CFG_UTIL_KEY_BLE_PRESENCE_MINOR, &_ctx.presenceMinorMSB, 0, 255);

    // no errors yet
    _ctx.bleErrorMask = 0;

    // and tell ble to go with a callback to tell me when its got something
    wble_start(_ctx.wbleCtx, ble_cb);
    // Return the scan time (checking config is ok)
    uint32_t bleScanTimeMS = 5000;
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_BLE_SCAN_TIME_MS, &bleScanTimeMS, 1000, 60000);
    
    CFMgr_getOrAddElement(CFG_UTIL_KEY_BLE_IBEACON_UUID, &_ctx.uuid, UUID_SZ);


    return bleScanTimeMS;
}

static void stop() {
    // Done BLE, go idle
    wble_scan_stop(_ctx.wbleCtx);
}
static void off() {
    // nothing to do
}
static void deepsleep() {
    // nothing to do
}

static bool getData(APP_CORE_UL_t* ul) {
    // we have knowledge of 2 types of ibeacons
    // - short range 'fixed navigation' type (sparsely deployed, we shouldn't see many, only send up best rssi ones)
    //      - major=0x00xx
    // - long range 'mobile tag' type : may congregate in areas so we see a lot of them.
    // Three sub cases : 
    //      'count only' : major = 0x01xx - 0x7Fxx
    //      'enter/exit' : major = 0x80xx
    //      'presence' : major=0x81xx, minor = 0xZZxx where ZZ is configured for this device.
    // This module deals with the long range types
    int nbEnter=0;
    int nbExit=0;
    int nbCount=0;
        // Presence guys : this is a single TLV (we only track 1 minor block per device)
    int maxMinorIdPresence = -1;        // to work out if we see any, and if so, the max id seen (to economise space)
    uint16_t majorPresence=0;         // Normally we expect all presence guys to have same major...

    uint32_t now = TMMgr_getRelTimeSecs();
    
    // reset countables counts
    memset(&_ctx.tcount[0], 0, sizeof(_ctx.tcount));
    // Check if table is full.
    int nActive = wble_getNbIBActive(_ctx.wbleCtx,0);
    log_debug("MBT: proc %d active BLE", nActive);
    if (nActive==MAX_BLE_TRACKED) {
        _ctx.bleErrorMask |= EM_BLE_TABLE_FULL;        
    }
    // for each one seen, check its type, flagging new ones, doing the count, etc
    for(int i=0;i<MAX_BLE_TRACKED;i++) {
        if (_ctx.iblist[i].lastSeenAt>0) {      // its a valid entry
            uint8_t bletype = (_ctx.iblist[i].major & 0xff00) >> 8;
            if (bletype==BLE_TYPE_NAV) {
                // ignore, shouldn't happen as the scanner was told to ignore these guys
                log_warn("MBT:remove unex NAV type");
                _ctx.bleErrorMask |= EM_BLE_RX_BADMAJ;
                // Free up his space
                _ctx.iblist[i].lastSeenAt=0;
            } else if (bletype==BLE_TYPE_ENTEREXIT) {
                // exit/enter type : if new, we want to put in enter list in the outgoing message
                if (_ctx.iblist[i].new) {
                    nbEnter++;
                } else {
                    //  if not seen for last X minutes, we want to put in the exit list
                    if ( (now - _ctx.iblist[i].lastSeenAt)>(_ctx.exitTimeoutMins*60)) {
                        nbExit++;       // gonna need to flag up as exit
                    }
                    // Note for enter/exits we only remove them when we have managed to send their id in the UL
                }
            } else if (bletype==BLE_TYPE_PRESENCE) {
                // Presence type: we only indicate each time if we see or not the minor set we are looking for
                if (((_ctx.iblist[i].minor & 0xff00) >> 8) == _ctx.presenceMinorMSB) {
                    // is he timed out (exited)? (using same timeout as enter/exit case)
                    if ((now-_ctx.iblist[i].lastSeenAt)>(_ctx.exitTimeoutMins*60)) {
                        // Yes, he's not present (and we'll 'delete' him by resetting his lastSeenAt)
                        _ctx.iblist[i].lastSeenAt=0;
                    } else {
                        // He's present
                        uint8_t minorId = (_ctx.iblist[i].minor & 0xff);     // bit position
                        if (minorId > maxMinorIdPresence) {
                            maxMinorIdPresence = minorId;
                        }
                        if (majorPresence!=_ctx.iblist[i].major) {
                            majorPresence = _ctx.iblist[i].major;
                            // Should only happen when set first time...
                            log_debug("MBT:presence major=%d", majorPresence);
                        }
                    }
                } else  {
                    // we don't care about ones with a minor that we're not looking for - remove from our list to avoid blocking a slot
                    _ctx.iblist[i].lastSeenAt=0;
                    log_debug("MBT:remove uncon pres minor=%d", _ctx.iblist[i].minor);
                }
            } else if (bletype>=BLE_TYPE_COUNTABLE_START && bletype<=BLE_TYPE_COUNTABLE_END) {
                // Ensure remove from our list if timed out
                if ((now-_ctx.iblist[i].lastSeenAt)>(_ctx.exitTimeoutMins*60)) {
                    // Yes, he's gone so we'll 'delete' him by setting his lastSeenAt to 0
                    _ctx.iblist[i].lastSeenAt=0;
                } else {
                    // countable type : inc its counter
                    int idx = (bletype - BLE_TYPE_COUNTABLE_START);
                    if (idx>=0 && idx<=BLE_NTYPES) {
                        // dont wrap the counter. 255==too many to count...
                        if (_ctx.tcount[idx]<255) {
                            _ctx.tcount[idx]++;
                        }
                        nbCount++;
                    } // else should not happen 
                }
            } else {
                // ignore, shouldn't happen as the scanner was told to ignore these guys
                log_warn("MBT:remove unex type=%d", bletype);
                _ctx.bleErrorMask |= EM_BLE_RX_BADMAJ;
                // Free up the space
                _ctx.iblist[i].lastSeenAt=0;
            }
        }
    }
    // Limit numbers in the UL to configured maxes
    if (nbExit>_ctx.maxExitPerUL) {
        nbExit = _ctx.maxExitPerUL;
    }
    if (nbEnter>_ctx.maxEnterPerUL) {
        nbEnter = _ctx.maxEnterPerUL;
    }
    // Count number of types with non-zero counts
    int nbTypes = 0;
    for(int i=0;(i<BLE_NTYPES);i++) {
        if (_ctx.tcount[i]>0) {
            nbTypes++;
        }
    }

    // Adjust numbers to divide up remaining UL space 'fairly' between enter/exit/types
    // how much space would it take (assuming spread over 4 UL packets)
    int bytesRequired = nbEnter*ENTER_UL_SZ + nbExit*EXIT_UL_SZ + nbTypes*COUNT_UL_SZ + TL_HDR_UL_SZ*6;
    int bytesAvailable = app_core_msg_ul_getTotalSpaceAvailable(ul);
    // Assume splitting space evenly ie 1/3 each so everyone has same reduction %age if required
    int percentReduc = (bytesAvailable>bytesRequired) ? 100 : (bytesAvailable*100 / bytesRequired);
    int nbEnterToAdd = (nbEnter * percentReduc) / 100;
    int nbExitToAdd = (nbExit * percentReduc) / 100;
    int nbTypesToAdd = (nbTypes * percentReduc) / 100;
    log_debug("MBT:br:%d ba:%d pr:%d ne:%d nea:%d",bytesRequired, bytesAvailable, percentReduc, nbEnter, nbEnterToAdd);
    // Now add the appropriate numbers of each element
    if (nbExitToAdd>0) {
        int nbAdded = 0;
        uint8_t* vp = NULL;
        int nbThisUL = 0;
        for(int i=0;i<MAX_BLE_TRACKED && nbAdded<nbExitToAdd; i++) {
            // If a valid entry, and of enter/exit ble type, and has timed out...
            if ((_ctx.iblist[i].lastSeenAt>0) 
                    && (((_ctx.iblist[i].major & 0xFF00) >> 8) == BLE_TYPE_ENTEREXIT) 
                    && (now-_ctx.iblist[i].lastSeenAt)>(_ctx.exitTimeoutMins*60)) {
                if (nbThisUL <= 0) {
                    // Find space in UL
                    int bytesInUL = app_core_msg_ul_remainingSz(ul);
                    // Check if space for TL and 1 ble at least
                    if (bytesInUL < (TL_HDR_UL_SZ + EXIT_UL_SZ)) {
                        // move to next message and get size (0=no next!)
                        if ((bytesInUL = app_core_msg_ul_requestNextUL(ul)) <= 0) {
                            // no more messages, sorry
                            log_debug("MBN: unexpected no next UL still got %d",(nbExitToAdd-nbAdded));
                            _ctx.bleErrorMask |= EM_UL_NONEXTUL;
                            break;      // from for, we're done here
                        }
                    }
                    nbThisUL = (bytesInUL-TL_HDR_UL_SZ) / EXIT_UL_SZ; 
                    if (nbThisUL > (nbExitToAdd-nbAdded)) {
                        // should always give a >0 answer as nbAdded is never >= nbExitToAdd here
                        nbThisUL = (nbExitToAdd-nbAdded);
                        assert(nbThisUL>0);
                    }
                    vp = app_core_msg_ul_addTLgetVP(ul, APP_CORE_UL_BLE_EXIT, nbThisUL*EXIT_UL_SZ);
                }
                if (vp!=NULL) {
                    // add maj/min to UL : must be number of bytes equal to EXIT_UL_SZ
                    *vp++=(_ctx.iblist[i].major & 0xFF);        // Just LSB of major
                    *vp++ = (_ctx.iblist[i].minor & 0xff);
                    *vp++ = ((_ctx.iblist[i].minor >> 8) & 0xff);
                    // delete from active list
                    _ctx.iblist[i].lastSeenAt=0;
                    nbAdded++;
                    nbThisUL--;
                    if (nbAdded>=nbExitToAdd) {
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
    // put up to max enter elemnents into UL.
    if (nbEnterToAdd>0) {
        int nbAdded = 0;
        uint8_t* vp = NULL;
        int nbThisUL = 0;
        for(int i=0;i<MAX_BLE_TRACKED && nbAdded<nbEnterToAdd; i++) {
            // If entry is valid, and of type enter/exit, and is new, then...
            if ((_ctx.iblist[i].lastSeenAt>0) 
                    && (((_ctx.iblist[i].major & 0xFF00) >> 8) == BLE_TYPE_ENTEREXIT)
                    && _ctx.iblist[i].new) {
                if (nbThisUL <= 0) {
                    // Find space in UL
                    int bytesInUL = app_core_msg_ul_remainingSz(ul);
                    // Check if space for TL and 1 ble at least
                    if (bytesInUL < (TL_HDR_UL_SZ + ENTER_UL_SZ)) {
                        // move to next message and get size (0=no next!)
                        if ((bytesInUL = app_core_msg_ul_requestNextUL(ul)) <= 0) {
                            // no more messages, sorry
                            log_debug("MBN: unexpected no next UL still got enter %d",(nbEnterToAdd-nbAdded));
                            _ctx.bleErrorMask |= EM_UL_NONEXTUL;
                            break;      // from for, we're done here
                        }
                    }
                    nbThisUL = (bytesInUL-TL_HDR_UL_SZ) / ENTER_UL_SZ; 
                    if (nbThisUL > (nbEnterToAdd-nbAdded)) {
                        // should always give a >0 answer as nbAdded is never >= nbEnterToAdd here
                        nbThisUL = (nbEnterToAdd-nbAdded);
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
                    _ctx.iblist[i].new = false;
                    nbAdded++;
                    nbThisUL--;
                    if (nbAdded>=nbEnterToAdd) {
                        break;      // added all that we're allowed
                    }

                } else {
                    // this should not happen if the previous calculations were correct...
                    log_debug("MBN: no space in UL for enter %d",nbThisUL);
                    _ctx.bleErrorMask |= EM_UL_NOSPACE;
                    break;
                }
            }
        }
    }
    // put in types and counts
    // WARNING : backend must handle case where set of type/counts split across multiple ULs - must deal with set of ULs together...
    if (nbTypesToAdd>0) {
        int nbAdded = 0;
        uint8_t* vp = NULL;
        int nbThisUL = 0;
        for(int i=0;(i<BLE_NTYPES);i++) {
            if (_ctx.tcount[i]>0) {
                if (nbThisUL <= 0) {
                    // Find space in UL
                    int bytesInUL = app_core_msg_ul_remainingSz(ul);
                    // Check if space for TL and 1 count at least
                    if (bytesInUL < (TL_HDR_UL_SZ + COUNT_UL_SZ)) {
                        // move to next message and get size (0=no next!)
                        if ((bytesInUL = app_core_msg_ul_requestNextUL(ul)) <= 0) {
                            // no more messages, sorry
                            log_debug("MBN: unexpected no next UL still got type %d",(nbTypesToAdd-nbAdded));
                            _ctx.bleErrorMask |= EM_UL_NONEXTUL;
                            break;      // from for, we're done here
                        }
                    }
                    nbThisUL = (bytesInUL-TL_HDR_UL_SZ) / COUNT_UL_SZ; 
                    if (nbThisUL > (nbTypesToAdd-nbAdded)) {
                        // should always give a >0 answer as nbAdded is never >= nbTypesToAdd here
                        nbThisUL = (nbTypesToAdd-nbAdded);
                        assert(nbThisUL>0);
                    }
                    vp = app_core_msg_ul_addTLgetVP(ul, APP_CORE_UL_BLE_COUNT, nbThisUL*COUNT_UL_SZ);
                }
                if (vp!=NULL) {
                    *vp++=(BLE_TYPE_COUNTABLE_START+i);
                    *vp++=_ctx.tcount[i];
                    log_debug("MBT: countable tags type %d saw %d", BLE_TYPE_COUNTABLE_START+i, _ctx.tcount[i]);
                    nbAdded++;
                    nbThisUL--;
                    if (nbAdded>=nbTypesToAdd) {
                        break;      // added all that we're allowed
                    }
                } else {
                    // this should not happen if the previous calculations were correct...
                    log_debug("MBN: no space in UL for types %d",nbThisUL);
                    _ctx.bleErrorMask |= EM_UL_NOSPACE;
                    break;
                }
            }
        }
    } else {
        // add empty TLV to signal we scanned but didnt see them
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_BLE_COUNT, 0, NULL);
    }

    // Ask for space for TLV if we see any presence guys as active
    if (maxMinorIdPresence>=0)  { 
        uint8_t* vp = app_core_msg_ul_addTLgetVP(ul, APP_CORE_UL_BLE_PRESENCE, PRESENCE_HDR_UL_SZ+((maxMinorIdPresence/8)+1));
        *vp++ = (majorPresence & 0xff);
        *vp++ = _ctx.presenceMinorMSB;
        for(int i=0;i<MAX_BLE_TRACKED;i++) {
            // Is this a valid entry, and a presence type, and for the minor range we monitor?
            if ((_ctx.iblist[i].lastSeenAt>0) &&
                (((_ctx.iblist[i].major & 0xff00) >> 8) == BLE_TYPE_PRESENCE) &&
                (((_ctx.iblist[i].minor & 0xff00) >> 8) == _ctx.presenceMinorMSB)) {
                uint8_t minorId = (_ctx.iblist[i].minor & 0xff);     // bit position
                if (minorId<=maxMinorIdPresence) {
                    // set this bit in byte array
                    vp[minorId/8] |= (1<<(minorId%8));
                } else {
                    // never happens? should be assert?
                    log_warn("MBT:pres:minorid(%d)>maxminor(%d)", minorId, maxMinorIdPresence);
                }
            }
        }
    } else {
        // add empty TLV to signal we scanned but didnt see them
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_BLE_PRESENCE, 0, NULL);
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
    log_info("MBT:UL enter %d/%d exit %d/%d types %d/%d/%d, maxPId %d err %02x", 
        nbEnter, nbEnterToAdd, nbExit, nbExitToAdd, nbCount, nbTypes, nbTypesToAdd, maxMinorIdPresence, _ctx.bleErrorMask);
//    return (nbEnterToAdd>0 || nbExitToAdd>0 || nbTypesToAdd>0 || _ctx.bleErrorMask!=0);
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
void mod_ble_scanA_tag_init(void) {
    // _ctx in bss -> set to 0 by default
    // Set non-0 init values (default before config read)
    _ctx.exitTimeoutMins=5;
    _ctx.maxEnterPerUL=50;
    _ctx.maxExitPerUL=50;
    // initialise access (this is resistant to multiple calls...)
    _ctx.wbleCtx = wble_mgr_init(MYNEWT_VAL(MOD_BLE_UART), MYNEWT_VAL(MOD_BLE_UART_BAUDRATE), MYNEWT_VAL(MOD_BLE_PWRIO), MYNEWT_VAL(MOD_BLE_UART_SELECT));

    // hook app-core for ble scan - serialised as competing for UART
    AppCore_registerModule("BLE-SCANA-TAG", APP_MOD_BLE_SCANA_TAGS, &_api, EXEC_SERIAL);
//    log_debug("MB:mod-ble-scanA-tag inited");
}
