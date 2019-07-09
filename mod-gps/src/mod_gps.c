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

#include "app-core/app_core.h"
#include "app-core/app_msg.h"

#define MIN_GOOD_FIXES 2

// COntext data
static struct appctx {
    uint32_t goodFixCnt;
    gps_data_t goodFix;
} _ctx = {
    .goodFix.rxAt=0,
};

static void gps_cb(GPS_EVENT_TYPE_t e) {
    switch(e) {
        case GPS_COMM_FAIL: {
            log_debug("gps comm nok");
            // This means we're done
            AppCore_module_done(APP_MOD_GPS);
            gps_stop();
            break;
        }
        case GPS_COMM_OK: {
            log_debug("gps comm ok");
            break;
        }
        case GPS_SATOK: {
            log_debug("gps locks on to sats");
            break;
        }
        case GPS_NEWFIX: {
            // YES :  read and move on
            gps_getData(&_ctx.goodFix);
            if (_ctx.goodFixCnt++ > MIN_GOOD_FIXES) {
                log_debug("gps new fix done");
                // This means we're done
                AppCore_module_done(APP_MOD_GPS);
                gps_stop();
            } else {
                log_debug("gps new fix");
            }
            break;
        }
        case GPS_SATLOSS: {
            // also means given up
            log_debug("gps has no sat lock");
            AppCore_module_done(APP_MOD_GPS);
            gps_stop();
            break;
        }
        default:
            break;            
    }
}

// My api functions
static uint32_t start() {
    uint32_t coldStartTime=5*60;
    uint32_t warmStartTime=60;
    CFMgr_getOrAddElement(CFG_UTIL_KEY_GPS_COLD_TIME_SECS, &coldStartTime, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_GPS_WARM_TIME_SECS, &warmStartTime, sizeof(uint32_t));

    // leaving to do somehting with the GPS, so tell it to go with a callback to tell me when its got something
    // If we had a lock before, and it was <24 hours, we should get a fix rapidly (if we can)
    int32_t fixage = gps_lastGPSFixAgeMins();
    uint32_t gpstimeout = warmStartTime + (fixage/24);      // adjust minimum fix time by up to 60s if last fix is old
    if (fixage<0 || fixage > 24*60) {
        // no fix last time, or was too long ago - could take 5 mins to find satellites?
        gpstimeout = coldStartTime;
    }
//    log_debug("mod-gps last %d m - next fix in %d s", fixage, gpstimeout);
    _ctx.goodFixCnt=0;
    gps_start(gps_cb, 0);
    return gpstimeout*1000;         // return time required in ms
}
static void stop() {
    gps_stop();
//    log_debug("finished mod-gps");
}
static void sleep() {
    // TODO
}
static void deepsleep() {
    // TODO
}
static bool getData(APP_CORE_UL_t* ul) {
    if (_ctx.goodFixCnt>0 && _ctx.goodFix.rxAt!=0) {
        app_core_msg_ul_addTLV(ul, APP_CORE_GPS, sizeof(gps_data_t), &_ctx.goodFix);
        log_debug("good gps fix for UL");
        return true;
    }
    return false;
}

static APP_CORE_API_t _api = {
    .startCB = &start,
    .stopCB = &stop,
    .sleepCB = &sleep,
    .deepsleepCB = &deepsleep,
    .getULDataCB = &getData,    
};
// Initialise module
void mod_gps_init(void) {
    // initialise access to GPS
    gps_mgr_init(MYNEWT_VAL(GPS_UART), MYNEWT_VAL(GPS_PWRIO), MYNEWT_VAL(GPS_UART_SELECT));
    // hook app-core for gps operation
    AppCore_registerModule(APP_MOD_GPS, &_api, EXEC_SERIAL);
//    log_debug("mod-gps inited");
}
