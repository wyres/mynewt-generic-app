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
 * Environment sensor handling generic app module
 */

#include "os/os.h"

#include "wyres-generic/wutils.h"
#include "wyres-generic/timemgr.h"
#include "wyres-generic/rebootmgr.h"
#include "wyres-generic/configmgr.h"
#include "wyres-generic/movementmgr.h"
#include "wyres-generic/sensormgr.h"

#include "app-core/app_core.h"
#include "app-core/app_msg.h"
#include "mod-env/mod_env.h"

// may wish to may configurable?
// Send debug data at startup twice
#define NB_REBOOT_INFOS (2)
// Force uplink with env data every hour?
#define FORCE_UL_INTERVAL_MS (60*60*1000)
// COntext data
static struct appctx {
    uint8_t sentRebootInfo;
    int32_t pressureOffsetPa;
} _ctx = {
    .sentRebootInfo=NB_REBOOT_INFOS,
    .pressureOffsetPa=0,
};
static void buttonChangeCB(void* ctx, SR_BUTTON_STATE_t currentState, SR_BUTTON_PRESS_TYPE_t currentPressType);
static void A_getdebug(uint8_t* v, uint8_t l);

// My api functions
static uint32_t start() {
    log_debug("ME:start env");
    // sensors that require power up or significant check time
    SRMgr_start();
    MMMgr_check();
    log_debug("ME:for 1s");
    return 1*1000;
}

static void stop() {
    log_debug("ME:done");
    SRMgr_stop();
}
static void sleep() {
    // ensure sensors are low power mode
    SRMgr_stop();
}
static void deepsleep() {
    // ensure sensors are off
    SRMgr_stop();
}
static bool getData(APP_CORE_UL_t* ul) {
    log_info("ME: UL env");
    // Decide if gonna force the UL to include current values and to be sent. 
    // TODO note this essentially override the 'max time between UL' setting in the appcore code
    bool forceULData = ((TMMgr_getRelTime() - AppCore_lastULTime()) > FORCE_UL_INTERVAL_MS);
    // if first N times after reboot, add reboot info
    if (_ctx.sentRebootInfo >0) {
        _ctx.sentRebootInfo--;
        // add to UL - last 8 reboot reasons
        uint8_t buf[8];
        RMMgr_getResetReasonBuffer(buf,8);
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_REBOOT, 8, buf);
        // last asset reason
        void* la = RMMgr_getLastAssertCallerFn();
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_LASTASSERT, sizeof(la), &la);
        // fn log list : only return the most recent one
        void* lf = RMMgr_getLogFn(0);
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_LASTLOGCALLER, sizeof(lf), &lf);
        // firmware version, build date
        APP_CORE_FW_t* fw = AppCore_getFwInfo();
        // Only sending up the minimum
        struct {
            uint8_t maj;
            uint8_t min;
            uint8_t build;
        } fwdata = {
            .maj = (uint8_t)(fw->fwmaj),
            .min = (uint8_t)(fw->fwmin),
            .build = (uint8_t)(fw->fwbuild),
        };
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_VERSION, sizeof(fwdata), &fwdata);
        forceULData = true;     // as we sent debug
    }
    // get accelero if changed since last UL
    if (MMMgr_getLastMovedTime() >= AppCore_lastULTime()) {
        struct {
            uint32_t lastMoveTS;
            uint8_t movePerUL;
        } v;
        v.lastMoveTS = MMMgr_getLastMovedTime();
        v.movePerUL = 1;       // TODO should record for last 8 ULs...
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_MOVE, sizeof(v), &v);
    }
    if (MMMgr_getLastFallTime() >= AppCore_lastULTime()) {
        struct {
            uint32_t lastFallTS;
            uint8_t fallPerUL;
        } v;
        v.lastFallTS = MMMgr_getLastFallTime();
        v.fallPerUL = 1;       // TODO should record for last 8 ULs...
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_FALL, sizeof(v), &v);
    }
    if (MMMgr_getLastShockTime() >= AppCore_lastULTime()) {
        struct {
            uint32_t lastShockTS;
            uint8_t shockPerUL;
        } v;
        v.lastShockTS = MMMgr_getLastShockTime();
        v.shockPerUL = 1;       // TODO should record for last 8 ULs...
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_SHOCK, sizeof(v), &v);
    }
    // orientation direct (if changed) 
    if (forceULData || MMMgr_getLastOrientTime() >= AppCore_lastULTime()) {
        struct {
            uint8_t orient;
            int8_t x;
            int8_t y;
            int8_t z;
        } v;
        v.orient = MMMgr_getOrientation();
        v.x = MMMgr_getXdG();
        v.y = MMMgr_getYdG();
        v.z = MMMgr_getZdG();        
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_ORIENT, sizeof(v), &v);
    }
    // Basic environmental stuff
    if (forceULData || SRMgr_hasLightChanged()) {
        // get luminaire
        uint8_t vl = SRMgr_getLight();
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_LIGHT, sizeof(vl), &vl);
    }
    if (forceULData || SRMgr_hasBattChanged()) {
        // get battery
        uint16_t vb = SRMgr_getBatterymV();
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_BATTERY, sizeof(vb), &vb);
    }
    if (forceULData || SRMgr_hasPressureChanged()) {
        // get altimetre
        int32_t vp = SRMgr_getPressurePa();
        // apply offset
        vp = vp + _ctx.pressureOffsetPa;
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_PRESSURE, sizeof(vp), &vp);
    }
    if (forceULData || SRMgr_hasTempChanged()) {
        // get temperature
        int16_t vt = SRMgr_getTempdC();
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_TEMP, sizeof(vt), &vt);
    }
    if (SRMgr_hasADC1Changed()) {
        // get adc 1
        uint16_t vv1 = SRMgr_getADC1mV();
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_ADC1, sizeof(vv1), &vv1);
    }
    if (SRMgr_hasADC2Changed()) {
        // get adc 2
        uint16_t vv2 = SRMgr_getADC2mV();
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_ADC2, sizeof(vv2), &vv2);
    }
    // get micro if noise detected
    if (forceULData || SRMgr_getLastNoiseTime() >= AppCore_lastULTime()) {
        struct {
            uint32_t time;
            uint8_t freqkHz;
            uint8_t leveldB;
        } v;
        v.time = SRMgr_getLastNoiseTime();
        v.freqkHz = SRMgr_getNoiseFreqkHz();
        v.leveldB = SRMgr_getNoiseLeveldB();
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_NOISE, sizeof(v), &v);
    }

    // get button
    if (forceULData || SRMgr_getLastButtonPressTS() >= AppCore_lastULTime()) {
        struct {
            uint32_t pressTS;
            uint32_t releaseTS;
            uint8_t currState;
            uint8_t lastPressType;
        } v;
        v.pressTS = SRMgr_getLastButtonPressTS();
        v.releaseTS = SRMgr_getLastButtonReleaseTS();
        v.currState = SRMgr_getButton();
        v.lastPressType = SRMgr_getLastButtonPressType();
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_BUTTON, sizeof(v), &v);
    }
    return forceULData || SRMgr_updateEnvs();     // update those that changed as we have added them to the UL... and return the flag that says if any changed
}

static APP_CORE_API_t _api = {
    .startCB = &start,
    .stopCB = &stop,
    .sleepCB = &sleep,
    .deepsleepCB = &deepsleep,
    .getULDataCB = &getData,
    .ticCB = NULL,    
};
// Initialise module
void mod_env_init(void) {
    // Sensor initialisation
    // TODO
    // Altimeter offset calibration
    // Read reference pressure (set by testbed production) in PASCAL
    uint32_t pref = 0;
    CFMgr_getOrAddElement(CFG_UTIL_KEY_ENV_PRESSURE_REF, &pref, sizeof(pref));
    // If not 0, calculate offset and write to config
    if (pref!=0) {
        // Get current pressure
        int32_t currp = SRMgr_getPressurePa();
        // Calculate offset
        _ctx.pressureOffsetPa = pref - currp;
        CFMgr_setElement(CFG_UTIL_KEY_ENV_PRESSURE_OFFSET, &_ctx.pressureOffsetPa, sizeof(_ctx.pressureOffsetPa));
        // reset reference to 0 (we only do the calibration offset calc immediately after testbed set it)
        pref=0;
        CFMgr_setElement(CFG_UTIL_KEY_ENV_PRESSURE_REF, &pref, sizeof(pref));
    } else {
        // get previously calculated calibration offset to use
        CFMgr_getOrAddElement(CFG_UTIL_KEY_ENV_PRESSURE_OFFSET, &_ctx.pressureOffsetPa, sizeof(_ctx.pressureOffsetPa));
    }
    // hook app-core for env data
    AppCore_registerModule(APP_MOD_ENV, &_api, EXEC_PARALLEL);
    // an action
    AppCore_registerAction(APP_CORE_DL_GET_DEBUG, &A_getdebug);

    // add cb for button press, no context required
    SRMgr_registerButtonCB(buttonChangeCB, NULL);
//    log_debug("mod-env initialised");
}

// internals
// callback each time button changes state
static void buttonChangeCB(void* ctx, SR_BUTTON_STATE_t currentState, SR_BUTTON_PRESS_TYPE_t currentPressType) {
    if (currentState==SR_BUTTON_RELEASED) {
        // note using log_noout as button shares GPIO with debug log uart...
        log_noout("ME:button released, duration %d ms, press type:%d", 
            (SRMgr_getLastButtonReleaseTS()-SRMgr_getLastButtonPressTS()),
            SRMgr_getLastButtonPressType());
        // ask for immediate UL with only us consulted
        AppCore_forceUL(APP_MOD_ENV);
    } else {
        log_noout("ME:button pressed");
    }
}
// Get debug data in next UL
static void A_getdebug(uint8_t* v, uint8_t l) {
    log_info("AC:action GETDEBUG");    
    _ctx.sentRebootInfo=1;      // so it gets sent in next UL one time
}
