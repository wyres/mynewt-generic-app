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
#include "wyres-generic/rebootMgr.h"
#include "wyres-generic/movementmgr.h"
#include "wyres-generic/sensorMgr.h"

#include "app-core/app_core.h"
#include "app-core/app_msg.h"
#include "mod-env/mod_env.h"

// may wish to may configurable?
#define NB_REBOOT_INFOS (4)

// COntext data
static struct appctx {
    uint8_t sentRebootInfo;
} _ctx = {
    .sentRebootInfo=NB_REBOOT_INFOS,
};
static void buttonChangeCB();

// My api functions
static uint32_t start() {
    log_debug("start env data collection");
    // sensors that require power up or significant check time
    SRMgr_start();
    log_debug("started env data collection for 1s");
    return 1*1000;
}

static void stop() {
    log_debug("done with env data collection");
    SRMgr_stop();
}
static void sleep() {
    // TODO ensure sensors are low power mode
    SRMgr_stop();
}
static void deepsleep() {
    // TODO ensure sensors are off
    SRMgr_stop();
}
static bool getData(APP_CORE_UL_t* ul) {
    log_debug("adding env data collection to UL");
    // if first N times after reboot, add reboot info
    if (_ctx.sentRebootInfo >0) {
        _ctx.sentRebootInfo--;
        // add to UL - last 8 reboot reasons
        uint8_t buf[8];
        RMMgr_getResetReasonBuffer(buf,8);
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_REBOOT, 8, buf);
        // last asset reason
        void* la = RMMgr_getLastAssertCallerFn();
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_LASTASSERT, sizeof(la), &la);
    }
    // get accelero if changed since last UL
    if (MMMgr_getLastMovedTime() >= AppCore_lastULTime()) {
        struct {
            uint32_t lastMoveTS;
            uint8_t movePerUL;
        } v;
        v.lastMoveTS = MMMgr_getLastMovedTime();
        v.movePerUL = 1;       // TODO should record for last 8 ULs...
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_MOVE, sizeof(v), &v);
    }
    if (MMMgr_getLastFallTime() >= AppCore_lastULTime()) {
        struct {
            uint32_t lastFallTS;
            uint8_t fallPerUL;
        } v;
        v.lastFallTS = MMMgr_getLastFallTime();
        v.fallPerUL = 1;       // TODO should record for last 8 ULs...
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_FALL, sizeof(v), &v);
    }
    if (MMMgr_getLastShockTime() >= AppCore_lastULTime()) {
        struct {
            uint32_t lastShockTS;
            uint8_t shockPerUL;
        } v;
        v.lastShockTS = MMMgr_getLastShockTime();
        v.shockPerUL = 1;       // TODO should record for last 8 ULs...
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_SHOCK, sizeof(v), &v);
    }
    // orientation direct (if changed) 
    if (MMMgr_getLastOrientTime() >= AppCore_lastULTime()) {
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
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_ORIENT, sizeof(v), &v);
    }
    // Basic environmental stuff
    if (SRMgr_hasLightChanged()) {
        // get luminaire
        uint8_t vl = SRMgr_getLight();
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_LIGHT, sizeof(vl), &vl);
    }
    if (SRMgr_hasBattChanged()) {
        // get battery
        uint16_t vb = SRMgr_getBatterymV();
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_BATTERY, sizeof(vb), &vb);
    }
    if (SRMgr_hasPressureChanged()) {
        // get altimetre
        uint32_t vp = SRMgr_getPressurePa();
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_PRESSURE, sizeof(vp), &vp);
    }
    if (SRMgr_hasTempChanged()) {
        // get temperature
        int16_t vt = SRMgr_getTempdC();
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_TEMP, sizeof(vt), &vt);
    }
    if (SRMgr_hasADC1Changed()) {
        // get adc 1
        uint16_t vv1 = SRMgr_getADC1mV();
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_ADC1, sizeof(vv1), &vv1);
    }
    if (SRMgr_hasADC2Changed()) {
        // get adc 2
        uint16_t vv2 = SRMgr_getADC2mV();
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_ADC2, sizeof(vv2), &vv2);
    }
    // get micro if noise detected
    if (SRMgr_getLastNoiseTime() >= AppCore_lastULTime()) {
        struct {
            uint32_t time;
            uint8_t freqkHz;
            uint8_t leveldB;
        } v;
        v.time = SRMgr_getLastNoiseTime();
        v.freqkHz = SRMgr_getNoiseFreqkHz();
        v.leveldB = SRMgr_getNoiseLeveldB();
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_NOISE, sizeof(v), &v);
    }

    // get button
    if (SRMgr_getLastButtonPressTS() >= AppCore_lastULTime()) {
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
        app_core_msg_ul_addTLV(ul, APP_CORE_ENV_BUTTON, sizeof(v), &v);
    }
    return SRMgr_updateEnvs();     // update those that changed as we have added them to the UL... and return the flag that says if any changed
}

static APP_CORE_API_t _api = {
    .startCB = &start,
    .stopCB = &stop,
    .sleepCB = &sleep,
    .deepsleepCB = &deepsleep,
    .getULDataCB = &getData,    
};
// Initialise module
void mod_env_init(void) {
    // hook app-core for env data
    AppCore_registerModule(APP_MOD_ENV, &_api, EXEC_PARALLEL);

    // add cb for button press
    SRMgr_registerButtonCB(buttonChangeCB);
//    log_debug("mod-env initialised");
}

// internals
// callback each time button changes state
static void buttonChangeCB() {
    uint8_t bs = SRMgr_getButton();
    if (bs==SR_BUTTON_RELEASED) {
        // note using log_noout as button shares GPIO with debug log uart...
        log_noout("button released, duration %d ms, press type:%d", 
            (SRMgr_getLastButtonReleaseTS()-SRMgr_getLastButtonPressTS()),
            SRMgr_getLastButtonPressType());
        // ask for immediate UL
        AppCore_forceUL();
    } else {
        log_noout("button pressed");
    }
}