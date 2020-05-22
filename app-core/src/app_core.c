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
#include "wyres-generic/timemgr.h"
#include "wyres-generic/rebootmgr.h"
#include "wyres-generic/wconsole.h"

#include "loraapi/loraapi.h"

#include "app-core/app_console.h"
#include "app-core/app_core.h"
#include "app-core/app_msg.h"

//MYNEWT_VAL(APP_CORE_MAX_MODS) - fix max number of modules that may be defined in a specific target 
#define MAX_MODS (8)        
// Size of bit mask in bytes to contain all known modules
#define MOD_MASK_SZ ((APP_MOD_LAST / 8) + 1)
// The timeout before leaving UL sending state. Should be big enough to allow any DL to have arrived
#define UL_WAIT_DL_TIMEOUTMS (20000)
// Delay between deciding on stock mode and actually entering the deep sleep, during which leds are on to signal to user
#define STOCK_MODE_DELAY_SECS (5)

// State machine for core app
// COntext data
static struct appctx
{
    SM_ID_t mySMId;
    bool deviceConfigOk;
    uint8_t deviceActive;          // is this device inactive? (1=ACTIVE, 0=INACTIVE)
    uint8_t enableStateLeds;   // flash LEDs regularly (at each tic) to show active/inactive states? 0=NO, 1=YES
    uint8_t lpUserId;
    uint8_t nMods;
    struct
    {
        const char* name;
        APP_MOD_ID_t id;
        APP_MOD_EXEC_t exec;
        APP_CORE_API_t *api;
    } mods[MAX_MODS];              // registered modules api fns
    uint8_t modsMask[MOD_MASK_SZ]; // bit mask to indicate if module is active or not currently
    int currentSerialModIdx;
    int requestedModule; // If forced UL then it may request only one module is run
    bool ulIsCrit;       // during data collection, module can signal critical data change ie must send UL
    APP_CORE_UL_t txmsg; // for building UL messages
    APP_CORE_DL_t rxmsg; // for decoding DL messages
    uint32_t lastULTime; // timestamp of last uplink in seconds since boot
    uint32_t idleTimeMovingSecs;
    uint32_t idleTimeNotMovingMins;
    uint32_t idleTimeInactiveMins;
    uint32_t idleTimeCheckSecs;
    uint32_t modSetupTimeSecs;
    uint32_t idleStartTS; // In seconds since epoch
    uint32_t joinStartTS; // in seconds since epoch
    uint32_t maxTimeBetweenULMins;
    bool doReboot;
    uint8_t notStockMode;
    uint32_t joinTimeCheckSecs;
    uint32_t rejoinWaitMins;
    uint32_t rejoinWaitSecs;
    uint8_t nbJoinAttempts;
    uint8_t nActions;
    ACTION_t actions[MAX_DL_ACTIONS];
    // ul response id : holds the last DL id we received. Sent in each UL to inform backend we got its DLs. 0=not listening
    uint8_t lastDLId;
    struct loraapp_config
    {
        bool useAck;
        bool useAdr;
        uint8_t txPort;
        uint8_t loraSF;
        int8_t txPower;
        uint32_t txTimeoutMs;
        uint8_t deveui[8];
        uint8_t appeui[8];
        uint8_t appkey[16];
    } loraCfg;
    APP_CORE_FW_t fw;
} _ctx = {
    .doReboot = false,
    .deviceActive = 1,
    .enableStateLeds = MYNEWT_VAL(ENABLE_ACTIVE_LEDS),     //0,
    .nMods = 0,
    .nActions = 0,
    .requestedModule = -1,
    .idleTimeMovingSecs = MYNEWT_VAL(IDLETIME_MOVING_SECS),     //5 * 60, // 5mins
    .idleTimeNotMovingMins = MYNEWT_VAL(IDLETIME_NOTMOVING_MINS),     //120, // 2 hours
    .idleTimeInactiveMins = MYNEWT_VAL(IDLETIME_INACTIVE_MINS),     //120, // 2 hours
    .idleTimeCheckSecs = MYNEWT_VAL(IDLETIME_CHECK_SECS),     // 60,
    .joinTimeCheckSecs = 60, // timeout on join attempt
    .rejoinWaitMins = MYNEWT_VAL(JOIN_RETRY_LONG_MINS),     //120,   // 2 hours for rejoin tries between the try blocks
    .rejoinWaitSecs = MYNEWT_VAL(JOIN_RETRY_SHORT_SECS),     //60,    // 60s default for rejoin tries in the 'try X times' (ok for SF10 duty cycle?)
    .nbJoinAttempts = 0,
    .notStockMode = 0, // in stock mode by default until a rejoin works
    .modSetupTimeSecs = 3,
    .maxTimeBetweenULMins = 120, // 2 hours
    .lastULTime = 0,
    .lastDLId = 0, // default when new, will be read from the config mgr
    .loraCfg = {
        .useAck = false,
        .useAdr = MYNEWT_VAL(LORA_DEFAULT_ADR), // Allow default to be a build option
        .txPort = MYNEWT_VAL(LORA_TX_PORT),     //3,
        .loraSF = MYNEWT_VAL(LORA_DEFAULT_SF),     //LORAWAN_SF10,
        .txPower = 14,
        .txTimeoutMs = 10000,
        .appeui = {0x38, 0xB8, 0xEB, 0xE0, 0x00, 0x00, 0x00, 0x00},
        // note devEUI/appKey cannot have valid defaults as are specific to every device so are fixed at all 0
        // These should be set either via AT command or initial factory PROM programming.
        .deveui = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        .appkey = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    },
}; // Although _ctx is 0'd by definition (bss) we set non-0 init values here as there are a lot...

typedef enum
{
    LORA_TX_OK,
    LORA_TX_OK_ACKD,
    LORA_TX_ERR_RETRY,
    LORA_TX_ERR_FATAL,
    LORA_TX_ERR_NOTJOIN,
    LORA_TX_NO_TX
} LORA_TX_RESULT_t;

// predeclarations
static void registerActions();
static void executeDL(struct appctx *ctx, APP_CORE_DL_t *data);

// Module id is NOT the index in the table, so gotta find it
static int findModuleById(APP_MOD_ID_t mid) {
    for(int i=0;i<_ctx.nMods;i++) {
        if (_ctx.mods[i].id == mid) {
            return i;
        }
    }
    // not found
    return -1;
}

// Check if reboot flag is set ie request for a reboot.
static void checkReboot(struct appctx *ctx) {
    if (ctx->doReboot) {
        // Ok. Check if the reboot is 'normal' or into stock mode
        if (ctx->notStockMode==0) {
            log_debug("AC: reboot pending for stock mode... bye bye....");
            // To enter stock mode, we reboot with a specific reboot reason, and the rebootmgr will
            // do the enter. This ensures that if the MCU internal watchdog timer is running, it will
            // be disabled (example: STM32 IWDG timer cannot be stopped once started, and runs even in STOP/STANDBY modes)
            RMMgr_reboot(RM_ENTER_STOCK_MODE);      
        } else {
            log_debug("AC: reboot pending due to DL action... bye bye....");
            RMMgr_reboot(RM_DM_ACTION);      
        }
        // assert just in case didn't actually reboot...
        log_error("AC: should have rebooted!");
        assert(0);
    }
}
//helper to indicate device state in idle phase if required
static void deviceStateIndicate() {
    // application may decide to have flashing leds during idle (your funeral for the battery life...)
    // Notice we don't cancel or interupt any leds currently active that a module may have set running..
    if (_ctx.enableStateLeds) {
        if (AppCore_isDeviceActive()) {
            // Signal we are active during idle
            ledRequest(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_MIN, _ctx.idleTimeCheckSecs, LED_REQ_ENQUEUE);
        } else {
            // Signal we are inactive during idle
            ledRequest(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_MIN, _ctx.idleTimeCheckSecs, LED_REQ_ENQUEUE);
        }
    }
}
// Callback from config mgr when the actived modules mask changes
static void configChangedCB(void* ctx, uint16_t key)
{
    // just re-get all my config in case it changed
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MODS_ACTIVE_MASK, &_ctx.modsMask[0], MOD_MASK_SZ);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_IDLE_TIME_MOVING_SECS, &_ctx.idleTimeMovingSecs, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_IDLE_TIME_NOTMOVING_MINS, &_ctx.idleTimeNotMovingMins, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MODSETUP_TIME_SECS, &_ctx.modSetupTimeSecs, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MAXTIME_UL_MINS, &_ctx.maxTimeBetweenULMins, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_JOIN_TIMEOUT_SECS, &_ctx.joinTimeCheckSecs, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_RETRY_JOIN_TIME_MINS, &_ctx.rejoinWaitMins, sizeof(uint32_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_RETRY_JOIN_TIME_SECS, &_ctx.rejoinWaitSecs, sizeof(uint32_t));
}
static bool isModActive(uint8_t *mask, APP_MOD_ID_t id)
{
    if (id < 0 || id >= APP_MOD_LAST)
    {
        return false;
    }
    return ((mask[id / 8] & (1 << (id % 8))) != 0);
}
// application core state machine
// Define my state ids
enum MyStates
{
    MS_STARTUP,
    MS_STOCK,
    MS_TRY_JOIN,
    MS_WAIT_JOIN_RETRY,
    MS_IDLE,
    MS_GETTING_SERIAL_MODS,
    MS_GETTING_PARALLEL_MODS,
    MS_SENDING_UL,
    MS_LAST
};
enum MyEvents
{
    ME_MODS_OK,
    ME_MODULE_DONE,
    ME_FORCE_UL,
    ME_LORA_JOIN_OK,
    ME_LORA_JOIN_FAIL,
    ME_LORA_RESULT,
    ME_LORA_RX,
    ME_CONSOLE_TIMEOUT
};
// related fns

static void lora_join_cb(void *userctx, LORAWAN_RESULT_t res)
{
    //    log_debug("lora tx cb : result:%d", res);
    if (res == LORAWAN_RES_JOIN_OK)
    {
        sm_sendEvent(_ctx.mySMId, ME_LORA_JOIN_OK, (void *)res);
    }
    else
    {
        sm_sendEvent(_ctx.mySMId, ME_LORA_JOIN_FAIL, (void *)res);
    }
}
static void lora_tx_cb(void *userctx, LORAWAN_RESULT_t res)
{
    //    log_debug("lora tx cb : result:%d", res);
    // Map lorawan api result codes to our list
    LORA_TX_RESULT_t ourres = LORA_TX_ERR_FATAL;
    switch (res)
    {
    case LORAWAN_RES_OK:
        ourres = LORA_TX_OK;
        break;
    case LORAWAN_RES_NOT_JOIN:
        ourres = LORA_TX_ERR_NOTJOIN;
        break;
    case LORAWAN_RES_DUTYCYCLE:
    case LORAWAN_RES_OCC:
    case LORAWAN_RES_NO_BW:
    case LORAWAN_RES_TIMEOUT: // what does this imply?
        ourres = LORA_TX_ERR_RETRY;
        break;
    default:
        log_error("AC:LW tx says error %d -> fatal", res);
        ourres = LORA_TX_ERR_FATAL;
        break;
    }
    // pass tx result directly as value as var 'res' may not be around when SM is run....
    sm_sendEvent(_ctx.mySMId, ME_LORA_RESULT, (void *)ourres);
}

static void lora_rx_cb(void *userctx, LORAWAN_RESULT_t res, uint8_t port, int rssi, int snr, uint8_t *msg, uint8_t sz)
{
    // Copy data into static message buffer in ctx as sendEvent is executed off this thread -> can't use stack var.
    if (sz > APP_CORE_DL_MAX_SZ)
    {
        // oops
        log_debug("AC:lora rx toobig sz %d", sz);
        return;
    }
    memcpy(&_ctx.rxmsg.payload[0], msg, sz);
    _ctx.rxmsg.sz = sz;
    // Decode it
    if (app_core_msg_dl_decode(&_ctx.rxmsg))
    {
        log_debug("AC:lora rx dlid %d, na %d", _ctx.rxmsg.dlId, _ctx.rxmsg.nbActions);
        sm_sendEvent(_ctx.mySMId, ME_LORA_RX, (void *)(&_ctx.rxmsg));
    }
    else
    {
        log_warn("AC:lora rx BAD sz %d b0/1 %02x:%02x", sz, ((uint8_t *)msg)[0], ((uint8_t *)msg)[1]);
    }
}
// SM state functions

// state for startup init, AT command line, etc before becoming idle
// Only for initial startup after boot and never again
static SM_STATE_ID_t State_Startup(void *arg, int e, void *data)
{
    struct appctx *ctx = (struct appctx *)arg;
    switch (e)
    {
    case SM_ENTER:
    {
        log_debug("AC:START");
        // Stop all leds, and flash slow to show we're in console... this is for debug only
        ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_MIN, -1);
        ledStart(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_MIN, -1);
        // exit by timer if no at commands received
        sm_timer_start(ctx->mySMId, 30 * 1000);

        // activate console on the uart (if configured).
        if (startConsole() == false)
        {
            // start directly
            sm_sendEvent(ctx->mySMId, ME_FORCE_UL, NULL);
        }
        return SM_STATE_CURRENT;
    }
    case SM_EXIT:
    {
        stopConsole();
        // LEDs off
        ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
        ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
        return SM_STATE_CURRENT;
    }
    case SM_TIMEOUT:
    {
        // Only do the loop if console not active, else must do AT+RUN or have idle for 30s
        // This protects against console activate by UART bad input once, and then draining battery
        if (isConsoleActive())
        {
            log_debug("AC : console active stays in startup mode (for next 30s)");
            // exit by timer if no at commands received
            sm_timer_start(ctx->mySMId, 30 * 1000);
            return SM_STATE_CURRENT;
        }
        // exit startup by the specific event
        sm_sendEvent(ctx->mySMId, ME_FORCE_UL, NULL);
        return SM_STATE_CURRENT;
    }
    // Force UL is how we say go...
    case ME_FORCE_UL:
    {
        // Can we go for normal operation?
        if (ctx->deviceConfigOk)
        {
            // Starts by joining
            return MS_TRY_JOIN;
        }
        else
        {
            log_warn("AC: critical device config not ok->STOCK mode");
            return MS_STOCK;
        }
    }

    default:
    {
        sm_default_event_log(ctx->mySMId, "AC", e);
        return SM_STATE_CURRENT;
    }
    }
    assert(0); // shouldn't get here
}
// After startup (power on) we try for a join
static SM_STATE_ID_t State_TryJoin(void *arg, int e, void *data)
{
    struct appctx *ctx = (struct appctx *)arg;
    switch (e)
    {
    case SM_ENTER:
    {
        checkReboot(ctx);
        // Stop all leds, and flash fast both to show we're trying join
        ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_5HZ, -1);
        ledStart(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_5HZ, -1);
        // Set desired low power mode to be just light doze as console is active for first period
        LPMgr_setLPMode(ctx->lpUserId, LP_DOZE);
        // start join process
        LORAWAN_RESULT_t status = lora_api_join(lora_join_cb, ctx->loraCfg.loraSF, NULL);
        if (status == LORAWAN_RES_JOIN_OK)
        {
            // already joined (?) seems unlikely so warn about it
            log_warn("AC:try join : already joined?!?");
            sm_sendEvent(ctx->mySMId, ME_LORA_JOIN_OK, NULL);
        }
        else if (status != LORAWAN_RES_OK)
        {
            // Failed to start join process... this isn't great
            log_warn("AC:try join : tx attempt failed immediately (%d)", status);
            sm_sendEvent(ctx->mySMId, ME_LORA_JOIN_FAIL, NULL);
        }
        else
        {
            // Start the join timeout (shouldnt need it...)
            sm_timer_start(ctx->mySMId, ctx->joinTimeCheckSecs * 1000);
            log_debug("AC:try join : timeout in %d secs", ctx->joinTimeCheckSecs);
            // Record time
            ctx->joinStartTS = TMMgr_getRelTimeSecs();
        }
        return SM_STATE_CURRENT;
    }
    case SM_EXIT:
    {
        // LEDs off
        ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
        ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
        // any low power when not idle will be basic low power MCU (ie with radio and gpios on)
        LPMgr_setLPMode(ctx->lpUserId, LP_DOZE);
        return SM_STATE_CURRENT;
    }
    case SM_TIMEOUT:
    {
        // this means join failed (and badly as stack didnt tell us)
        log_warn("AC:tj: fail as sm timeout??");
        sm_sendEvent(ctx->mySMId, ME_LORA_JOIN_FAIL, NULL);
        return SM_STATE_CURRENT;
    }

    case ME_LORA_JOIN_OK:
    {
        log_info("AC:join ok");
        // Update to say we are not in stock mode
        ctx->notStockMode = 1;
        CFMgr_setElement(CFG_UTIL_KEY_STOCK_MODE, &ctx->notStockMode, 1);
        app_core_msg_ul_init(&ctx->txmsg);
        ctx->ulIsCrit = false;         // assume we're not gonna send it (its not critical)
        return MS_GETTING_SERIAL_MODS; // go directly get data and send it
    }
    case ME_LORA_JOIN_FAIL:
    {
        // For stock mode, check if we ever managed to join (which sets stock mode to false)
        if (ctx->notStockMode == 0)
        {
            log_warn("AC:join fail and never joined, going stock mode");
            return MS_STOCK;
        }
        log_warn("AC:join fail wait to retry");
        return MS_WAIT_JOIN_RETRY;
    }
    default:
    {
        sm_default_event_log(ctx->mySMId, "AC", e);
        return SM_STATE_CURRENT;
    }
    }
    assert(0); // shouldn't get here
}
// if no join, and has never joined, we end up here to be in very low power mode forever...
static SM_STATE_ID_t State_Stock(void *arg, int e, void *data)
{
    struct appctx *ctx = (struct appctx *)arg;
    switch (e)
    {
    case SM_ENTER:
    {
        log_warn("AC:stock mode");
        // Before entering stock mode, we leave a small window in which it is possible to connect to the device
        // JTAG port to update firmware/config (as in the stock mode MCU low power this may not be possible)
        // During this time we will put leds on permantently to signal this to user 
        // (for 1s less than the timeout to let ledmgr go idle before deep sleep)
        ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_ON, STOCK_MODE_DELAY_SECS-1);
        ledStart(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_ON, STOCK_MODE_DELAY_SECS-1);
        sm_timer_start(ctx->mySMId, STOCK_MODE_DELAY_SECS*1000);      // till zombie
        return SM_STATE_CURRENT;
    }
    case SM_EXIT:
    {
        // Should not happen!
        log_debug("AC:stock forever but exiting???");
        // any low power when not idle will be basic low power MCU (ie with radio and gpios on)
        LPMgr_setLPMode(ctx->lpUserId, LP_DOZE);
        return SM_STATE_CURRENT;
    }
    case SM_TIMEOUT:
    {
        // Reboot into stock mode
        ctx->doReboot = true;
        ctx->notStockMode = 0;      // normally the case if entered this state
        checkReboot(ctx);
        return SM_STATE_CURRENT;
    }
    default:
    {
        sm_default_event_log(ctx->mySMId, "AC", e);
        return SM_STATE_CURRENT;
    }
    }
    assert(0); // shouldn't get here
}
// Wait here in low power until time to retry our join
static SM_STATE_ID_t State_WaitJoinRetry(void *arg, int e, void *data)
{
    struct appctx *ctx = (struct appctx *)arg;
    switch (e)
    {
    case SM_ENTER:
    {
        checkReboot(ctx);
        // Start the retry join timeout
        ctx->nbJoinAttempts++;
        // We are allowing up to X tries with just Y second intervals, after which we go to a longer timeout of Z mins
        uint8_t maxrapid = 3; // Default of 3 goes before sleeping a long time
        CFMgr_getOrAddElement(CFG_UTIL_KEY_MAX_RAPID_JOIN_ATTEMPTS, &maxrapid, 1);
        if (ctx->nbJoinAttempts < maxrapid)
        {
            // try again in short time
            sm_timer_start(ctx->mySMId, ctx->rejoinWaitSecs * 1000);
            log_debug("AC:wjr : retry %d secs", ctx->rejoinWaitSecs);
        }
        else
        {
            sm_timer_start(ctx->mySMId, ctx->rejoinWaitMins * 60000);
            log_debug("AC:wjr : retry %d mins", ctx->rejoinWaitMins);
            ctx->nbJoinAttempts = 0; // as we're in the long timeout, reset count for next time
        }
        // all modules deepsleep
        for (int i = 0; i < ctx->nMods; i++)
        {
            if (ctx->mods[i].api->deepsleepCB != NULL)
            {
                (*(ctx->mods[i].api->deepsleepCB))();
            }
        }

        // LEDs off
        ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
        ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
        LPMgr_setLPMode(ctx->lpUserId, LP_DEEPSLEEP);
        return SM_STATE_CURRENT;
    }
    case SM_EXIT:
    {
        // LEDs off
        ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
        ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
        // any low power when not idle will be basic low power MCU (ie with radio and gpios on)
        LPMgr_setLPMode(ctx->lpUserId, LP_DOZE);
        return SM_STATE_CURRENT;
    }
    case SM_TIMEOUT:
    {
        // Ok, try join again
        return MS_TRY_JOIN;
    }
    default:
    {
        sm_default_event_log(ctx->mySMId, "AC", e);
        return SM_STATE_CURRENT;
    }
    }
    assert(0); // shouldn't get here
}
// Idling
static SM_STATE_ID_t State_Idle(void *arg, int e, void *data)
{
    struct appctx *ctx = (struct appctx *)arg;
    switch (e)
    {
    case SM_ENTER:
    {
        checkReboot(ctx);
        //Initialise the DM we're sending next time -> this means executed actions can start to fill it during idle time
        app_core_msg_ul_init(&ctx->txmsg);
        ctx->ulIsCrit = false; // assume we're not gonna send it (its not critical)
        if (ctx->idleTimeMovingSecs == 0)
        {
            // no idleness, this device runs continuously... (eg if its powered)
            log_debug("AC:no idle");
            sm_sendEvent(ctx->mySMId, ME_FORCE_UL, NULL);
            return SM_STATE_CURRENT;
        }
        // Start the wakeup timeout (every 60s we wake to check for movement)
        sm_timer_start(ctx->mySMId, ctx->idleTimeCheckSecs * 1000);
        log_debug("AC:idle %d secs", ctx->idleTimeCheckSecs);
        // Record time
        ctx->idleStartTS = TMMgr_getRelTimeSecs();
        // all modules deepsleep
        for (int i = 0; i < ctx->nMods; i++)
        {
            if (ctx->mods[i].api->deepsleepCB != NULL)
            {
                (*(ctx->mods[i].api->deepsleepCB))();
            }
        }
        // and stay idle in deep sleep if possible (other code may also have an option)
        LPMgr_setLPMode(ctx->lpUserId, LP_DEEPSLEEP);
        // if enabled signal the device state (active or inactive) via LED flash pattern (enqueued) 
        // Note we don't cancel leds; to allow any modules to have set a time limited leds sequence without it getting cancelled immediatly...
        deviceStateIndicate();
        
        return SM_STATE_CURRENT;
    }
    case SM_EXIT:
    {
        // LEDs off
        ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
        ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
        //            log_check_uart_active();        // so closes it if no logs being sent - not yet tested removed
        // any low power when not idle will be basic low power MCU (ie with radio and gpios on)
        LPMgr_setLPMode(ctx->lpUserId, LP_DOZE);
        return SM_STATE_CURRENT;
    }
    case SM_TIMEOUT:
    {
        // Call any module's tic cbs that are registered (before checking run cycle)
        for (int i = 0; i < ctx->nMods; i++)
        {
            if (isModActive(ctx->modsMask, ctx->mods[i].id))
            {
                // Call if defined
                if (ctx->mods[i].api->ticCB != NULL)
                {
                    (*(ctx->mods[i].api->ticCB))();
                }
            }
        }

        // calculate timeout 
        // if device not active, use specific timeout as default
        uint32_t idletimeS = ctx->idleTimeInactiveMins * 60;
        if (AppCore_isDeviceActive()) {
            // device is active -> check did we move? deal with difference between moving and not moving times
            MMMgr_check(); // check hardware
            idletimeS = ctx->idleTimeNotMovingMins * 60;
            // check if has moved recently and use different timeout
            if (MMMgr_hasMovedSince(ctx->lastULTime)) {
                idletimeS = ctx->idleTimeMovingSecs;
                log_debug("AC:move (%d ago) since UL , it %d", (TMMgr_getRelTimeSecs() - MMMgr_getLastMovedTime()), idletimeS);
            } else {
                log_debug("AC:no move (%d ago) since UL , it %d", (TMMgr_getRelTimeSecs() - MMMgr_getLastMovedTime()), idletimeS);
            }
        }
        idletimeS -= 1; // adjust by 1s to get run if 'close' to timeout
        uint32_t dt = TMMgr_getRelTimeSecs() - ctx->idleStartTS;
        if (dt >= idletimeS)
        {
            return MS_GETTING_SERIAL_MODS;
        }
        // else stay here. reset timeout for next check
        sm_timer_start(ctx->mySMId, ctx->idleTimeCheckSecs * 1000);
        log_debug("AC:reidle %ds as %d < %d", ctx->idleTimeCheckSecs, dt, idletimeS);
        //            log_check_uart_active();        // so closes it if no logs being sent - not yet tested removed
        LPMgr_setLPMode(ctx->lpUserId, LP_DEEPSLEEP);
        // if enabled signal the device state (active or inactive)
        deviceStateIndicate();
        return SM_STATE_CURRENT;
    }

    case ME_FORCE_UL:
    {
        // another module woke us up...
        // potentially deal with request to only run 1 module.... data is either NULL or 1000+module id
        _ctx.requestedModule = -1;
        if (data != NULL)
        {
            int mid = ((uint32_t)data) - 1000;
            if (mid >= 0 && mid < APP_MOD_LAST)
            {
                _ctx.requestedModule = mid;
            }
        }
        return MS_GETTING_SERIAL_MODS;
    }
    case ME_LORA_RX:
    {
        // This should not happen in this state as we are in DEEPSLEEP ie radio off
        if (data != NULL)
        {
            executeDL(ctx, (APP_CORE_DL_t *)data);
            // if enabled signal the device state (active or inactive) in case it changed, or in case the action changed the leds
            deviceStateIndicate();
        }
        return SM_STATE_CURRENT;
    }
    default:
    {
        sm_default_event_log(ctx->mySMId, "AC", e);
        return SM_STATE_CURRENT;
    }
    }
    assert(0); // shouldn't get here
}
// Get all the modules that require to be executed in series
static SM_STATE_ID_t State_GettingSerialMods(void *arg, int e, void *data)
{
    struct appctx *ctx = (struct appctx *)arg;

    switch (e)
    {
    case SM_ENTER:
    {
        ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_2HZ, -1);
        // find first serial guy by sending ourselves the done event with idx=-1
        ctx->currentSerialModIdx = -1;
        sm_sendEvent(ctx->mySMId, ME_MODULE_DONE, NULL);
        // NOTE : the UL message is initialise when entering idle -> this lets any code that
        // executes during idle (eg on a tic) put their data in directly.
        return SM_STATE_CURRENT;
    }
    case SM_EXIT:
    {
        ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
        // Get data from active modules to build UL message
        return SM_STATE_CURRENT;
    }
    case SM_TIMEOUT:
    {
        // Timeout is same as saying its done
        if (ctx->currentSerialModIdx < 0)
        {
            // no mod running, move on
            return MS_GETTING_PARALLEL_MODS;
        }
        // drop thru with data set to id of running guy
        data = (void *)(ctx->mods[ctx->currentSerialModIdx].id);
        // !! DROP THRU INTENTIONAL !!
    }
    case ME_MODULE_DONE:
    {
        // get the id of the module what is done (the ID is the 'data' param's value)
        // check its the one we're waiting for (if not first time)
        if (ctx->currentSerialModIdx > 0)
        {
            if ((int)data == ctx->mods[ctx->currentSerialModIdx].id)
            {
                //                log_debug("done Smod %d, id %d ", ctx->currentSerialModIdx, (int)data);
                // Get the data
                ctx->ulIsCrit |= (*(ctx->mods[ctx->currentSerialModIdx].api->getULDataCB))(&ctx->txmsg);
                // stop any activity
                (*(ctx->mods[ctx->currentSerialModIdx].api->stopCB))();
            }
            else
            {
                log_warn("AC:Smod %d done but not active [%d]", (int)data, ctx->mods[ctx->currentSerialModIdx].id);
                // this should not happen!
                return MS_GETTING_PARALLEL_MODS;
            }
        }
        // Find next serial one to run or goto parallels if all done
        for (int i = (ctx->currentSerialModIdx + 1); i < ctx->nMods; i++)
        {
            // Module is selected iff we are NOT explicitly requesting 1 module and its active, OR it is the one requested
            if ((ctx->requestedModule < 0 && isModActive(ctx->modsMask, ctx->mods[i].id)) ||
                (ctx->mods[i].id == ctx->requestedModule))
            {
                if (ctx->mods[i].exec == EXEC_SERIAL)
                {
                    ctx->currentSerialModIdx = i;
                    uint32_t timeReqd = (*(ctx->mods[i].api->startCB))();
                    // May return 0, which means no need for this module to run this time (no UL data)
                    if (timeReqd != 0)
                    {
                        // start timeout for current mod to get their data
                        sm_timer_start(ctx->mySMId, timeReqd);
                        log_debug("AC:Smod [%s] for %d ms", ctx->mods[i].name, timeReqd);
                        return SM_STATE_CURRENT;
                    }
                    else
                    {
                        log_debug("AC:Smod [%s] says not this cycle", ctx->mods[i].name);
                    }
                }
            }
        }
        // No more serial guys, go to parallels
        return MS_GETTING_PARALLEL_MODS;
    }
    default:
    {
        sm_default_event_log(ctx->mySMId, "AC", e);
        return SM_STATE_CURRENT;
    }
    }
    assert(0); // shouldn't get here
}
// Get all the modules that allow to be executed in parallel
static SM_STATE_ID_t State_GettingParallelMods(void *arg, int e, void *data)
{
    struct appctx *ctx = (struct appctx *)arg;

    switch (e)
    {
    case SM_ENTER:
    {
        ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_2HZ, -1);
        uint32_t modtime = 0;
        // and tell mods to go for max timeout they require
        for (int i = 0; i < ctx->nMods; i++)
        {
            //                log_debug("Pmod %d for mask %x", ctx->mods[i].id,  ctx->modsMask[0]);
            // Module is selected iff we are NOT explicitly requesting 1 module and its active, OR it is the one requested
            if ((ctx->requestedModule < 0 && isModActive(ctx->modsMask, ctx->mods[i].id)) ||
                (ctx->mods[i].id == ctx->requestedModule))
            {
                if (ctx->mods[i].exec == EXEC_PARALLEL)
                {
                    uint32_t timeReqd = (*(ctx->mods[i].api->startCB))();
                    if (timeReqd > modtime)
                    {
                        modtime = timeReqd;
                    }
                }
            }
        }
        if (modtime > 0)
        {
            // start timeout for current mods to get their data (short time)
            log_debug("AC:pmod for %d ms", modtime);
            sm_timer_start(ctx->mySMId, modtime);
        }
        else
        {
            // no parallel mods, timeout now
            log_debug("AC:no Pmods to check");
            sm_sendEvent(ctx->mySMId, SM_TIMEOUT, NULL);
        }
        return SM_STATE_CURRENT;
    }
    case SM_EXIT:
    {
        ledCancel(MYNEWT_VAL(MODS_ACTIVE_LED));
        return SM_STATE_CURRENT;
    }
    case SM_TIMEOUT:
    {
        // Get data from active modules to build UL message
        // Note that to mediate between modules that use same IOs eg I2C or UART (with UART selector)
        // they should be "serial" type not parallel
        for (int i = 0; i < ctx->nMods; i++)
        {
            // Module is selected iff we are NOT explicitly requesting 1 module and its active, OR it is the one requested
            if ((ctx->requestedModule < 0 && isModActive(ctx->modsMask, ctx->mods[i].id)) ||
                (ctx->mods[i].id == ctx->requestedModule))
            {
                if (ctx->mods[i].exec == EXEC_PARALLEL)
                {
                    // Get the data, and set the flag if module says the ul MUST be sent
                    ctx->ulIsCrit |= (*(ctx->mods[i].api->getULDataCB))(&ctx->txmsg);
                    // stop any activity
                    (*(ctx->mods[i].api->stopCB))();
                }
            }
        }
        // critical to send it if been a while since last one
        ctx->ulIsCrit |= ((TMMgr_getRelTimeSecs() - ctx->lastULTime) > (ctx->maxTimeBetweenULMins * 60));
        if (ctx->ulIsCrit)
        {
            return MS_SENDING_UL;
        }
        else
        {
            return MS_IDLE;
        }
    }

    default:
    {
        sm_default_event_log(ctx->mySMId, "AC", e);
        return SM_STATE_CURRENT;
    }
    }
    assert(0); // shouldn't get here
}
static LORA_TX_RESULT_t tryTX(struct appctx *ctx, bool willListen)
{
    uint8_t txsz = app_core_msg_ul_prepareNextTx(&ctx->txmsg, ctx->lastDLId, willListen);
    LORA_TX_RESULT_t res = LORA_TX_ERR_RETRY;
    if (txsz > 0)
    {
        LORAWAN_RESULT_t txres = lora_api_send(ctx->loraCfg.loraSF, ctx->loraCfg.txPort, ctx->loraCfg.useAck, willListen,
                                               app_core_msg_ul_getTxPayload(&ctx->txmsg), txsz, lora_tx_cb, ctx);
        if (txres == LORAWAN_RES_OK)
        {
            res = LORA_TX_OK;
            log_info("AC:UL tx req SF %d, ack %d, listen %d, sz %d", ctx->loraCfg.loraSF, ctx->loraCfg.useAck, willListen, txsz);
        }
        else
        {
            res = LORA_TX_ERR_RETRY;
            log_warn("AC:no UL tx said %d", txres);
        }
    }
    else
    {
        log_info("AC:no UL to send");
        res = LORA_TX_NO_TX; // not an actual error but no tx so no point waiting for result
    }
    return res;
}
static SM_STATE_ID_t State_SendingUL(void *arg, int e, void *data)
{
    struct appctx *ctx = (struct appctx *)arg;

    switch (e)
    {
    case SM_ENTER:
    {
        log_debug("AC:trying to send UL");
        // start leds for UL
        ledStart(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_5HZ, -1);
        log_debug("UL has %d elements, sz %d %d %d %d", ctx->txmsg.msgNbFilling, ctx->txmsg.msgs[0].sz, ctx->txmsg.msgs[1].sz, ctx->txmsg.msgs[2].sz, ctx->txmsg.msgs[3].sz);
        LORA_TX_RESULT_t res = tryTX(ctx, true);
        if (res == LORA_TX_OK)
        {
            // this timer should be big enough to have received any DL triggered by the ULs
            sm_timer_start(ctx->mySMId, UL_WAIT_DL_TIMEOUTMS);
            // ok wait for result
        }
        else
        {
            // want to abort immediately... send myself a result event
            log_warn("AC:send UL tx failed %d", res);
            sm_sendEvent(ctx->mySMId, ME_LORA_RESULT, (void *)res); // pass the code as the value not as a pointer
        }
        return SM_STATE_CURRENT;
    }
    case SM_EXIT:
    {
        ledCancel(MYNEWT_VAL(NET_ACTIVE_LED));
        return SM_STATE_CURRENT;
    }
    case SM_TIMEOUT:
    {
        // Done , go idle
        log_info("AC:stop UL send SM timeout");
        return MS_IDLE;
    }
    case ME_LORA_RX:
    {
        if (data != NULL)
        {
            executeDL(ctx, (APP_CORE_DL_t *)data);
        }
        return SM_STATE_CURRENT;
    }

    case ME_LORA_RESULT:
    {
        // Default is an error on tx which sent us this event... and the value is passed
        LORA_TX_RESULT_t res = ((LORA_TX_RESULT_t)data);
        switch (res)
        {
        case LORA_TX_OK_ACKD:
        {
            log_info("AC:tx : ACKD");
            ctx->lastULTime = TMMgr_getRelTimeSecs();
            break;
        }
        case LORA_TX_OK:
        {
            log_info("AC:tx : OK");
            ctx->lastULTime = TMMgr_getRelTimeSecs();
            break;
        }
        case LORA_TX_ERR_RETRY:
        {
            log_warn("AC:tx : fail:retry");
            // Step back one in ULs so that next tryTx gets same one to retry
            // TODO                    app_core_msg_ul_retry(&ctx->txmsg);
            break;
        }
        case LORA_TX_ERR_NOTJOIN:
        {
            // Retry join here? change the SF? TODO
            log_warn("AC:tx : fail : notJOIN?");
            return MS_IDLE;
        }
        case LORA_TX_ERR_FATAL:
        {
            log_error("AC:tx : FATAL ERROR, assert/restart!");
            assert(0);
            return MS_IDLE;
        }
        case LORA_TX_NO_TX:
        {
            log_info("AC:tx initial no UL, going idle");
            return MS_IDLE;
        }
        default:
        {
            log_warn("AC:tx : result %d? trynext", res);
            break; // fall out
        }
        }
        // See if we have another msg to go. Note we say 'not listening' ie don't send me DL as we haven't
        // processing any RX yet, so our 'lastDLId' is not up to date and we'll get a repeated action DL!
        res = tryTX(ctx, false);
        if (res == LORA_TX_OK)
        {
            // this timer should be big enough to have received any DL triggered by the ULs
            sm_timer_start(ctx->mySMId, UL_WAIT_DL_TIMEOUTMS);
            // ok wait for result
            return SM_STATE_CURRENT;
        }
        else
        {
            log_debug("AC:lora tx UL res %d, going idle", res);
            // TODO - if a ERR_RETRY, then get any unsent ULs and keep for next time to retry???? or have explicit state?
            // And we're done
            return MS_IDLE;
        }
    }
    default:
    {
        sm_default_event_log(ctx->mySMId, "AC", e);
        return SM_STATE_CURRENT;
    }
    }
    assert(0); // shouldn't get here
}
// State table : note can be in any order as the 'id' field is what maps the state id to the rest
static SM_STATE_t _mySM[MS_LAST] = {
    {.id = MS_STARTUP, .name = "Startup", .fn = State_Startup},
    {.id = MS_STOCK, .name = "Stock", .fn = State_Stock},
    {.id = MS_TRY_JOIN, .name = "TryJoin", .fn = State_TryJoin},
    {.id = MS_WAIT_JOIN_RETRY, .name = "WaitJoinRetry", .fn = State_WaitJoinRetry},
    {.id = MS_IDLE, .name = "Idle", .fn = State_Idle},
    {.id = MS_GETTING_SERIAL_MODS, .name = "GettingSerialMods", .fn = State_GettingSerialMods},
    {.id = MS_GETTING_PARALLEL_MODS, .name = "GettingParallelMods", .fn = State_GettingParallelMods},
    {.id = MS_SENDING_UL, .name = "SendingUL", .fn = State_SendingUL},
};

// Called to start the action, after all sysinit done
void app_core_start(int fwmaj, int fwmin, int fwbuild, const char *fwdate, const char *fwname)
{
    log_debug("AC:init");
    // Store build data for anyone that wants it
    _ctx.fw.fwmaj = fwmaj;
    _ctx.fw.fwmin = fwmin;
    _ctx.fw.fwbuild = fwbuild;
    strncpy(_ctx.fw.fwdate, fwdate, MAXFWDATE);
    strncpy(_ctx.fw.fwname, fwname, MAXFWNAME);

    // set the hardware base card version from the config to the BSP
    uint8_t hwrev = BSP_getHwVer();
    CFMgr_getOrAddElement(CFG_UTIL_KEY_HW_BASE_REV, &hwrev, sizeof(uint8_t));
    BSP_setHwVer(hwrev);

    // Get the app core config, ensuring values are 'reasonable'
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_IDLE_TIME_MOVING_SECS, &_ctx.idleTimeMovingSecs, 0, 24 * 60 * 60);
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_IDLE_TIME_NOTMOVING_MINS, &_ctx.idleTimeNotMovingMins, 0, 24 * 60);
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_IDLE_TIME_INACTIVE_MINS, &_ctx.idleTimeInactiveMins, 5, 24*60);
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_IDLE_TIME_CHECK_SECS, &_ctx.idleTimeCheckSecs, 15, 5 * 60);
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_JOIN_TIMEOUT_SECS, &_ctx.joinTimeCheckSecs, 18, 30);
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_RETRY_JOIN_TIME_MINS, &_ctx.rejoinWaitMins, 1, 24 * 60);
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_RETRY_JOIN_TIME_SECS, &_ctx.rejoinWaitSecs, 15, 120);
    memset(&_ctx.modsMask[0], 0xff, MOD_MASK_SZ); // Default every module is active
    CFMgr_getOrAddElement(CFG_UTIL_KEY_MODS_ACTIVE_MASK, &_ctx.modsMask[0], MOD_MASK_SZ);
    CFMgr_getOrAddElementCheckRangeUINT32(CFG_UTIL_KEY_MAXTIME_UL_MINS, &_ctx.maxTimeBetweenULMins, 1, 24 * 60);
    CFMgr_getOrAddElementCheckRangeUINT8(CFG_UTIL_KEY_DL_ID, &_ctx.lastDLId, 0, 15);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_STOCK_MODE, &_ctx.notStockMode, sizeof(uint8_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_DEVICE_ACTIVE, &_ctx.deviceActive, sizeof(uint8_t));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_ENABLE_DEVICE_STATE_LEDS, &_ctx.enableStateLeds, sizeof(uint8_t));
    CFMgr_registerCB(configChangedCB); // For changes to our config

    registerActions();
    // register to be able to change LowPower mode (no callback as we don't care about lp changes...)
    _ctx.lpUserId = LPMgr_register(NULL);

    // deveui, and other lora setup config are in PROM
    // any config that is critical is checked for existance - if it isn't already in PROM we have no reasonable
    // default, and so cannot run -> we will take appropriate action in the state machine
    _ctx.deviceConfigOk = true;
    // devEUI is critical : default is all 0s -> not configured
    CFMgr_getOrAddElement(CFG_UTIL_KEY_LORA_DEVEUI, &_ctx.loraCfg.deveui, 8);
    _ctx.deviceConfigOk &= Util_notAll0(&_ctx.loraCfg.deveui[0], 8);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_LORA_APPEUI, &_ctx.loraCfg.appeui, 8);
    // appKey is critical
    CFMgr_getOrAddElement(CFG_UTIL_KEY_LORA_APPKEY, &_ctx.loraCfg.appkey, 16);
    _ctx.deviceConfigOk &= Util_notAll0(&_ctx.loraCfg.appkey[0], 16);
    //    CFMgr_getOrAddElement(CFG_UTIL_KEY_LORA_DEVADDR, &_ctx.loraCfg.devAddr, sizeof(uint32_t));
    //    CFMgr_getOrAddElement(CFG_UTIL_KEY_LORA_NWKSKEY, &_ctx.loraCfg.nwkSkey, 16);
    //    CFMgr_getOrAddElement(CFG_UTIL_KEY_LORA_APPSKEY, &_ctx.loraCfg.appSkey, 16);
    CFMgr_getOrAddElement(CFG_UTIL_KEY_LORA_ADREN, &_ctx.loraCfg.useAdr, sizeof(bool));
    CFMgr_getOrAddElement(CFG_UTIL_KEY_LORA_ACKEN, &_ctx.loraCfg.useAck, sizeof(bool));
    CFMgr_getOrAddElementCheckRangeUINT8(CFG_UTIL_KEY_LORA_SF, &_ctx.loraCfg.loraSF, LORAWAN_SF7, LORAWAN_SF_DEFAULT);
    CFMgr_getOrAddElementCheckRangeINT8(CFG_UTIL_KEY_LORA_TXPOWER, &_ctx.loraCfg.txPower, 0, 22);
    CFMgr_getOrAddElementCheckRangeUINT8(CFG_UTIL_KEY_LORA_TXPORT, &_ctx.loraCfg.txPort, 1, 255);
    if (_ctx.deviceConfigOk) {
        // Note the api wants the ids in init -> this means if user changes in AT then they need to reboot...
        lora_api_init(&_ctx.loraCfg.deveui[0], &_ctx.loraCfg.appeui[0], &_ctx.loraCfg.appkey[0], _ctx.loraCfg.useAdr, _ctx.loraCfg.loraSF, _ctx.loraCfg.txPower);
        LORAWAN_RESULT_t rxres = lora_api_registerRxCB(-1, lora_rx_cb, &_ctx);
        if (rxres != LORAWAN_RES_OK)
        {
            // oops
            log_warn("AC:bad register lora rx cb %d", rxres);
            assert(0);
        }
        _ctx.fw.loraregion = lora_api_getCurrentRegion();
    } else {
        log_warn("AC:no lora init as missing critical device config");
    }
    // write fw config into PROM as config key so that it is accessible via AT command or DL action?
    // auto creates if 1st run
    CFMgr_setElement(CFG_UTIL_KEY_FIRMWARE_INFO, &_ctx.fw, sizeof(_ctx.fw));

    // initialise console for use during idle periods if enabled
    if (MYNEWT_VAL(WCONSOLE_ENABLED) != 0)
    {
        initConsole();
        log_debug("AC:CN EN");
    }
    else
    {
        log_warn("AC:CN DIS");
    }

    // post boot we do STARTUP state (as its name suggests)
    _ctx.mySMId = sm_init("app-core", _mySM, MS_LAST, MS_STARTUP, &_ctx);
    sm_start(_ctx.mySMId);
}

// Get fw build info
APP_CORE_FW_t *AppCore_getFwInfo()
{
    return &_ctx.fw;
}

// core api for modules
// Allow other code (modules) to change the active state of the device (eg via user input using buttons or shaking)
void AppCore_setDeviceState(bool active) {
    _ctx.deviceActive = (active?1:0);
    CFMgr_setElement(CFG_UTIL_KEY_DEVICE_ACTIVE, &_ctx.deviceActive, sizeof(uint8_t));
    if (_ctx.enableStateLeds) {
        if (active) {
            // Signal briefly we are active now
            ledStart(MYNEWT_VAL(MODS_ACTIVE_LED), FLASH_05HZ, 2);
        } else {
            // Signal briefly we are inactive now
            ledStart(MYNEWT_VAL(NET_ACTIVE_LED), FLASH_05HZ, 2);
        }
    }
}
// Is the device in active (data collection) or inactive (no data collection) mode?
bool AppCore_isDeviceActive() {
    return (_ctx.deviceActive==1);
}
// Enable or disable leds feedback about active/inactive state
void AppCore_setStateLeds(bool enabled) {
    _ctx.enableStateLeds = (enabled?1:0);
    CFMgr_setElement(CFG_UTIL_KEY_ENABLE_DEVICE_STATE_LEDS, &_ctx.enableStateLeds, sizeof(uint8_t));
}
// mcbs pointer must be to a static structure
void AppCore_registerModule(const char * name, APP_MOD_ID_t id, APP_CORE_API_t *mcbs, APP_MOD_EXEC_t execType)
{
    assert(id < APP_MOD_LAST);
    assert(_ctx.nMods < MAX_MODS);
    assert(mcbs != NULL);
    assert(mcbs->startCB != NULL);
    assert(mcbs->stopCB != NULL);
    assert(mcbs->getULDataCB != NULL);

    _ctx.mods[_ctx.nMods].api = mcbs;
    _ctx.mods[_ctx.nMods].name = name;
    _ctx.mods[_ctx.nMods].id = id;
    _ctx.mods[_ctx.nMods].exec = execType;
    _ctx.nMods++;
//    log_debug("AC: add [%d=%s] exec[%d]", id, name, execType);
}

const char* AppCore_getModuleName(APP_MOD_ID_t mid) {
    int midx = findModuleById(mid);
    if (midx >= 0 && _ctx.mods[midx].name!=NULL)
    {
        return _ctx.mods[midx].name;
    }
    return "NA";
}

// is module active?
bool AppCore_getModuleState(APP_MOD_ID_t mid)
{
    return isModActive(_ctx.modsMask, mid); // This is protected against bad mid values
}
// set module active/not active
void AppCore_setModuleState(APP_MOD_ID_t mid, bool active)
{
    if (mid >= 0 && mid < APP_MOD_LAST)
    {
        if (active)
        {
            // set the bit
            _ctx.modsMask[mid / 8] |= (1 << (mid % 8));
        }
        else
        {
            // clear the bit
            _ctx.modsMask[mid / 8] &= ~(1 << (mid % 8));
        }
        // And writeback to PROM
        CFMgr_setElement(CFG_UTIL_KEY_MODS_ACTIVE_MASK, &_ctx.modsMask[0], MOD_MASK_SZ);
    }
}

// register a DL action handler
// Note asserts if id already registered, or table is full
void AppCore_registerAction(uint8_t id, ACTIONFN_t cb)
{
    assert(_ctx.nActions < MAX_DL_ACTIONS);
    assert(cb != NULL);
    // Check noone else has registerd this
    for (int i = 0; i < _ctx.nActions; i++)
    {
        if (_ctx.actions[i].id == id)
        {
            assert(0);
        }
    }
    _ctx.actions[_ctx.nActions].id = id;
    _ctx.actions[_ctx.nActions].fn = cb;
    _ctx.nActions++;
//    log_debug("AC: RA [%d]", id);
}
// Find action fn or NULL
ACTIONFN_t AppCore_findAction(uint8_t id)
{
    for (int i = 0; i < _ctx.nActions; i++)
    {
        if (_ctx.actions[i].id == id)
        {
            return _ctx.actions[i].fn;
        }
    }
    return NULL;
}

// Last UL sent time (relative, seconds)
uint32_t AppCore_lastULTime()
{
    return _ctx.lastULTime;
}
// Time in ms to next UL
uint32_t AppCore_getTimeToNextUL()
{
    return TMMgr_getRelTimeSecs() - _ctx.idleStartTS;
}
// Request stop idle and goto UL phase now
// optionally request 'fast' UL ie just 1 module to let do data collection
//  (required for fast button UL sending)
bool AppCore_forceUL(int reqModule)
{
    void *ed = NULL;
    if (reqModule >= 0 && reqModule < APP_MOD_LAST)
    {
        ed = (void *)(1000 + reqModule); // add 1000 as if 0 then same as NULL....
    }
    return sm_sendEvent(_ctx.mySMId, ME_FORCE_UL, ed);
}

// Tell core we're done processing
void AppCore_module_done(APP_MOD_ID_t id)
{
    sm_sendEvent(_ctx.mySMId, ME_MODULE_DONE, (void *)id);
}

// Internals : action handling
static void executeDL(struct appctx *ctx, APP_CORE_DL_t *data)
{
    // Execute actions only if the dlid is not the last one we did, OR always if it is 0 (ie backend does not handle dlid)
    if ((data->dlId==0) || (data->dlId != ctx->lastDLId))
    {
        if (app_core_msg_dl_execute(data))
        {
            // If the id in the DL was 0, we don't update as this was not a 'controlled' case
            if (data->dlId!=0) {
                log_info("AC: exec DL ok id now %d", data->dlId);
                // Can update last dl id since we did its actions
                ctx->lastDLId = data->dlId;
                // Store in case we reboot
                CFMgr_setElement(CFG_UTIL_KEY_DL_ID, &ctx->lastDLId, sizeof(uint8_t));
            } else {
                log_info("AC: exec DL id 0, current last dlId was %d", ctx->lastDLId);
            }
        }
        else
        {
            log_warn("AC: DL with id %d not exec OK??", data->dlId);
        }
    }
    else
    {
        log_debug("AC:ignore DL actions repeat id (%d)", data->dlId);
    }
}

static void A_reboot(uint8_t *v, uint8_t l)
{
    log_info("action REBOOT");
    // TODO : if a value is given and is 1 -> goto stock mode ie clear stock flag and reboot into halt
    if (l>0 && v[0]==1) {
        // Update to say we ARE in stock mode
        _ctx.notStockMode = 0;
        CFMgr_setElement(CFG_UTIL_KEY_STOCK_MODE, &_ctx.notStockMode, 1);
    }
    // must wait till execute actions finished
    _ctx.doReboot = true;
}

static void A_setConfig(uint8_t *v, uint8_t l)
{
    if (l < 3)
    {
        log_warn("AC:action SETCONFIG BAD (too short value)");
    }
    // value is 2 bytes config id, l-2 bytes value to set
    uint16_t key = Util_readLE_uint16_t(v, 2);
    // Check if length is as the config key is already expecting
    uint8_t exlen = CFMgr_getElementLen(key);
    if (exlen == (l - 2))
    {
        if (CFMgr_setElement(key, v + 2, l - 2))
        {
            log_info("AC:action SETCONFIG (%04x) to %d len value OK", key, l - 2);
        }
        else
        {
            log_warn("AC:action SETCONFIG (%04x) to %d len value FAILS", key, l - 2);
        }
    }
    else
    {
        log_warn("AC:action SETCONFIG (%04x) to %d len value FAILS as expected len is %d", key, (l - 2), exlen);
    }
}

static void A_getConfig(uint8_t *v, uint8_t l)
{
    if (l < 2)
    {
        log_warn("AC:action GETCONFIG BAD (too short value)");
    }
    // value is 2 bytes config id
    uint16_t key = Util_readLE_uint16_t(v, 2);
    uint8_t vb[20]; // 2 bytes key, 1 byte len, only allow to get keys up to 16 bytes..
    int cl = CFMgr_getElement(key, &vb[3], 16);
    if (cl > 0)
    {
        log_info("AC:action GETCONFIG (%04x) value [", key);
        // Must give key/len as DL may request multiple!
        Util_writeLE_uint16_t(vb, 0, key);
        vb[2] = cl;
        for (int i = 0; i < cl; i++)
        {
            log_info("%02x", vb[i]);
        }
        // Allowed to add to UL during action execution
        if (app_core_msg_ul_addTLV(&_ctx.txmsg, APP_CORE_UL_CONFIG, cl + 3, vb))
        {
            log_info("] added to UL");
        }
        else
        {
            log_warn("] too long for UL");
        }
    }
    else
    {
        log_warn("AC:action GETCONFIG (%04x) no such key", key);
    }
}
static void A_fota(uint8_t *v, uint8_t l)
{
    // Expected to get 7 bytes of param:
    if (l != 7)
    {
        log_warn("AC:ignore FOTA bad len %d", l);
        return;
    }
    uint8_t fwmaj = v[0];
    uint8_t fwmin = v[1];
    uint8_t fwbuild = v[2];
    uint32_t fwhash = Util_readLE_uint32_t(&v[3], 4);
    log_warn("AC:action FOTA (%d/%d/%d : %8x)", fwmaj, fwmin, fwbuild, fwhash);
    // TODO set ready for fota
}

static void A_flashled(int8_t led, uint8_t *v, uint8_t l)
{
    if (l != 2)
    {
        log_warn("AC:ignore action flash led1 as bad param len %d", l);
        return;
    }
    // b0 = time in 100ms units -> convert to seconds
    int time = (*v / 10);
    if (time < 1)
    {
        time = 1;
    }
    // b1 = frequency in Hz
    int freq = *(v + 1);
    log_info("AC:action flash led %d %dms @%dHz", led, time, freq);
    if (freq < 1)
    {
        ledRequest(led, FLASH_05HZ, time, LED_REQ_INTERUPT);
    }
    else if (freq == 1)
    {
        ledRequest(led, FLASH_1HZ, time, LED_REQ_INTERUPT);
    }
    else if (freq == 2)
    {
        ledRequest(led, FLASH_2HZ, time, LED_REQ_INTERUPT);
    }
    else
    {
        ledRequest(led, FLASH_5HZ, time, LED_REQ_INTERUPT);
    }
}
static void A_flashled1(uint8_t *v, uint8_t l)
{
    A_flashled(MYNEWT_VAL(MODS_ACTIVE_LED), v, l);
}
static void A_flashled2(uint8_t *v, uint8_t l)
{
    A_flashled(MYNEWT_VAL(NET_ACTIVE_LED), v, l);
}
static void A_settime(uint8_t *v, uint8_t l)
{
    log_info("AC:action SETTIME");
    uint32_t now = Util_readLE_uint32_t(v, l);
    // boot time in UTC seconds is now - time elapsed since boot
    TMMgr_setBootTime(now - TMMgr_getRelTimeSecs());
}
// Return state of modules?
static void A_getmods(uint8_t *v, uint8_t l)
{
    log_info("AC:action GETMODS (TBI)");
}

static void registerActions()
{
    AppCore_registerAction(APP_CORE_DL_REBOOT, &A_reboot);
    AppCore_registerAction(APP_CORE_DL_SET_CONFIG, &A_setConfig);
    AppCore_registerAction(APP_CORE_DL_GET_CONFIG, &A_getConfig);
    AppCore_registerAction(APP_CORE_DL_SET_UTCTIME, &A_settime);
    AppCore_registerAction(APP_CORE_DL_FLASH_LED1, &A_flashled1);
    AppCore_registerAction(APP_CORE_DL_FLASH_LED2, &A_flashled2);
    AppCore_registerAction(APP_CORE_DL_FOTA, &A_fota);
    AppCore_registerAction(APP_CORE_DL_GET_MODS, &A_getmods);
}
