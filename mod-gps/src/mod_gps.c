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
/**
 * Module to provide gps service to app core
 */

#include "os/os.h"

#include "bsp/bsp.h"
#include "wyres-generic/wutils.h"
#include "wyres-generic/configmgr.h"
#include "wyres-generic/gpsmgr.h"
#include "wyres-generic/sm_exec.h"
#include "wyres-generic/movementmgr.h"
#include "wyres-generic/timemgr.h"

#include "app-core/app_core.h"
#include "app-core/app_msg.h"
#include "mod-gps/mod_gps.h"

#define MIN_GOOD_FIXES (5)          // Must get 5 good fixes with acceptable precision to exit
#define REQUEST_N_TIMES (1)         // How many rounds to do a fix for when request by backend DL action?
#define ACCEPTABLE_PRECISION_DM (200)   // accept fixes when precision is estimated as <20.0m (200dm)
// COntext data
static struct appctx {
    uint32_t goodFixCnt;
    uint8_t fixDemanded;   // did we get a DL action asking for a fix?
    bool doFix;             // did we try to do a fix this round?
    gps_data_t goodFix;     // good (merged) fix
    gps_data_t currFix;     // current fix got from mgr
} _ctx;     // all initialised to 0 as bss

static void logGPSPosition(gps_data_t* pos);
static bool mergeNewGPSFix() {
    if (gps_getData(&_ctx.currFix)) {
        // if no good fix currently, or a not very good one, just copy the new one (as long as its better)
        // This ensures we end up with goodFix containing a fix of some kind, even if its not the optimal result
        if (_ctx.goodFix.rxAt==0) {
            _ctx.goodFix.lat = _ctx.currFix.lat;
            _ctx.goodFix.lon = _ctx.currFix.lon;
            _ctx.goodFix.alt = _ctx.currFix.alt;
            _ctx.goodFix.prec = _ctx.currFix.prec;
            _ctx.goodFix.rxAt = _ctx.currFix.rxAt;
            return true;        // got at least 1 fix
        } else if ((_ctx.goodFix.prec > ACCEPTABLE_PRECISION_DM) && (_ctx.currFix.prec < _ctx.goodFix.prec)) {
            _ctx.goodFix.lat = _ctx.currFix.lat;
            _ctx.goodFix.lon = _ctx.currFix.lon;
            _ctx.goodFix.alt = _ctx.currFix.alt;
            _ctx.goodFix.prec = _ctx.currFix.prec;
            _ctx.goodFix.rxAt = _ctx.currFix.rxAt;
            // tell caller if they got an acceptable one here
            return (_ctx.goodFix.prec < ACCEPTABLE_PRECISION_DM);
        } else {
            // Merge new fix with historic via averaging if its reasonable
            if (_ctx.currFix.prec < ACCEPTABLE_PRECISION_DM) {
                _ctx.goodFix.lat = (_ctx.goodFix.lat+_ctx.currFix.lat)/2;
                _ctx.goodFix.lon = (_ctx.goodFix.lon+_ctx.currFix.lon)/2;
                _ctx.goodFix.alt = (_ctx.goodFix.alt+_ctx.currFix.alt)/2;
                _ctx.goodFix.prec = (_ctx.goodFix.prec+_ctx.currFix.prec)/2;
                _ctx.goodFix.rxAt = _ctx.currFix.rxAt;
                return true;
            }
        }
    }
    return false;       // no new fix merged
}

static void gps_cb(GPS_EVENT_TYPE_t e) {
    switch(e) {
        case GPS_COMM_FAIL: {
            log_debug("MG: comm nok");
            // This means we're done
            gps_stop();
            AppCore_module_done(APP_MOD_GPS);
            break;
        }
        case GPS_COMM_OK: {
            log_debug("MG: comm ok");
            break;
        }
        case GPS_SATOK: {
            log_debug("MG: lock");
            break;
        }
        case GPS_NEWFIX: {
            // decide if precision is good enough and can merge in new value. If so, see if we can stop.
            // This is only an option when not doing fixOnDemand triggered by backend
            // action, as we want to go the full timeout to get best result
            if (mergeNewGPSFix()) {
                if (_ctx.fixDemanded==0 &&
                        _ctx.goodFixCnt++ > MIN_GOOD_FIXES) {
                    log_debug("MG: fix done");
                    // This means we're done
                    AppCore_module_done(APP_MOD_GPS);
                    gps_stop();
                } else {
                    log_debug("MG: fix ok");
                }
            } else {
                log_debug("MG: fix nok");
            }
            break;
        }
        case GPS_SATLOSS: {
            // also means given up
            log_debug("MG: no sat lock");
            AppCore_module_done(APP_MOD_GPS);
            gps_stop();
            break;
        }
        case GPS_DONE: {
            log_debug("MG: done");
            break;
        }
        default:
            break;            
    }
}

// My api functions
static uint32_t start() {
    uint32_t coldStartTime=2*60;
    uint32_t warmStartTime=60;
    uint8_t powermode = POWER_ONOFF;     
    uint8_t fixmode = FIX_ALWAYS; // FIX_ON_DEMAND;
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_GPS_COLD_TIME_SECS, &coldStartTime, 10, 15*60);
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_GPS_WARM_TIME_SECS, &warmStartTime, 1, 15*60);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_GPS_POWER_MODE, &powermode, sizeof(uint8_t));
    gps_setPowerMode(powermode);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_GPS_FIX_MODE, &fixmode, sizeof(uint8_t));

    // how many good fixes this time?
    _ctx.goodFixCnt = 0;
    _ctx.goodFix.rxAt = 0;  // not got one yet...

    int32_t fixage = gps_lastGPSFixAgeMins();
    uint32_t gpstimeout = 1;        // 1 second if we don't decide to do a fix
    _ctx.doFix = false;
    // TODO conditions of movement vs fixmode
    switch(fixmode) {
        case FIX_WHILE_MOVING: {
            // if last moved time < fix age, do fix
            if (MMMgr_hasMovedSince(gps_lastGPSFixTimeSecs())) {
                _ctx.doFix = true;
            }
            break;
        }
        case FIX_ON_STOP: {
            // if last moved time > 5 mins ago, and fix time before last moved time, do fix
            if (MMMgr_hasMovedSince(gps_lastGPSFixTimeSecs()) &&
                    ((TMMgr_getRelTimeSecs() - MMMgr_getLastMovedTime()) > 5*60)) {
                _ctx.doFix = true;
            }
            break; 
        }
        case FIX_ALWAYS: {
            _ctx.doFix = true;
            break;
        }
        case FIX_ON_DEMAND: {
            // Check if demand outstanding
            if (_ctx.fixDemanded>0) {
                _ctx.doFix = true;
                _ctx.fixDemanded--;       // dec shots
            }
        }
        default: 
            _ctx.doFix = false;
            break;
    }
    // TODO check global flag indicating if we got indoor loc, in this case may not want to do gps???
    // or maybe just have a short timeout?

    // Depending on fix mode, we start GPS or not this time round...
    if (_ctx.doFix) {
        // leaving to do somehting with the GPS, so tell it to go with a callback to tell me when its got something
        if (fixage<0 || fixage > 24*60) {
            // no fix last time, or was too long ago - could take 5 mins to find satellites?
            gpstimeout = coldStartTime;
        } else {
            // If we had a lock before, and it was <24 hours, we should get a fix rapidly (if we can)
            gpstimeout = warmStartTime + (fixage/24);      // adjust minimum fix time by up to 60s if last fix is old
        }
    //    log_debug("mod-gps last %d m - next fix in %d s", fixage, gpstimeout);
        gps_start(gps_cb, 0);
        return gpstimeout*1000;         // return time required in ms
    } else {
        return 0;       // no op required
    }
}
static void stop() {
    gps_stop();
//    log_debug("finished mod-gps");
}
static void off() {
    gps_stop();
    // nothing to do
}
static void deepsleep() {
    gps_stop();
    // nothing to do
}
static bool getData(APP_CORE_UL_t* ul) {
    if (_ctx.doFix) {
        // Did we get a fix this time?
        if (_ctx.goodFix.rxAt!=0) {
            app_core_msg_ul_addTLV(ul, APP_CORE_UL_GPS, sizeof(gps_data_t), &_ctx.goodFix);
            log_info("MG: UL fix %d,%d,%d p=%d from %d sats", _ctx.goodFix.lat, _ctx.goodFix.lon, _ctx.goodFix.alt, _ctx.goodFix.prec, _ctx.goodFix.nSats);
            // Log this position with timestamp (can be retrieved with DL action)
            logGPSPosition(&_ctx.goodFix);
        } else {
            log_info("MG: no fix for UL");
            // Send empty TLV to indicate we tried, but didn't get a fix
            app_core_msg_ul_addTLV(ul, APP_CORE_UL_GPS, 0, NULL);
        }
        // always UL as we tried...
        return true;
    }
    return false;       // didn't try to do fix, so no data
}

static APP_CORE_API_t _api = {
    .startCB = &start,
    .stopCB = &stop,
    .offCB = &off,
    .deepsleepCB = &deepsleep,
    .getULDataCB = &getData,
    .ticCB = NULL,
};

// DL action to request GPS FIX
static void A_fixgps(uint8_t* v, uint8_t l) {
    log_debug("MG:action FIX ");
    _ctx.fixDemanded = REQUEST_N_TIMES;       // we try N rounds to do a fix from the action
    AppCore_forceUL(-1);        // just do everyone? or check gps module is enabled?
}

// Initialise module
void mod_gps_init(void) {
    // initialise access to GPS
    gps_mgr_init(MYNEWT_VAL(MOD_GPS_UART), MYNEWT_VAL(MOD_GPS_UART_BAUDRATE), MYNEWT_VAL(MOD_GPS_PWRIO), MYNEWT_VAL(MOD_GPS_UART_SELECT));
    // hook app-core for gps operation
    AppCore_registerModule(APP_MOD_GPS, &_api, EXEC_SERIAL);
    // Register for the gps action(s)
    AppCore_registerAction(APP_CORE_DL_FIX_GPS, &A_fixgps);
//    log_debug("mod-gps inited");
}

static void logGPSPosition(gps_data_t* pos) {
    // Store position in circular buffer in config (for easy access)
    // Add new fix with timestamp
    // use delta from last values, code with CBOR?
//    LGMgr_addElement(LOGGING_KEY_GPS, pos);

}