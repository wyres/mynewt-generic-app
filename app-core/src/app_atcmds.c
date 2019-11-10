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
#include "wyres-generic/rebootmgr.h"
#include "wyres-generic/wconsole.h"
#include "wyres-generic/configmgr.h"
#include "wyres-generic/timemgr.h"
#include "wyres-generic/sensormgr.h"
#include "loraapi/loraapi.h"

#include "app-core/app_core.h"
#include "app-core/app_console.h"

/**
 *  AT Commands for appcore in idle mode
 */

// WConsole at commands we use
static ATRESULT atcmd_hello(uint8_t nargs, char* argv[]) {
    wconsole_println("Hello.");
    return ATCMD_OK;
}
static ATRESULT atcmd_who(uint8_t nargs, char* argv[]) {
    wconsole_println("AppCore");
    return ATCMD_OK;
}
// predef
static ATRESULT atcmd_listcmds(uint8_t nargs, char* argv[]);
static ATRESULT atcmd_reset(uint8_t nargs, char* argv[]) {
    // do reset
    RMMgr_reboot(RM_AT_ACTION);
    return ATCMD_OK;
}
static void printKey(uint16_t k) {
    uint8_t d[16];
    // get element max data length 16
    int l =  CFMgr_getElement(k, d, 16);
    switch(l) {
        case 0:
        case -1:  {
            wconsole_println("Key[%04x]=NOT FOUND", k);
            break;
        }
        case 1: {
            // print as decimal
            wconsole_println("Key[%04x]=%d", k, d[0]);
            break;
        }
        case 2: {
            // print as decimal
            wconsole_println("Key[%04x]=%d / 0x%04x", k, *(uint16_t *)(&d[0]), *(uint16_t *)(&d[0]));
            break;
        }
        case 4: {
            // print as decimal
            wconsole_println("Key[%04x]=%d / 0x%08x", k, *(uint32_t *)(&d[0]), *(uint32_t *)(&d[0]));
            break;
        }
        default: {
            // dump as hex
            char hs[33];
            for(int i=0;i<l;i++) {
                sprintf(&hs[i*2], "%02x", d[i]);
            }
            hs[l*2]='\0';
            wconsole_println("Key[%04x]=0x%s", k, hs);
            break;
        }
    }
}
static ATRESULT atcmd_getcfg(uint8_t nargs, char* argv[]) {
    // Check args - if 1 present then show just that config element else show all
    if (nargs>2) {
        return ATCMD_BADARG;
    }
    if (nargs==1) {
        // get al config elements and print them
        CFMgr_iterateKeys(-1, &printKey);
    } else if (nargs==2) {
        int k=0;
        int kl = strlen(argv[1]);
        if (kl==2) {
            // get 2 digit key module
            sscanf(argv[1], "%2x", &k);
            // and dump all keys with this module
            CFMgr_iterateKeys(k, &printKey);
        } else if (kl==4) {
            sscanf(argv[1], "%4x", &k);
            if (k!=0) {
                printKey(k);
            } else {
                wconsole_println("Unknown key [%04x]", k);
            }
        } else {
            wconsole_println("Error : Must give either key module as 2 digits, or full key of 4 digits");
            return ATCMD_BADARG;
        }
    }
    return ATCMD_OK;
}
static ATRESULT atcmd_setcfg(uint8_t nargs, char* argv[]) {
    // args : cmd, config key (always 4 digits hex), config value as hex string (0x prefix) or decimal
    if (nargs<3) {
        return ATCMD_BADARG;
    }
    // parse key
    if (strlen(argv[1])!=4) {
        wconsole_println("<Key> must be 4 digits");
        return ATCMD_BADARG;
    }
    int k=0;
    sscanf(argv[1], "%4x", &k);
    if (k!=0) {
        // parse value
        int l = CFMgr_getElementLen(k);
        switch(l) {
            case 0: {
                wconsole_println("bad key[%04x]");
                break;
            }
            case 1: 
            case 2:
            case 3:
            case 4: {
                int v = 0;
                char *vp = argv[2];
                if (*vp=='0' && *(vp+1)=='x') {
                    if (sscanf((vp+2), "%x", &v)<0) {
                        wconsole_println("Key[%04x] bad hex value:%s",k,vp);
                        return ATCMD_BADARG;
                    }
                } else {
                    if (sscanf(vp, "%d", &v)<0) {
                        wconsole_println("Key[%04x] bad dec value:%s",k,vp);
                        return ATCMD_BADARG;
                    }
                }
                CFMgr_setElement(k, &v, l);
                printKey(k);        // Show the value now in the config 
//                wconsole_println("Key[%04x]=%d",k,v);
                break;
            }

            default: {
                // parse as hex string
                char* vp = argv[2];
                // Skip 0x if user put it in
                if (*vp=='0' && *(vp+1)=='x') {
                    vp+=2;
                }
                //Check got enough digits
                if (strlen(vp)!=(l*2)) {
                    wconsole_println("Key[%04x]=%s incorrect length should be %d bytes", k, argv[2], l);
                    return ATCMD_BADARG;
                }
                if (l>16) {
                    wconsole_println("Key[%04x] has length %d but cannot set here (max 16)", k, l);
                    return ATCMD_BADARG;
                }
                // gonna allow up to 16 bytes
                uint8_t val[16];
                for(int i=0;i<l;i++) {
                    sscanf(vp, "%02x", (unsigned int*)(&val[i]));
                    vp+=2;
                }
                CFMgr_setElement(k, &val[0], l);
                printKey(k);
                break;
            }
        }
    }
    return ATCMD_OK;
}
static ATRESULT atcmd_getmods(uint8_t nargs, char* argv[]) {
    // Check args - if an arg present then show just that module else show all
    if (nargs>2) {
        return ATCMD_BADARG;
    }
    if (nargs==1) {
        for(int i=0;i<APP_MOD_LAST;i++) {
            wconsole_println("Module[%d] State[%s]", i, AppCore_getModuleState(i)?"ON":"OFF");
        }
    } else if (nargs==2) {
        int mid = atoi(argv[1]);
        wconsole_println("Module[%d] State[%s]", mid, AppCore_getModuleState(mid)?"ON":"OFF");
    }

    return ATCMD_OK;
}
static ATRESULT atcmd_setmod(uint8_t nargs, char* argv[]) {
    // args : cmd, module id, state 0 or 1
    if (nargs<3) {
        return ATCMD_BADARG;
    }
    // parse key
    int mid = atoi(argv[1]);
    if (mid<0 || mid>APP_MOD_LAST) {
        return ATCMD_BADARG;
    }
    // parse value
    if (strncmp(argv[2], "ON", 3)==0) {
        AppCore_setModuleState(mid, true);
    }  else {
        AppCore_setModuleState(mid, false);
    }
    wconsole_println("Module[%d] State[%s]", mid, AppCore_getModuleState(mid)?"ON":"OFF");

    // set and return result
    return ATCMD_OK;
}
static ATRESULT atcmd_runcycle(uint8_t nargs, char* argv[]) {
    wconsole_println("AppCore: Exit console, run data collection...");
    stopConsole();
    AppCore_forceUL(-1);
    return ATCMD_OK;
}

static ATRESULT atcmd_setlogs(uint8_t nargs, char* argv[]) {
    if (nargs<2) {
        return ATCMD_BADARG;
    }
    if (strcmp(argv[1], "DEBUG")==0) {
        wconsole_println("AppCore: Log level set to DEBUG");
        log_level(LOGS_DEBUG);
    } else if (strcmp(argv[1], "INFO")==0) {
        wconsole_println("AppCore: Log level set to INFO");
        log_level(LOGS_INFO);
    } else if (strcmp(argv[1], "RUN")==0) {
        wconsole_println("AppCore: Log level set to RUN (WARN+ERROR)");
        log_level(LOGS_RUN);
    } else if (strcmp(argv[1], "OFF")==0) {
        wconsole_println("AppCore: Log level set to OFF");
        log_level(LOGS_OFF);
    } else {
        wconsole_println("AppCore: unknown log level %d (must be DEBUG, INFO, RUN or OFF)", argv[1]);
        return ATCMD_BADARG;
    }
    return ATCMD_OK;
}
static ATRESULT atcmd_info(uint8_t nargs, char* argv[]) {
    // Display uptime, lora state, battery, last reboot reason, last assert etc etc
    wconsole_println("Uptime:%dh %dm", TMMgr_getRelTime()/3600000, TMMgr_getRelTime()/60000);
    wconsole_println("LoraJoin:%s", lora_api_isJoined()?"OK":"NOK");
    wconsole_println("Battery:%d", SRMgr_getBatterymV());
    wconsole_println("Light:%d", SRMgr_getLight());
    wconsole_println("ResetReason: %04x", RMMgr_getResetReasonCode());
    wconsole_println("Last assert [0x%08x]", RMMgr_getLastAssertCallerFn());
    return ATCMD_OK;
}
static ATCMD_DEF_t ATCMDS[] = {
    { .cmd="AT", .desc="Wakeup", atcmd_hello},
    { .cmd="AT+HELLO", .desc="Wakeup", atcmd_hello},
    { .cmd="AT+WHO", .desc="Dsplay card type", atcmd_who},
    { .cmd="AT+HELP", .desc="List commands", atcmd_listcmds},
    { .cmd="AT?", .desc="List commands", atcmd_listcmds},
    { .cmd="ATZ", .desc="Reset card", atcmd_reset},
    { .cmd="AT+INFO", .desc="Show info", atcmd_info},
    { .cmd="AT+GETCFG", .desc="Show config", atcmd_getcfg},
    { .cmd="AT&V", .desc="Show config", atcmd_getcfg},
    { .cmd="AT+SETCFG", .desc="Set config", atcmd_setcfg},
    { .cmd="AT+GETMODS", .desc="Show modules and states", atcmd_getmods},
    { .cmd="AT+SETMODS", .desc="Set module state", atcmd_setmod},
    { .cmd="AT+RUN", .desc="Go for active cycle immediately", atcmd_runcycle},
    { .cmd="AT+LOG", .desc="Set logging level", atcmd_setlogs},
};
static ATRESULT atcmd_listcmds(uint8_t nargs, char* argv[]) {
    uint8_t cl = sizeof(ATCMDS)/sizeof(ATCMDS[0]);
    for(int i=0;i<cl;i++) {
        wconsole_println("%s: %s", ATCMDS[i].cmd, ATCMDS[i].desc);
    }
    return ATCMD_OK;
}

void initConsole() {
    wconsole_mgr_init(MYNEWT_VAL(WCONSOLE_UART_DEV), MYNEWT_VAL(WCONSOLE_UART_BAUD), MYNEWT_VAL(WCONSOLE_UART_SELECT));
}
bool startConsole() {
    if (wconsole_isInit()) {
        uint8_t cl = sizeof(ATCMDS)/sizeof(ATCMDS[0]);
        log_debug("AC:console starts with %d commands", cl);
        // start with our command set, no idle timeout
        wconsole_start(cl, ATCMDS, 0);
        return true;
    }
    return false;       // not inited
}
void stopConsole() {
    wconsole_stop();
}
bool consoleIsInit() {
    return wconsole_isInit();
}

bool isConsoleActive() {
    return wconsole_isActive();
}