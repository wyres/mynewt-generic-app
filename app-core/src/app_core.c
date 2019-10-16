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


#include "os/os.h"

#include "bsp.h"
#include "wyres-generic/wutils.h"
#include "wyres-generic/configmgr.h"
#include "wyres-generic/ledmgr.h"
#include "wyres-generic/movementmgr.h"
#include "wyres-generic/sm_exec.h"
#include "wyres-generic/lowpowermgr.h"
#include "wyres-generic/loraapp.h"
#include "wyres-generic/timemgr.h"
#include "wyres-generic/rebootmgr.h"

#include "app-core/app_console.h"
#include "app-core/app_core.h"
#include "app-core/app_msg.h"

#define MAX_DL_ACTIONS  (8)
#define MAX_MODS    MYNEWT_VAL(APP_CORE_MAX_MODS)
// Size of bit mask in bytes to contain all known modules
#define MOD_MASK_SZ ((APP_MOD_LAST/8)+1)

// State machine for core app
// COntext data
static struct appctx {
    SM_ID_t mySMId;
    uint8_t nMods;
    struct {
        APP_MOD_ID_t id;
        APP_MOD_EXEC_t exec;
        APP_CORE_API_t* api;
    } mods[MAX_MODS];   // registered modules api fns
    uint8_t modsMask[MOD_MASK_SZ];       // bit mask to indicate if module is active or not currently
    int currentSerialModIdx;
    bool ulIsCrit;              // during data collection, module can signal critical data change ie must send UL
    APP_CORE_UL_t txmsg;        // for building UL messages
    APP_CORE_DL_t rxmsg;        // for decoding DL messages
    uint32_t lastULTime;        // timestamp of last uplink
    uint32_t idleTimeMovingSecs;
    uint32_t idleTimeNotMovingMins;
    uint32_t idleTimeCheckSecs;
    uint32_t modSetupTimeSecs;
    uint32_t idleStartTS;
    uint32_t maxTimeBetweenULMins;
    bool doReboot;
    uint8_t nActions;
    ACTION_t actions[MAX_DL_ACTIONS];
    // ul response id : holds the last DL id we received. Sent in each UL to inform backend we got its DLs. 0=not listening
    uint8_t lastDLId;
} _ctx = {
    .doReboot=false,
    .nMods=0,
    .nActions=0,
    .idleTimeMovingSecs=5*60,           // 5mins
    .idleTimeNotMovingMins=120,      // 2 hours
    .idleTimeCheckSecs=60,   
    .modSetupTimeSecs=3,
    .maxTimeBetweenULMins=120,    // 2 hours
    .lastULTime=0,
    .lastDLId=0,            // default when new, will be read from the config mgr
};

// predeclarations
static void registerActions();
static void executeDL(struct appctx* ctx, APP_CORE_DL_t* data);

// Callback from config mgr when the actived modules mask changes
static void configChangedCB(uint16_t key) {
    // just re-get all my config in case it changed
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MODS_ACTIVE_MASK, &_ctx.modsMask[0], MOD_MASK_SZ);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_IDLE_TIME_MOVING_SECS, &_ctx.idleTimeMovingSecs, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_IDLE_TIME_NOTMOVING_MINS, &_ctx.idleTimeNotMovingMins, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MODSETUP_TIME_SECS, &_ctx.modSetupTimeSecs, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MAXTIME_UL_MINS, &_ctx.maxTimeBetweenULMins, sizeof(uint32_t));

}
static bool isModActive(uint8_t* mask, APP_MOD_ID_t id) {
    if (id<0 || id>=APP_MOD_LAST) {
        return false;
    }
    return ((mask[id/8] & (1<<(id%8)))!=0);
}
// application core state machine
// Define my state ids
enum MyStates { MS_STARTUP, MS_IDLE, MS_GETTING_SERIAL_MODS, MS_GETTING_PARALLEL_MODS, MS_SENDING_UL, MS_LAST };
enum MyEvents { ME_MODS_OK, ME_MODULE_DONE, ME_FORCE_UL, ME_LORA_RESULT, ME_LORA_RX, ME_CONSOLE_TIMEOUT };
// related fns

static void lora_tx_cb(LORA_TX_RESULT_t res) {
//    log_debug("lora tx cb : result:%d", res);
    // pass tx result directly as value as var 'res' may not be around when SM is run....
    sm_sendEvent(_ctx.mySMId, ME_LORA_RESULT, (void*)res);
}

static void lora_rx_cb(uint8_t port, void* data, uint8_t sz) {
    // Copy data into static message buffer in ctx as sendEvent is executed off this thread -> can't use stack var.
    if (sz>APP_CORE_DL_MAX_SZ) {
        // oops
        log_debug("AC:lora rx toobig sz %d", sz);
        return;
    }
    memcpy(&_ctx.rxmsg.payload[0], data, sz);
    _ctx.rxmsg.sz = sz;
    // Decode it
    if (app_core_msg_dl_decode(&_ctx.rxmsg)) {
        log_debug("AC:lora rx dlid %d, na %d", _ctx.rxmsg.dlId, _ctx.rxmsg.nbActions);
        sm_sendEvent(_ctx.mySMId, ME_LORA_RX, (void*)(&_ctx.rxmsg));
    } else {
        log_debug("AC:lora rx BAD sz %d b0/1 %02x:%02x", sz, ((uint8_t*)data)[0], ((uint8_t*)data)[1]);
    }
}
// SM state functions

// state for startup init, AT command line, etc before becoming idle
static SM_STATE_ID_t State_Startup(void* arg, int e, void* data) {
    struct appctx* ctx = (struct appctx*)arg;
    switch(e) {
        case SM_ENTER: {
            log_debug("AC:START");
            // Stop all leds, and flash slow to show we're in console... this is for debug only
            ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_MIN, -1);
            ledStart(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_MIN, -1);
            // exit by timer if no at commands received
            sm_timer_start(ctx->mySMId, 30*1000);

            // activate console on the uart (if configured). 
            if (startConsole()==false) {
                // start directly
                sm_sendEvent(ctx->mySMId, ME_FORCE_UL, NULL);
            }
            return SM_STATE_CURRENT;
        }
        case SM_EXIT: {
            stopConsole();
            // LEDs off
            ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
            ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
            return SM_STATE_CURRENT;
        }
        case SM_TIMEOUT: {
            // Only do the loop if console not active, else must do AT+RUN
            if (isConsoleActive()) {
                log_debug("AC : console active stays in startup mode");
                return SM_STATE_CURRENT;
            } 
            // Starts by running the aquisitiion
            return MS_GETTING_SERIAL_MODS;
        }
        case ME_FORCE_UL: {
            return MS_GETTING_SERIAL_MODS;
        }

        default: {
            log_debug("AC:unknown %d in Startup", e);
            return SM_STATE_CURRENT;
        }
    }
    assert(0);      // shouldn't get here
}
static SM_STATE_ID_t State_Idle(void* arg, int e, void* data) {
    struct appctx* ctx = (struct appctx*)arg;
    switch(e) {
        case SM_ENTER: {
            if (ctx->doReboot) {
                log_debug("AC:enter idle and reboot pending... bye bye....");
                RMMgr_reboot(RM_DM_ACTION);
                // Fall out just in case didn't actually reboot...
                log_error("AC: should have rebooted!");
                assert(0);
            }
            //Initialise the DM we're sending next time -> this means executed actions can start to fill it during idle time
            app_core_msg_ul_init(&ctx->txmsg);
            ctx->ulIsCrit = false;       // assume we're not gonna send it (its not critical)
            if (ctx->idleTimeMovingSecs==0) {
                // no idleness, this device runs continuously... (eg if its powered)
                log_debug("AC:no idle");
                sm_sendEvent(ctx->mySMId, ME_FORCE_UL, NULL);
                return SM_STATE_CURRENT;
            }
            // Start the wakeup timeout (every 60s we wake to check for movement) 
            sm_timer_start(ctx->mySMId, ctx->idleTimeCheckSecs*1000);
            log_debug("AC:idle %d secs", ctx->idleTimeCheckSecs);
            // Record time
            ctx->idleStartTS = TMMgr_getRelTime();
            // activate console on the uart (if configured). 
            if (startConsole()) {
                // Stop all leds, and flash slow to show we're in console
                ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_MIN, -1);
                ledStart(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_MIN, -1);
                // Set desired low power mode to be just sleep as console is active for first period
                LPMgr_setLPMode(LP_SLEEP);
                    // start timer for specific event to turn off console if not activated
                sm_timer_startE(ctx->mySMId, 10000, ME_CONSOLE_TIMEOUT);
            } else {
                // LEDs off, we are sleeping
                ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
                ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
                // and stay idle in deep sleep this time
                LPMgr_setLPMode(LP_DEEPSLEEP);
            }
            return SM_STATE_CURRENT;
        }
        case SM_EXIT: {
            stopConsole();
            // LEDs off
            ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
            ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
            // any low power when not idle will be basic low power MCU (ie with radio and gpios on)
            LPMgr_setLPMode(LP_DOZE);
            return SM_STATE_CURRENT;
        }
        case SM_TIMEOUT: {
            // Only do the loop if console not active, else set timer again
            if (isConsoleActive()) {
                log_debug("AC : console active not doing work");
                sm_timer_start(ctx->mySMId, ctx->idleTimeMovingSecs*1000);
                return SM_STATE_CURRENT;
            } 
            // timeout -> did we move? deal with difference between moving and not moving times
            uint32_t idletimeMS = ctx->idleTimeNotMovingMins * 60000;
            // check if has moved recently and use different timeout
            if (MMMgr_hasMovedSince(ctx->lastULTime)) {
                idletimeMS = ctx->idleTimeMovingSecs*1000;
            }
            idletimeMS -= 1000;     // adjust by 1s to get run if 'close' to timeout
            uint32_t dt = TMMgr_getRelTime() - ctx->idleStartTS;
            if (dt >= idletimeMS) {
                return MS_GETTING_SERIAL_MODS;
            }
            // else stay here. reset timeout for next check
            sm_timer_start(ctx->mySMId, ctx->idleTimeCheckSecs*1000);
            log_debug("AC:reidle %d secs as been idle for %d not more than the limit %d", ctx->idleTimeCheckSecs, dt, idletimeMS);
            // Call any module's tic cbs
            for(int i=0;i<ctx->nMods;i++) {
                if (isModActive(ctx->modsMask, ctx->mods[i].id)) {
                    // Call if defined
                    if (ctx->mods[i].api->ticCB!=NULL) {
                        (*(ctx->mods[i].api->ticCB))();
                    }
                }
            }
            log_debug("called tics");

            return SM_STATE_CURRENT;
        }
        // you only get Xs to use console then goes to sleep properly
        case ME_CONSOLE_TIMEOUT: {
            if (!isConsoleActive()) {
                log_debug("AC: console timeout");
                // No more uarting
                stopConsole();
                // LEDs off
                ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
                ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
                // and stay idle in deep sleep this time
                LPMgr_setLPMode(LP_DEEPSLEEP);
            } // else console in use, so user must do explicit exit by AT+RUN or wait for global timeout after inactivity
            return SM_STATE_CURRENT;
        }
        case ME_FORCE_UL: {
            // TODO deal with request to only run 1 module....
            return MS_GETTING_SERIAL_MODS;
        }
        case ME_LORA_RX: {
            if (data!=NULL) {
                executeDL(ctx, (APP_CORE_DL_t*)data);
            }
            return SM_STATE_CURRENT;
        }
        default: {
            log_debug("AC:unknown %d in Idle", e);
            return SM_STATE_CURRENT;
        }
    }
    assert(0);      // shouldn't get here
}
// Get all the modules that require to be executed in series
static SM_STATE_ID_t State_GettingSerialMods(void* arg, int e, void* data) {
    struct appctx* ctx = (struct appctx*)arg;

    switch(e) {
        case SM_ENTER: {
            ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_2HZ, -1);
            // find first serial guy by sending ourselves the done event with idx=-1
            ctx->currentSerialModIdx = -1;
            sm_sendEvent(ctx->mySMId, ME_MODULE_DONE, NULL);
            return SM_STATE_CURRENT;
        }
        case SM_EXIT: {
            ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
            // Get data from active modules to build UL message
            return SM_STATE_CURRENT;
        }
        case SM_TIMEOUT: {
            // Timeout is same as saying its done
            if (ctx->currentSerialModIdx<0) {
                // no mod running, move on
                return MS_GETTING_PARALLEL_MODS;
            }
            // drop thru with data set to id of running guy
            data = (void*)(ctx->mods[ctx->currentSerialModIdx].id);
            // !! DROP THRU INTENTIONAL !!
        }
        case ME_MODULE_DONE: {
            // get the id of the module what is done (the ID is the 'data' param's value)
            // check its the one we're waiting for (if not first time)
            if (ctx->currentSerialModIdx > 0) {
                if ((int)data == ctx->mods[ctx->currentSerialModIdx].id) {
    //                log_debug("done Smod %d, id %d ", ctx->currentSerialModIdx, (int)data);
                    // Get the data
                    ctx->ulIsCrit |= (*(ctx->mods[ctx->currentSerialModIdx].api->getULDataCB))(&ctx->txmsg);
                    // stop any activity
                    (*(ctx->mods[ctx->currentSerialModIdx].api->stopCB))();
                } else {
                    log_warn("AC:Smod %d done but not active [%d]", (int)data, ctx->mods[ctx->currentSerialModIdx].id);
                    // this should not happen!
                    return MS_GETTING_PARALLEL_MODS;
                }
            }
            // Find next serial one to run or goto parallels if all done
            for(int i=(ctx->currentSerialModIdx+1);i<ctx->nMods;i++) {
                if (isModActive(ctx->modsMask, ctx->mods[i].id)) {
                    if (ctx->mods[i].exec == EXEC_SERIAL) {
                        ctx->currentSerialModIdx = i;
                        uint32_t timeReqd = (*(ctx->mods[i].api->startCB))();
                        // May return 0, which means no need for this module to run this time (no UL data)
                        if (timeReqd!=0) {
                            // start timeout for current mod to get their data
                            sm_timer_start(ctx->mySMId, timeReqd);
                            log_debug("AC:Smod [%d] for %d ms", ctx->mods[i].id, timeReqd);
                            return SM_STATE_CURRENT;
                        } else {
                            log_debug("AC:Smod [%d] says not this cycle", ctx->mods[i].id);
                        }
                    }
                }
            }
            // No more serial guys, go to parallels
            return MS_GETTING_PARALLEL_MODS;
        }
        default: {
            log_debug("AC:unknown %d in GettingSerialMods", e);
            return SM_STATE_CURRENT;
        }
    }
    assert(0);      // shouldn't get here
}
// Get all the modules that allow to be executed in parallel
static SM_STATE_ID_t State_GettingParallelMods(void* arg, int e, void* data) {
    struct appctx* ctx = (struct appctx*)arg;

    switch(e) {
        case SM_ENTER: {
            ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_2HZ, -1);
            uint32_t modtime = 0;
            // and tell mods to go for max timeout they require
            for(int i=0;i<ctx->nMods;i++) {
//                log_debug("Pmod %d for mask %x", ctx->mods[i].id,  ctx->modsMask[0]);
                if (isModActive(ctx->modsMask, ctx->mods[i].id)) {
                    if (ctx->mods[i].exec==EXEC_PARALLEL) {
                        uint32_t timeReqd = (*(ctx->mods[i].api->startCB))();
                        if (timeReqd > modtime) {
                            modtime = timeReqd;
                        }
                    }
                }
            }
            if (modtime>0) {
                // start timeout for current mods to get their data (short time)
                log_debug("AC:pmod for %d ms", modtime);
                sm_timer_start(ctx->mySMId, modtime);
            } else {
                // no parallel mods, timeout now
                log_debug("AC:no Pmods to check");
                sm_sendEvent(ctx->mySMId, SM_TIMEOUT, NULL);
            }
            return SM_STATE_CURRENT;
        }
        case SM_EXIT: {
            ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
            return SM_STATE_CURRENT;
        }
        case SM_TIMEOUT: {
            // Get data from active modules to build UL message
            // Note that to mediate between modules that use same IOs eg I2C or UART (with UART selector)
            // they should be "serial" type not parallel            
            for(int i=0;i<ctx->nMods;i++) {
                if (isModActive(ctx->modsMask, ctx->mods[i].id)) {
                    if (ctx->mods[i].exec==EXEC_PARALLEL) {
                        // Get the data, and set the flag if module says the ul MUST be sent
                        ctx->ulIsCrit |= (*(ctx->mods[i].api->getULDataCB))(&ctx->txmsg);
                        // stop any activity
                        (*(ctx->mods[i].api->stopCB))();
                    }
                }
            }
            // critical to send it if been a while since last one
            ctx->ulIsCrit |= ((TMMgr_getRelTime() - ctx->lastULTime) > (ctx->maxTimeBetweenULMins*60000));
            if (ctx->ulIsCrit) {
                return MS_SENDING_UL;
            } else {            
                return MS_IDLE;
            }
        }

        default: {
            log_debug("AC:unknown %d in GettingParallelMods", e);
            return SM_STATE_CURRENT;
        }
    }
    assert(0);      // shouldn't get here
}
static LORA_TX_RESULT_t tryTX(APP_CORE_UL_t* txmsg, uint8_t dlid, bool willListen) {
    uint8_t txsz = app_core_msg_ul_finalise(txmsg, dlid, willListen);
    LORA_TX_RESULT_t res = LORA_TX_ERR_RETRY;
    if (txsz>0) {
        res = lora_app_tx(&(txmsg->msgs[txmsg->msbNbTxing].payload[0]), txsz, 8000);
    } else {
        log_info("AC:no UL finalise said %d", txsz);
        res = LORA_TX_NO_TX;    // not an actual error but no tx so no point waiting for result
    }
    return res;
}
static SM_STATE_ID_t State_SendingUL(void* arg, int e, void* data) {
    struct appctx* ctx = (struct appctx*)arg;

    switch(e) {
        case SM_ENTER: {
            log_debug("AC:trying to send UL");
            // start leds for UL
            ledStart(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_5HZ, -1);
            LORA_TX_RESULT_t res = tryTX(&ctx->txmsg, ctx->lastDLId, true);
            if (res==LORA_TX_OK) {
                // this timer should be bigger than the one we give to the tx above... its a belt and braces job
                sm_timer_start(ctx->mySMId,  25000);
                // ok wait for result
            } else {
                // want to abort immediately... send myself a result event
                log_warn("AC:send UL tx failed %d", res);
                sm_sendEvent(ctx->mySMId, ME_LORA_RESULT, (void*)res);    // pass the code as the value not as a pointer
            }
            return SM_STATE_CURRENT;
        }
        case SM_EXIT: {
            ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
            return SM_STATE_CURRENT;
        }
        case SM_TIMEOUT: {
            // Done , go idle
            log_info("AC:done with UL send due to SM timeout");
            return MS_IDLE;
        }
        case ME_LORA_RX: {
            if (data!=NULL) {
                APP_CORE_DL_t* rxmsg = (APP_CORE_DL_t*)data;
                // Execute actions if the dlid is not the last one we did
                if (rxmsg->dlId!=ctx->lastDLId) {
                    executeDL(ctx, rxmsg);
                } else {
                    log_debug("AC:ignore DL actions as id same as last time (%d)", rxmsg->dlId);
                }
            }
            return SM_STATE_CURRENT;
        }

        case ME_LORA_RESULT: {
            // Default is an error on tx which sent us this event... and the value is passed
            LORA_TX_RESULT_t res = ((LORA_TX_RESULT_t)data);
            switch (res) {
                case LORA_TX_OK_ACKD: {
                    log_info("AC:lora tx is ACKD, check next");
                    ctx->lastULTime = TMMgr_getRelTime();
                    break;
                }
                case LORA_TX_OK: {
                    log_info("AC:lora tx is OK, check next");
                    ctx->lastULTime = TMMgr_getRelTime();
                    break;
                }
                case LORA_TX_ERR_NOTJOIN: {
                    // Retry join here? change the SF? TODO
                    log_warn("AC:lora tx fails as JOIN fail, going idle");
                    return MS_IDLE;
                }
                case LORA_TX_ERR_FATAL: {
                    log_error("AC:lora tx result is FATAL ERROR, assert!");
                    assert(0);
                    return MS_IDLE;
                }
                default: {
                    log_warn("AC:lora tx result %d not cased", res);
                    break; // fall out
                }
            }
            // See if we have another mssg to go. Note we say 'not listening' ie don't send me DL as we haven't 
            // processing any RX yet, so our 'lastDLId' is not up to date and we'll get a repeated action DL!
            res = tryTX(&ctx->txmsg, ctx->lastDLId, false);
            if (res==LORA_TX_OK) {
                // this timer should be bigger than the one we give to the tx above... its a belt and braces job
                sm_timer_start(ctx->mySMId,  25000);
                // ok wait for result
                return SM_STATE_CURRENT;
            } else {
                log_debug("AC:lora tx UL res %d, going idle", res);
                // And we're done
                return MS_IDLE;
            }
        }
        default: {
            log_debug("AC:unknown %d in SendingUL", e);
            return SM_STATE_CURRENT;
        }
    }
    assert(0);      // shouldn't get here
}
// State table : note can be in any order as the 'id' field is what maps the state id to the rest
static SM_STATE_t _mySM[MS_LAST] = {
    {.id=MS_STARTUP,       .name="Startup",   .fn=State_Startup},
    {.id=MS_IDLE,           .name="Idle",       .fn=State_Idle},
    {.id=MS_GETTING_SERIAL_MODS,    .name="GettingSerialMods", .fn=State_GettingSerialMods},
    {.id=MS_GETTING_PARALLEL_MODS,    .name="GettingParallelMods", .fn=State_GettingParallelMods},
    {.id=MS_SENDING_UL,     .name="SendingUL",  .fn=State_SendingUL},    
};

// Called to start the action, after all sysinit done
void app_core_start() {
    log_debug("AC:init");
    CFMgr_getOrAddElement(CFG_UTIL_KEY_IDLE_TIME_MOVING_SECS, &_ctx.idleTimeMovingSecs, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_IDLE_TIME_NOTMOVING_MINS, &_ctx.idleTimeNotMovingMins, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_IDLE_TIME_CHECK_SECS, &_ctx.idleTimeCheckSecs, sizeof(uint32_t));
    memset(&_ctx.modsMask[0], 0xff, MOD_MASK_SZ);       // Default every module is active
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MODS_ACTIVE_MASK, &_ctx.modsMask[0], MOD_MASK_SZ);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MAXTIME_UL_MINS, &_ctx.maxTimeBetweenULMins, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_DL_ID, &_ctx.lastDLId, sizeof(uint8_t));
    CFMgr_registerCB(configChangedCB);      // For changes to our config

    registerActions();

    // deveui, and other lora setup config are in PROM
    lora_app_init(lora_tx_cb, lora_rx_cb);

    // initialise console for use during idle periods if enabled
    if (MYNEWT_VAL(WCONSOLE_ENABLED)!=0) {
        initConsole();
    }

    // Do modules immediately on boot by starting in post-idle state
    _ctx.mySMId = sm_init("app-core", _mySM, MS_LAST, MS_STARTUP, &_ctx);
    sm_start(_ctx.mySMId);
}

// core api for modules
// mcbs pointer must be to a static structure
void AppCore_registerModule(APP_MOD_ID_t id, APP_CORE_API_t* mcbs, APP_MOD_EXEC_t execType) {
    assert(id < APP_MOD_LAST);
    assert(_ctx.nMods<MAX_MODS);
    assert(mcbs!=NULL);
    assert(mcbs->startCB != NULL);
    assert(mcbs->stopCB != NULL);
    assert(mcbs->getULDataCB != NULL);
    
    _ctx.mods[_ctx.nMods].api = mcbs;
    _ctx.mods[_ctx.nMods].id = id;
    _ctx.mods[_ctx.nMods].exec = execType;
    _ctx.nMods++;
    log_debug("AC: add [%d] exec[%d]", id, execType);
}

// is module active?
bool AppCore_getModuleState(APP_MOD_ID_t mid) {
    return isModActive(_ctx.modsMask, mid);     // This is protected against bad mid values
}
// set module active/not active
void AppCore_setModuleState(APP_MOD_ID_t mid, bool active) {
    if (mid>=0 && mid<APP_MOD_LAST) {
        if (active) {
            // set the bit
            _ctx.modsMask[mid/8] |= (1<<(mid%8));
        } else {
            // clear the bit
            _ctx.modsMask[mid/8] &= ~(1<<(mid%8));
        }
        // And writeback to PROM
        CFMgr_setElement(CFG_UTIL_KEY_MODS_ACTIVE_MASK, &_ctx.modsMask[0], MOD_MASK_SZ);
    }
}

// register a DL action handler
// Note asserts if id already registered, or table is full
void AppCore_registerAction(uint8_t id, ACTIONFN_t cb) {
    assert(_ctx.nActions<MAX_DL_ACTIONS);
    assert(cb!=NULL);
    // Check noone else has registerd this
    for(int i=0;i<_ctx.nActions;i++) {
        if (_ctx.actions[i].id==id) {
            assert(0);
        }
    }
    _ctx.actions[_ctx.nActions].id = id;
    _ctx.actions[_ctx.nActions].fn = cb;
    _ctx.nActions++;
    log_debug("AC: reg action [%d]", id);
}
// Find action fn or NULL
ACTIONFN_t AppCore_findAction(uint8_t id) {
    for(int i=0;i<_ctx.nActions;i++) {
        if (_ctx.actions[i].id==id) {
            return _ctx.actions[i].fn;
        }
    }
    return NULL;
}

// Last UL sent time (relative, ms)
uint32_t AppCore_lastULTime() {
    return _ctx.lastULTime;
}
// Time in ms
uint32_t AppCore_getTimeToNextUL() {
    return TMMgr_getRelTime() - _ctx.idleStartTS;
}
// Request stop idle and goto UL phase now
// optionally request 'fast' UL ie just 1 module to let do data collection
// TODO (required for fast button UL sending)
bool AppCore_forceUL(int reqModule) {
    void* ed = NULL;
    if (reqModule>=0 && reqModule<APP_MOD_LAST) {
        ed = (void*)(1000+reqModule); // add 1000 as if 0 then same as NULL....
    }
    return sm_sendEvent(_ctx.mySMId, ME_FORCE_UL, ed);
}

// Tell core we're done processing
void AppCore_module_done(APP_MOD_ID_t id) {
    sm_sendEvent(_ctx.mySMId, ME_MODULE_DONE, (void*)id);
}

// Internals : action handling
static void executeDL(struct appctx* ctx, APP_CORE_DL_t* data) {
    if (app_core_msg_dl_execute(data)) {
        log_info("AC: exec DL ok id now %d", data->dlId);
        // Can update last dl id since we did its actions
        ctx->lastDLId = data->dlId;
        // Store in case we reboot
        CFMgr_setElement(CFG_UTIL_KEY_DL_ID, &ctx->lastDLId, sizeof(uint8_t));
    } else {
        log_warn("AC: DL not exec OK");
    }
}

// helper to read LE from buffer (may be 0 stripped)
uint32_t readUINT32LE(uint8_t* b, uint8_t l) {
    uint32_t ret = 0;
    for(int i=0;i<4;i++) {
        if (b!=NULL && i<l) {
            ret += (b[i] << 8*i);
        } // else 0
    }
    return ret;
}
uint16_t readUINT16LE(uint8_t* b, uint8_t l) {
    uint16_t ret = 0;
    for(int i=0;i<2;i++) {
        if (b!=NULL && i<l) {
            ret += (b[i] << 8*i);
        } // else 0
    }
    return ret;
}
static void A_reboot(uint8_t* v, uint8_t l) {
    log_info("action REBOOT");
    // must wait till execute actions finished
    _ctx.doReboot = true;
}

static void A_setConfig(uint8_t* v, uint8_t l) {
    if (l<3) {
        log_warn("AC:action SETCONFIG BAD (too short value)");
    }
    // value is 2 bytes config id, l-2 bytes value to set
    uint16_t key = readUINT16LE(v, 2);
    // Check if length is as the config key is already expecting
    uint8_t exlen = CFMgr_getElementLen(key);
    if (exlen==(l-2)) {
        if (CFMgr_setElement(key, v+2, l-2)) {
            log_info("AC:action SETCONFIG (%d) to %d len value OK", key, l-2);
        } else {
            log_warn("AC:action SETCONFIG (%d) to %d len value FAILS", key, l-2);
        }
    } else {
        log_warn("AC:action SETCONFIG (%d) to %d len value FAILS as expected len is ", key, (l-2), exlen);
    }
}

static void A_getConfig(uint8_t* v, uint8_t l) {
    if (l<2) {
        log_warn("AC:action GETCONFIG BAD (too short value)");
    }
    // value is 2 bytes config id
    uint16_t key = readUINT16LE(v, 2);
    uint8_t vb[16];     // only allow to get keys up to 16 bytes..
    int cl = CFMgr_getElement(key, vb, 16);
    if (cl>0) {
        log_info("AC:action GETCONFIG (%d) value [");
        for(int i=0;i<cl;i++) {
            log_info("%02x", vb[i]);
        }
        // Allowed to add to UL during action execution
        if (app_core_msg_ul_addTLV(&_ctx.txmsg, APP_CORE_UL_CONFIG, cl, vb)) {
            log_info("] added to UL");
        } else {
            log_warn("] could not be added to UL");
        }
    } else {
            log_warn("AC:action GETCONFIG (%d) no such key");
    }
}
static void A_fota(uint8_t* v, uint8_t l) {
    log_info("AC:action FOTA (TBI)");
}
static void A_settime(uint8_t* v, uint8_t l) {
    log_info("AC:action SETTIME");
    uint32_t now = readUINT32LE(v, l);
    // boot time in UTC is now - time elapsed since boot
    TMMgr_setBootTime(now - TMMgr_getRelTime());
}

static void A_getmods(uint8_t* v, uint8_t l) {
    log_info("AC:action FOTA (TBI)");    
}

static void registerActions() {
    AppCore_registerAction(APP_CORE_DL_REBOOT, &A_reboot);
    AppCore_registerAction(APP_CORE_DL_SET_CONFIG, &A_setConfig);
    AppCore_registerAction(APP_CORE_DL_GET_CONFIG, &A_getConfig);
    AppCore_registerAction(APP_CORE_DL_SET_UTCTIME, &A_settime);
    AppCore_registerAction(APP_CORE_DL_FOTA, &A_fota);
    AppCore_registerAction(APP_CORE_DL_GET_MODS, &A_getmods);
}
