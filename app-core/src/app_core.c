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
#include "wyres-generic/sm_exec.h"
#include "wyres-generic/lowpowermgr.h"
#include "wyres-generic/loraapp.h"
#include "wyres-generic/timemgr.h"

#include "app-core/app_core.h"
#include "app-core/app_msg.h"


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
    uint32_t idleTimeNotMovingSecs;
    uint32_t modSetupTimeSecs;
    uint32_t idleStartTS;
    uint32_t maxTimeBetweenULSecs;
} _ctx = {
    .nMods=0,
    .idleTimeMovingSecs=5*60,           // 5mins
    .idleTimeNotMovingSecs=120*60,      // 2 hours
    .modSetupTimeSecs=3,
    .maxTimeBetweenULSecs=15*60,    // 15 mins
};


// Callback from config mgr when the actived modules mask changes
static void configChangedCB(uint16_t key) {
    // just re-get all my config in case it changed
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MODS_ACTIVE_MASK, &_ctx.modsMask[0], MOD_MASK_SZ);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_IDLE_TIME_MOVING_SECS, &_ctx.idleTimeMovingSecs, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_IDLE_TIME_NOTMOVING_SECS, &_ctx.idleTimeNotMovingSecs, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MODSETUP_TIME_SECS, &_ctx.modSetupTimeSecs, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MAXTIME_UL_SECS, &_ctx.maxTimeBetweenULSecs, sizeof(uint32_t));

}
static bool isModActive(uint8_t* mask, APP_MOD_ID_t id) {
    if (id<0 || id>=APP_MOD_LAST) {
        return false;
    }
    return ((mask[id/8] & (1<<(id%8)))!=0);
}
// application core state machine
// Define my state ids
enum MyStates { MS_IDLE, MS_GETTING_SERIAL_MODS, MS_GETTING_PARALLEL_MODS, MS_SENDING_UL, MS_LAST };
enum MyEvents { ME_MODS_OK, ME_MODULE_DONE, ME_FORCE_UL, ME_LORA_RESULT, ME_LORA_RX };
// related fns

static void lora_tx_cb(LORA_TX_RESULT_t res) {
//    log_debug("lora tx cb : result:%d", res);
    // pass tx result directly as value as var 'res' may not be around when SM is run....
    sm_sendEvent(_ctx.mySMId, ME_LORA_RESULT, (void*)res);
}

static void lora_rx_cb(uint8_t port, void* data, uint8_t sz) {
    // Use static message buffer in ctx as sendEvent is executed off this thread -> can't use stack var.
    if (app_core_msg_dl_decode(&_ctx.rxmsg)) {
        log_debug("lora rx type %d", _ctx.rxmsg.type);
        sm_sendEvent(_ctx.mySMId, ME_LORA_RX, (void*)(&_ctx.rxmsg));
    } else {
        log_debug("lora rx BAD DECODE sz %d", sz);
    }
}
// SM state functions

// ADD state for startup init, join+syncreq, etc before becoming idle
static SM_STATE_ID_t State_Idle(void* arg, int e, void* data) {
    struct appctx* ctx = (struct appctx*)arg;
    switch(e) {
        case SM_ENTER: {
            log_debug("idle %d s", ctx->idleTimeMovingSecs);
            // Stop all leds, and flash slow to show we're in sleep... this is for debug only
            ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_MIN, -1);
            ledStart(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_MIN, -1);
            // Set desired low power mode to be DEEP so it knows when mynewt says to sleep
            LPMgr_setLPMode(LP_DEEPSLEEP);
            // Start the wakeup timeout TODO check if moved recently and use different timer
            sm_timer_start(ctx->mySMId, ctx->idleTimeMovingSecs*1000);
            // Record time
            ctx->idleStartTS = TMMgr_getRelTime();
            return SM_STATE_CURRENT;
        }
        case SM_EXIT: {
            // LEDs off
            ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
            ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
            // any low power when not idle will be basic sleeping (ie with radio and gpios on)
            LPMgr_setLPMode(LP_SLEEP);
            //Initialise the DM we're sending
            app_core_msg_ul_init(&ctx->txmsg);
            ctx->ulIsCrit = false;       // assume we're not gonna send it (its not critical)
            return SM_STATE_CURRENT;
        }
        case SM_TIMEOUT: {
            // timeout -> did we move? deal with difference between moving and not moving times TODO
            return MS_GETTING_SERIAL_MODS;
        }
        case ME_FORCE_UL: {
            return MS_GETTING_SERIAL_MODS;
        }
        case ME_LORA_RX: {
            if (data!=NULL) {
                if (app_core_msg_dl_execute((APP_CORE_DL_t*)data)) {
                    log_debug("executed DL ok");
                } else {
                    log_debug("some elements of DL not executed");
                }
            }
            return SM_STATE_CURRENT;
        }
        default: {
            log_debug("unknown event %d in state Idle", e);
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
            // find first serial guy
            for(int i=0;i<ctx->nMods;i++) {
//                log_debug("Smod %d mask %x", ctx->mods[i].id,  ctx->modsMask[0]);
                if (isModActive(ctx->modsMask, ctx->mods[i].id)) {
                    if (ctx->mods[i].exec == EXEC_SERIAL) {
                        ctx->currentSerialModIdx = i;
                        uint32_t timeReqd = (*(ctx->mods[i].api->startCB))();
                        // start timeout for current mod to get their data (short time)
                        sm_timer_start(ctx->mySMId, timeReqd);
                        log_debug("Smod [%d] for %d ms", ctx->mods[i].id, timeReqd);
                        return SM_STATE_CURRENT;
                    }
                }
            }
            log_debug("no SMods");
            // No serial mods, force change to parallel ones
            ctx->currentSerialModIdx = -1;
            sm_sendEvent(ctx->mySMId, SM_TIMEOUT, NULL);
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
        }
        case ME_MODULE_DONE: {
            // get the id of the module what is done
            // check its the one we're waiting for
            if ((int)data == ctx->mods[ctx->currentSerialModIdx].id) {
//                log_debug("done Smod %d, id %d ", ctx->currentSerialModIdx, (int)data);
                // Get the data
                ctx->ulIsCrit |= (*(ctx->mods[ctx->currentSerialModIdx].api->getULDataCB))(&ctx->txmsg);
                // stop any activity
                (*(ctx->mods[ctx->currentSerialModIdx].api->stopCB))();
                // Find next serial one
                for(int i=(ctx->currentSerialModIdx+1);i<ctx->nMods;i++) {
                    if (isModActive(ctx->modsMask, ctx->mods[i].id)) {
                        if (ctx->mods[i].exec == EXEC_SERIAL) {
                            ctx->currentSerialModIdx = i;
                            uint32_t timeReqd = (*(ctx->mods[i].api->startCB))();
                            // start timeout for current mod to get their data
                            sm_timer_start(ctx->mySMId, timeReqd);
                            log_debug("Smod [%d] for %d ms", ctx->mods[i].id, timeReqd);
                            return SM_STATE_CURRENT;
                        }
                    }
                }
                // No more serial guys, go to parallels
                return MS_GETTING_PARALLEL_MODS;
            } else {
                log_debug("Smod %d done but not active [%d]", (int)data, ctx->mods[ctx->currentSerialModIdx].id);
            }
        }
        default: {
            log_debug("unknown event %d in state getting serial mods", e);
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
                log_debug("pmod for %d ms", modtime);
                sm_timer_start(ctx->mySMId, modtime);
            } else {
                // no parallel mods, timeout now
                log_debug("no Pmods to check");
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
            // !! how to mediate between modules that use same IOs eg I2C or UART (with UART selector)
            // The accessed resource MUST provide a mutex type access control
            
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
            ctx->ulIsCrit |= ((TMMgr_getRelTime() - ctx->lastULTime) > (ctx->maxTimeBetweenULSecs*1000));
            if (ctx->ulIsCrit) {
                return MS_SENDING_UL;
            } else {            
                return MS_IDLE;
            }
        }

        default: {
            log_debug("unknown event %d in state parallel mods", e);
            return SM_STATE_CURRENT;
        }
    }
    assert(0);      // shouldn't get here
}

static SM_STATE_ID_t State_SendingUL(void* arg, int e, void* data) {
    struct appctx* ctx = (struct appctx*)arg;

    switch(e) {
        case SM_ENTER: {
            log_debug("trying to send UL");
            // start leds for UL
            ledStart(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_5HZ, -1);
            uint8_t txsz = app_core_msg_ul_finalise(&ctx->txmsg);
            LORA_TX_RESULT_t res = LORA_TX_ERR_RETRY;
            if (txsz>0) {
                res = lora_app_tx(ctx->txmsg.payload, txsz, 8000);
            } else {
                log_debug("not sending UL as finalise said bad %d", txsz);
            }
            if (res==LORA_TX_OK) {
                // this timer should be bigger than the one we give to the tx above... its a belt and braces job
                sm_timer_start(ctx->mySMId,  25000);
                // ok wait for result
            } else {
                // want to abort immediately... send myself a result (think this will work...)
                log_debug("trying to send UL tx failed immediatrly...");
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
            log_debug("done with UL send due to SM timeout");
            return MS_IDLE;
        }
        case ME_LORA_RX: {
            if (data!=NULL) {
                if (app_core_msg_dl_execute((APP_CORE_DL_t*)data)) {
                    log_debug("executed DL ok");
                } else {
                    log_debug("some elements of DL not executed");
                }
            }
            return SM_STATE_CURRENT;
        }

        case ME_LORA_RESULT: {
            // Default is an error on tx which sent us this event... and the value is passed
            LORA_TX_RESULT_t res = ((LORA_TX_RESULT_t)data);
            switch (res) {
                case LORA_TX_OK_ACKD: {
                    log_debug("lora tx is ACKD, going idle");
                    ctx->lastULTime = TMMgr_getRelTime();
                    return MS_IDLE;
                }
                case LORA_TX_OK: {
                    log_debug("lora tx is OK, going idle");
                    ctx->lastULTime = TMMgr_getRelTime();
                    return MS_IDLE;
                }
                case LORA_TX_ERR_NOTJOIN: {
                    // Retry join here? change the SF? TODO
                    log_debug("lora tx fails as JOIN fail, going idle");
                    return MS_IDLE;
                }
                case LORA_TX_ERR_FATAL: {
                    log_debug("lora tx result is FATAL ERROR, assert!");
                    assert(0);
                    return MS_IDLE;
                }
                default: {
                    break; // fall out
                }
            }
            log_debug("lora tx result is %d, going idle", res);
            // And we're done
            return MS_IDLE;
        }
        default: {
            log_debug("unknown event %d in state sending DM", e);
            return SM_STATE_CURRENT;
        }
    }
    assert(0);      // shouldn't get here
}
// State table : note can be in any order as the 'id' field is what maps the state id to the rest
static SM_STATE_t _mySM[MS_LAST] = {
    {.id=MS_IDLE,           .name="Idle",       .fn=State_Idle},
    {.id=MS_GETTING_SERIAL_MODS,    .name="GettingSerialMods", .fn=State_GettingSerialMods},
    {.id=MS_GETTING_PARALLEL_MODS,    .name="GettingParallelMods", .fn=State_GettingParallelMods},
    {.id=MS_SENDING_UL,     .name="SendingUL",  .fn=State_SendingUL},    
};

// Called to start the action, after all sysinit done
void app_core_start() {
    log_debug("init app-core");
    CFMgr_getOrAddElement(CFG_UTIL_KEY_IDLE_TIME_MOVING_SECS, &_ctx.idleTimeMovingSecs, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_IDLE_TIME_NOTMOVING_SECS, &_ctx.idleTimeNotMovingSecs, sizeof(uint32_t));
    memset(&_ctx.modsMask[0], 0xff, MOD_MASK_SZ);       // Default every module is active
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MODS_ACTIVE_MASK, &_ctx.modsMask[0], MOD_MASK_SZ);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MAXTIME_UL_SECS, &_ctx.maxTimeBetweenULSecs, sizeof(uint32_t));
    CFMgr_registerCB(configChangedCB);      // For changes to our config

    // Can set config before init, then it all gets set once init done
    lora_app_setAck(true);      // we like acks in this test program
    lora_app_setAdr(false);      // but not adr coz we're mobile
    lora_app_setTxPort(LORAWAN_UL_PORT);      
    lora_app_setRxPort(LORAWAN_DL_PORT);      
    lora_app_setTxPower(14);      // go max
    lora_app_setDR(2);          // go SF10
    // deveui etc are in PROM
    lora_app_init(lora_tx_cb, lora_rx_cb);

    // Do modules immediately on boot by starting in post-idle state
    _ctx.mySMId = sm_init("app-core", _mySM, MS_LAST, MS_IDLE, &_ctx);
    sm_start(_ctx.mySMId);
    // Do UL immediately on boot
    AppCore_forceUL();
}

// core api for modules
// mcbs pointer must be to a static structure
bool AppCore_registerModule(APP_MOD_ID_t id, APP_CORE_API_t* mcbs, APP_MOD_EXEC_t execType) {
    assert(id < APP_MOD_LAST);
    assert(_ctx.nMods<MAX_MODS);

    _ctx.mods[_ctx.nMods].api = mcbs;
    _ctx.mods[_ctx.nMods].id = id;
    _ctx.mods[_ctx.nMods].exec = execType;
    _ctx.nMods++;
    log_debug("a-c add [%d] exec[%d]", id, execType);
    return true;
}

// Last UL sent time (relative, ms)
uint32_t AppCore_lastULTime() {
    return _ctx.lastULTime;
}
// Time in ms
uint32_t AppCore_getTimeToNextUL() {
    return TMMgr_getRelTime() - _ctx.idleStartTS;
}
bool AppCore_forceUL() {
    return sm_sendEvent(_ctx.mySMId, ME_FORCE_UL, NULL);
}

// Tell core we're done processing
void AppCore_module_done(APP_MOD_ID_t id) {
    sm_sendEvent(_ctx.mySMId, ME_MODULE_DONE, (void*)id);
}

