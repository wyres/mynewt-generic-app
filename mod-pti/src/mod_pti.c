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
 * PTI feature handling app module
 */

#include "os/os.h"

#include "wyres-generic/wutils.h"
#include "wyres-generic/timemgr.h"
#include "wyres-generic/rebootmgr.h"
#include "wyres-generic/movementmgr.h"
#include "wyres-generic/sensormgr.h"

#include "app-core/app_core.h"
#include "app-core/app_msg.h"

#define USER_BUTTON  ((int8_t)MYNEWT_VAL(BUTTON_IO))

// COntext data
static struct appctx {
    uint32_t lastRelease;
} _ctx;
static void buttonChangeCB(void* ctx, SR_BUTTON_STATE_t currentState, SR_BUTTON_PRESS_TYPE_t currentPressType);

// My api functions
static uint32_t start() {
    log_debug("MP:start env");
    // sensors that require power up or significant check time
    SRMgr_start();
    MMMgr_check();
    log_debug("MP:for 1s");
    return 1*1000;
}

static void stop() {
    log_debug("MP:done");
    SRMgr_stop();
}
static void off() {
    // ensure sensors are low power mode
    SRMgr_stop();
}
static void deepsleep() {
    // ensure sensors are off
    SRMgr_stop();
}
static bool getData(APP_CORE_UL_t* ul) {
    log_info("MP: UL env last button @%d", _ctx.lastRelease);
    uint8_t v[12];
    // get button
    if (((SRMgr_getLastButtonPressTS(USER_BUTTON)/1000) >= AppCore_lastULTime())) {
        /* equivalent structure but we explicitly pack our data
        struct {
            uint32_t pressTS;
            uint32_t releaseTS;
            uint8_t currState;
            uint8_t lastPressType;
        } v;
        */
        Util_writeLE_uint32_t(v, 0, SRMgr_getLastButtonPressTS(USER_BUTTON));
        Util_writeLE_uint32_t(v, 4, SRMgr_getLastButtonReleaseTS(USER_BUTTON));
        v[8]= SRMgr_getButton(USER_BUTTON);
        v[9] = SRMgr_getLastButtonPressType(USER_BUTTON);
        app_core_msg_ul_addTLV(ul, APP_CORE_UL_ENV_BUTTON, 10, v);
        SRMgr_updateButton(USER_BUTTON);
        return true;
    }
    return false;       // nothing vital here
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
void mod_pti_init(void) {
    // hook app-core for env data
    AppCore_registerModule("PTI", APP_MOD_PTI, &_api, EXEC_PARALLEL);

    // add cb for button press, no context required
    SRMgr_registerButtonCB(USER_BUTTON, buttonChangeCB, NULL);
//    log_debug("MP:initialised");
}

// internals
// callback each time button changes state
static void buttonChangeCB(void* ctx, SR_BUTTON_STATE_t currentState, SR_BUTTON_PRESS_TYPE_t currentPressType) {
    if (currentState==SR_BUTTON_RELEASED) {
        // note using log_noout as button shares GPIO with debug log uart...
        log_noout("MP:button released");
        // ask for immediate UL with only us consulted
        AppCore_forceUL(APP_MOD_PTI);
    } else {
        log_noout("MP:button pressed");
    }
}