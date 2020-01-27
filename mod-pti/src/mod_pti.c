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
    // TOD check movement, falls, etc
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
    AppCore_registerModule(APP_MOD_PTI, &_api, EXEC_PARALLEL);

    // add cb for button press, no context required
    SRMgr_registerButtonCB(buttonChangeCB, NULL);
//    log_debug("MP:initialised");
}

// internals
// callback each time button changes state
static void buttonChangeCB(void* ctx, SR_BUTTON_STATE_t currentState, SR_BUTTON_PRESS_TYPE_t currentPressType) {
    if (currentState==SR_BUTTON_RELEASED) {
        // note using log_noout as button shares GPIO with debug log uart...
        log_noout("MP:button released, duration %d ms, press type:%d", 
            (SRMgr_getLastButtonReleaseTS()-SRMgr_getLastButtonPressTS()),
            SRMgr_getLastButtonPressType());
        _ctx.lastRelease = SRMgr_getLastButtonReleaseTS();
        // ask for immediate UL with only us consulted
        AppCore_forceUL(APP_MOD_PTI);
    } else {
        log_noout("MP:button pressed");
    }
}