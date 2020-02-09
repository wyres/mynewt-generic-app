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
 * Production test module, only used in production first flash target
 */

#include "os/os.h"

#include "bsp/bsp.h"
#include "hal/hal_gpio.h"
#include "wyres-generic/wutils.h"
#include "wyres-generic/timemgr.h"
#include "wyres-generic/configmgr.h"
#include "wyres-generic/rebootmgr.h"
#include "wyres-generic/ledmgr.h"
#include "wyres-generic/movementmgr.h"
#include "wyres-generic/sensormgr.h"

#include "app-core/app_core.h"
#include "app-core/app_msg.h"


// COntext data
static struct appctx {
    uint32_t nRuns;
    bool hwFail;
} _ctx = {
    .nRuns=0,
    .hwFail=false,
};

static void halt_fail() {
    ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
    ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
    ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_5HZ, -1);
    ledStart(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_5HZ, -1);
    while(1) {
        // do nothing but block the appcore SM 
    }

}
// My api functions
static uint32_t start() {
    // if first or second time, do tests and send data
    if (_ctx.nRuns++ < 2) {
        log_debug("PT:start env");
        // sensors that require power up or significant check time
        if (!SRMgr_start() || !MMMgr_start()) {
            // failure
            log_warn("PT : sensor failure alti or accelero");
            halt_fail();
        }
        // If target for testing BLE or BLE/GPS dcard, start comm with it to test it
#if MYNEWT_VAL(DCARD_BLE) 
        // TODO
#endif
#if MYNEWT_VAL(DCARD_BLEGPS) 
        // TODO
#endif

        log_warn("PT:checking hw 1s");
        return 1*1000;
    } 
    if (_ctx.hwFail) {
        log_warn("PT : sensor checks indicated bad data");
        halt_fail();
    }
    log_warn("PT: done checking hw, card is OK...");
    ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
    ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
    ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_05HZ, -1);
    ledStart(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_05HZ, -1);
    while(1) {
        // do nothing but block the appcore SM 
    }
    assert(0);      // never gets here
}

static void stop() {
    log_debug("PT:done");
    SRMgr_stop();
    MMMgr_stop();
}
static void off() {
    // ensure sensors are low power mode
    MMMgr_stop();
    SRMgr_stop();
}
static void deepsleep() {
    // ensure sensors are off
    SRMgr_stop();
    MMMgr_stop();
}
static bool getData(APP_CORE_UL_t* ul) {
    log_info("PT: data check and UL");
    uint8_t v[16];

    // Create UL message with hw check data, etc
    // firmware version, build number (note this relates to the prod test target, not the final target!!)
    APP_CORE_FW_t* fw = AppCore_getFwInfo();
    // Only sending up the minimum
    /* equivalent structure but we explicitly pack our data
    struct {
        uint8_t maj;
        uint8_t min;
        uint16_t buildNb;
    } */
    v[0] = (uint8_t)(fw->fwmaj);
    v[1] = (uint8_t)(fw->fwmin);
    v[2] = (uint8_t)(fw->fwbuild & 0xff);
    v[3] = (uint8_t)((fw->fwbuild & 0xff00) >> 8);
    app_core_msg_ul_addTLV(ul, APP_CORE_UL_VERSION, 4, v);

    // movement XYZ
    v[0] = MMMgr_getOrientation();
    MMMgr_getXYZ((int8_t*)&v[1], (int8_t*)&v[2], (int8_t*)&v[3]);
    app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_ORIENT, 4, v);
    
    // alti temp/pressure
    Util_writeLE_int16_t(v, 0, SRMgr_getTempcC());
    app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_TEMP, 2, v);
    // If temp out of wack, fail
    if (SRMgr_getTempcC()<-4000 || SRMgr_getTempcC()>9000) {
        log_warn("PT: Temp %d bad range!", SRMgr_getTempcC());
        _ctx.hwFail=true;
    }

    // get altimetre
    Util_writeLE_int32_t(v, 0, SRMgr_getPressurePa());
    app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_PRESSURE, 4, v);
    // if pressure bad, fail
    if (SRMgr_getPressurePa()<90000 || SRMgr_getPressurePa()>120000) {
        log_warn("PT: pressure %d bad range!", SRMgr_getPressurePa());
        _ctx.hwFail=true;
    }

    // light
    v[0] = SRMgr_getLight();
    app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_LIGHT, 1, v);
    // get battery
    Util_writeLE_uint16_t(v, 0, SRMgr_getBatterymV());
    app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_BATTERY, 2, v);
    // if battery seems weird, fail
    if (SRMgr_getBatterymV()<2000 || SRMgr_getBatterymV()>4000) {
        log_warn("PT: battery mV %d bad range!", SRMgr_getBatterymV());
        _ctx.hwFail=true;
    }

    // Send 'real' devEUI so that this card can be identified : temporarily stored in the appEUI during prod test
    // Get it from config into a GETCFG TLV, flagged as the devEUI config
    Util_writeLE_uint16_t(v, 0, CFG_UTIL_KEY_LORA_DEVEUI);
    v[2] = 8;       // length of cfg key
    CFMgr_getOrAddElement(CFG_UTIL_KEY_LORA_APPEUI, &v[3], 8);
    app_core_msg_ul_addTLV(ul, APP_CORE_UL_CONFIG, 3+8, v);
    return true;        // must send report
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
void mod_prodtest_init(void) {
    // hook app-core for implication in round data collection (stealing the PTI module's id here)
    AppCore_registerModule(APP_MOD_PTI, &_api, EXEC_PARALLEL);

//    log_debug("MP:initialised");
}

// internals
