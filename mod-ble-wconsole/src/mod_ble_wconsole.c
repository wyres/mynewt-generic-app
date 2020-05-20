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

// BLE WCONSOLE
// Note this module is compatible with the other BLE modules as long as the BLE module is activated.
#include "os/os.h"
#include "bsp/bsp.h"

#include "wyres-generic/wutils.h"
#include "wyres-generic/configmgr.h"
#include "wyres-generic/timemgr.h"
#include "wyres-generic/wblemgr.h"
#include "wyres-generic/wconsole.h"

#include "app-core/app_core.h"
#include "app-core/app_msg.h"
#include "app-core/app_console.h"

#include "mod-ble/mod_ble.h"

#define MAX_TXSZ (256)

static struct {
    void* wbleCtx;
    uint8_t txbuf[MAX_TXSZ+3];      // space for \r\n\0 if required
} _ctx;

// Redecs
static void processATCmd(char* line);
static bool bleconsole_println(const char* l, ...);

/** callback fns from BLE generic package */
static void ble_cb(WBLE_EVENT_t e, void* d) {
    switch(e) {
        case WBLE_UART_DISC:
        case WBLE_COMM_FAIL: {
            log_debug("MBC: comm nok");
            // We're done : tell app-core
            AppCore_module_done(APP_MOD_BLE_CONSOLE);
            // No need to close connection
            break;
        }
        case WBLE_COMM_OK: {
            log_debug("MBC: comm ok");
            // cool. ready to rx data
            wble_line_open(_ctx.wbleCtx);
            break;
        }
        case WBLE_UART_CONN: {
            log_debug("MBC: uart ok");
            // Send initial "OK" so remote user knows we have connected
            APP_CORE_FW_t* fwinfo = AppCore_getFwInfo();
            bleconsole_println("WConsole v%d.%d/%d", fwinfo->fwmaj, fwinfo->fwmin, fwinfo->fwbuild);
            bleconsole_println("OK");
            break;
        }
        case WBLE_UART_RX: {
            // Parse it
            processATCmd((char*)d);
            break;
        }
        default: {
            // Ignore anything else
            log_debug("MBC cb %d", e);
            break;         
        }   
    }
}

// My api functions
static uint32_t start() {
    // start ble to go (may already be running), with a callback to tell me when its comm is ok (may be immediate if already running)
    // Request to check for connection is sent once comm is ok
    wble_start(_ctx.wbleCtx, ble_cb);

    return 30*60*1000;      // you can have the console on for 30 minutes max (make sure can't be stuck forever)
}

static void stop() {
    // Close the uart connection and ble use
    wble_line_close(_ctx.wbleCtx);
}

static void off() {
    // nothing to do
}
static void deepsleep() {
    // nothing to do
}

static bool getData(APP_CORE_UL_t* ul) {
    return false;
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
void mod_ble_wconsole_init(void) {
    // initialise access (this is resistant to multiple calls...)
    _ctx.wbleCtx = wble_mgr_init(MYNEWT_VAL(MOD_BLE_UART), MYNEWT_VAL(MOD_BLE_UART_BAUDRATE), MYNEWT_VAL(MOD_BLE_PWRIO), MYNEWT_VAL(MOD_BLE_UART_SELECT)); 
    // hook app-core for ble scan - serialised as competing for UART. Note we claim we're an ibeaon module
    AppCore_registerModule("BLE-WCONSOLE", APP_MOD_BLE_CONSOLE, &_api, EXEC_SERIAL);
//    log_debug("MB:mod-ble-wconsole inited");
}

static bool bleconsole_println(const char* l, ...) {
    bool ret = true;
    va_list vl;
    va_start(vl, l);
    vsprintf((char*)&_ctx.txbuf[0], l, vl);
    int len = strnlen((const char*)&_ctx.txbuf[0], MAX_TXSZ);
    if (len>=MAX_TXSZ) {
        // oops might just have broken stuff...
        len = MAX_TXSZ-1;
        ret = false;        // caller knows there was an issue
    }
    _ctx.txbuf[len]='\n';
    _ctx.txbuf[len+1]='\r';
    _ctx.txbuf[len+2]=0;
    len+=2;     // Don't send the null byte!
    int res = wble_line_write(_ctx.wbleCtx, &_ctx.txbuf[0], len);
    if (res<0) {
        _ctx.txbuf[0] = '*';
        wble_line_write(_ctx.wbleCtx, &_ctx.txbuf[0], 1);      // so user knows he missed something.
        ret = false;        // caller knows there was an issue
        // Not actually a lot we can do about this especially if its a flow control (SKT_NOSPACE) condition - ignore it
        log_debug("MBC wfail %d", res);      // just for debugger to watch
    } else {
        log_debug("MBC wok %d", len);      // just for debugger to watch
    }
    va_end(vl);
    return ret;
}

static void processATCmd(char* line) {
    // parse line into : command, args
    char* els[5];
    char* s = line;
    int elsi = 0;
    // first segment
    els[elsi++] = s;
    while (*s!=0 && elsi<5) {
        if (*s==' ' || *s=='=' || *s==',') {
            // make end of string at white space or param seperator
            *s='\0';
            s++;
            // consume blank space (not separators)
            while(*s==' ') {
                s++;
            }
            // If more string, then its the next element..
            if (*s!='\0') {
                els[elsi++] = s;
            } // else leave it on the end of string as we're done
        } else {
            s++;
        }
    }
    // check we are not talking to another console (return of OK or ERROR)
    if (strncmp("OK", els[0], 2)==0 || strncmp("ERROR", els[0], 5)==0) {
        // give up immediately
        log_debug("detected remote is console[%s], stopping", els[0]);
        wble_line_close(_ctx.wbleCtx);
        AppCore_module_done(APP_MOD_BLE_CONSOLE);
        return;
    }
    // Also break if get sent a AT+DISC
    if (strncmp("AT+DISC", els[0], 7)==0) {
        // give up immediately
        log_debug("remote disconnect, stopping");
        wble_line_close(_ctx.wbleCtx);
        AppCore_module_done(APP_MOD_BLE_CONSOLE);
        return;
    }

    // Ask the appcode atcmd console to process it
    ATRESULT res = execConsoleCmd(&bleconsole_println, elsi, els);
    switch(res) {
        case ATCMD_OK: {
            bleconsole_println("OK");
            break;
        }
        case ATCMD_GENERR: {
            bleconsole_println("ERROR");
            break;
        }
        case ATCMD_BADCMD: {
            bleconsole_println("ERROR");
            bleconsole_println("Unknown command [%s].", els[0]);
            log_debug("no cmd %s with %d args", els[0], elsi-1);
        }
        default:
            // Command processing already did return
            break;
    }
}
