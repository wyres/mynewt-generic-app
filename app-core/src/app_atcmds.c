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
#include "wyres-generic/movementmgr.h"
#include "wyres-generic/sensormgr.h"
#include "loraapi/loraapi.h"

#include "app-core/app_core.h"
#include "app-core/app_console.h"

/**
 *  AT Commands for appcore in idle mode
 */

// WConsole at commands we use
static ATRESULT atcmd_hello(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    (*pfn)("Hello.");
    return ATCMD_PROCESSED;
}
static ATRESULT atcmd_who(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    APP_CORE_FW_t* fwinfo = AppCore_getFwInfo();
    (*pfn)("AppCore:%s (%lu)", fwinfo->fwname, Util_hashstrn(fwinfo->fwname, MAXFWNAME));
    (*pfn)("Build v%d/%d.%d @%s", fwinfo->fwmaj, fwinfo->fwmin, fwinfo->fwbuild, fwinfo->fwdate);

    return ATCMD_PROCESSED;
}
// predef
static ATRESULT atcmd_listcmds(PRINTLN_t pfn, uint8_t nargs, char* argv[]);
static ATRESULT atcmd_reset(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    // do reset
    RMMgr_reboot(RM_AT_ACTION);
    return ATCMD_OK;
}
static void printKey(void* ctx, uint16_t k) {
    PRINTLN_t pfn = (PRINTLN_t)ctx;
    uint8_t d[16];
    // Must explicitly check for illegal key (0000) as used for error check ie assert in configmgr
    if (k==CFG_KEY_ILLEGAL) {
        (*pfn)("Key[%04x]=NOT FOUND", k);
        return;
    }
    // get element max data length 16
    int l =  CFMgr_getElement(k, d, 16);
    switch(l) {
        case 0:
        case -1:  {
            (*pfn)("Key[%04x]=NOT FOUND", k);
            break;
        }
        case 1: {
            // print as decimal
            (*pfn)("Key[%04x]=%d", k, d[0]);
            break;
        }
        case 2: {
            // print as decimal
            uint16_t* vp = (uint16_t *)(&d[0]);     // avoid the overly keen anti-aliasing check
            (*pfn)("Key[%04x]=%d / 0x%04x", k, *vp, *vp);
            break;
        }
        case 4: {
            // print as decimal
            uint32_t* vp = (uint32_t *)(&d[0]);     // avoid the overly keen anti-aliasing check
            (*pfn)("Key[%04x]=%d / 0x%08x", k, *vp, *vp);
            break;
        }
        default: {
            // dump as hex up to 16 bytes
            char hs[34]; // 16*2 + 1 (terminator) + 1 (for luck)
            int sz = (l<16?l:16);       // in case its longer!
            for(int i=0;i<sz;i++) {
                sprintf(&hs[i*2], "%02x", d[i]);
            }
            hs[sz*2]='\0';
            (*pfn)("Key[%04x]=0x%s", k, hs);
            break;
        }
    }
}
static ATRESULT atcmd_getcfg(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    // Check args - if 1 present then show just that config element else show all
    if (nargs>2) {
        return ATCMD_BADARG;
    }
    if (nargs==1) {
        // get al config elements and print them
        // Currently not possible as uart output buffer can't hold them all
//        CFMgr_iterateKeys(-1, &printKey);
        // Tell user about groups instead
        (*pfn)("Config modules available:");
        (*pfn)(" 00 - System");
        (*pfn)(" 01 - LoRaWAN");
        (*pfn)(" 04 - AppCore");
        (*pfn)(" 05 - AppMods"); 
    } else if (nargs==2) {
        int k=0;
        int kl = strlen(argv[1]);
        if (kl==2) {
            // get 2 digit key module
            if (sscanf(argv[1], "%2x", &k)<1) {
                (*pfn)("Bad module [%s] must be 2 digits", argv[1]);
                return ATCMD_BADARG;
            } else {
                // and dump all keys with this module
                CFMgr_iterateKeys(k, &printKey, (void*)pfn);
            }
        } else if (kl==4) {
            if (sscanf(argv[1], "%4x", &k)<1) {
                (*pfn)("Bad key [%s] must be 4 hex digits", argv[1]);
                return ATCMD_BADARG;
            } else {
                printKey(pfn, k);
            }
        } else {
            (*pfn)("Error : Must give either key module as 2 digits, or full key of 4 digits");
            return ATCMD_BADARG;
        }
    }
    return ATCMD_PROCESSED;
}
static ATRESULT atcmd_setcfg(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    // args : cmd, config key (always 4 digits hex), config value as hex string (0x prefix) or decimal
    if (nargs<3) {
        return ATCMD_BADARG;
    }
    // parse key
    if (strlen(argv[1])!=4) {
        (*pfn)("Key[%s] must be 4 digits", argv[1]);
        return ATCMD_BADARG;
    }
    int k=0;
    if (sscanf(argv[1], "%4x", &k)<1) {
        (*pfn)("Key[%s] must be 4 digits", argv[1]);
    } else {
        // parse value
        int l = CFMgr_getElementLen(k);
        switch(l) {
            case 0: {
                (*pfn)("Key[%s] does not exist", argv[1]);
                // TODO ? create in this case? maybe if a -c arg at the end? maybe not useful?
                break;
            }
            case 1: 
            case 2:
            case 3:
            case 4: {
                int v = 0;
                char *vp = argv[2];
                if (*vp=='0' && *(vp+1)=='x') {
                    if (strlen((vp+2))!=(l*2)) {
                        (*pfn)("Key[%04x]=%s value is incorrect length. (expected %d bytes)", k, argv[2], l);
                        return ATCMD_BADARG;
                    }
                    if (sscanf((vp+2), "%x", &v)<1) {
                        (*pfn)("Key[%04x] bad hex value:%s",k,vp);
                        return ATCMD_BADARG;
                    }
                } else {
                    if (sscanf(vp, "%d", &v)<1) {
                        (*pfn)("Key[%04x] bad dec value:%s",k,vp);
                        return ATCMD_BADARG;
                    }
                }
                CFMgr_setElement(k, &v, l);
                printKey(pfn, k);        // Show the value now in the config 
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
                    (*pfn)("Key[%04x]=%s value is incorrect length. (expected %d bytes)", k, argv[2], l);
                    return ATCMD_BADARG;
                }
                if (l>16) {
                    (*pfn)("Key[%04x] has length %d : cannot set (max 16)", k, l);
                    return ATCMD_BADARG;
                }
                // gonna allow up to 16 bytes
                uint8_t val[16];
                for(int i=0;i<l;i++) {
                    // sscanf into int (4 bytes) then copy just LSB as value for each byte
                    unsigned int b=0;
                    if (sscanf(vp, "%02x", &b)<1) {
                        (*pfn)("Key[%04x] bad hex : %s", k, vp);
                        return ATCMD_BADARG;
                    }
                    val[i] = b;
                    vp+=2;
                }
                CFMgr_setElement(k, &val[0], l);
                printKey(pfn, k);
                break;
            }
        }
    }
    return ATCMD_PROCESSED;
}
static ATRESULT atcmd_getmods(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    // Check args - if an arg present then show just that module else show all
    if (nargs>2) {
        return ATCMD_BADARG;
    }
    if (nargs==1) {
        for(int i=0;i<APP_MOD_LAST;i++) {
            (*pfn)("Module[%d][%s]: %s", i, AppCore_getModuleName(i), AppCore_getModuleState(i)?"ON":"OFF");
        }
    } else if (nargs==2) {
        int mid = atoi(argv[1]);
        if (mid<0 || mid>=APP_MOD_LAST) {
            (*pfn)("Module id [%s] out of range",argv[1]);
            return ATCMD_BADARG;
        } else {
            (*pfn)("Module[%d][%s]: %s", mid, AppCore_getModuleName(mid), AppCore_getModuleState(mid)?"ON":"OFF");
        }
    }

    return ATCMD_PROCESSED;
}
static ATRESULT atcmd_setmod(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    // args : cmd, module id, state 0 or 1
    if (nargs<3) {
        return ATCMD_BADARG;
    }
    // parse key
    int mid = atoi(argv[1]);
    if (mid<0 || mid>=APP_MOD_LAST) {
        (*pfn)("Module id [%s] out of range",argv[1]);
        return ATCMD_BADARG;
    }
    // parse value
    if (strncmp(argv[2], "ON", 3)==0) {
        AppCore_setModuleState(mid, true);
    } else if (strncmp(argv[2], "OFF", 3)==0) {
        AppCore_setModuleState(mid, false);
    }  else {
        (*pfn)("Bad state [%s]: must be ON or OFF",argv[1]);
        return ATCMD_BADARG;
    }
    (*pfn)("Module[%d][%s]: %s", mid, AppCore_getModuleName(mid), AppCore_getModuleState(mid)?"ON":"OFF");

    // set and return result
    return ATCMD_PROCESSED;
}
static ATRESULT atcmd_runcycle(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    (*pfn)("Exit console, run data collection...");
    stopConsole();
    AppCore_forceUL(-1);
    return ATCMD_OK;
}

static ATRESULT atcmd_setlogs(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    if (nargs>1) {
        if (strcmp(argv[1], "DEBUG")==0) {
            set_log_level(LOGS_DEBUG);
        } else if (strcmp(argv[1], "INFO")==0) {
            set_log_level(LOGS_INFO);
        } else if (strcmp(argv[1], "RUN")==0) {
            set_log_level(LOGS_RUN);
        } else if (strcmp(argv[1], "WARN")==0) {
            set_log_level(LOGS_RUN);
        } else if (strcmp(argv[1], "ERROR")==0) {
            set_log_level(LOGS_RUN);
        } else if (strcmp(argv[1], "OFF")==0) {
            set_log_level(LOGS_OFF);
        } else {
            (*pfn)("Unknown log level [%s] (must be DEBUG, INFO, RUN or OFF)", argv[1]);
            return ATCMD_BADARG;
        }
    }
    // And print new level
    (*pfn)("Log level: %s", get_log_level_str());
    return ATCMD_PROCESSED;
}
static ATRESULT atcmd_info(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    // Display fw info, lora state, battery, last reboot reason, last assert etc etc
    SRMgr_start();
    APP_CORE_FW_t* fwinfo = AppCore_getFwInfo();
    (*pfn)("FW:%s [%08x], v%d/%d.%d @%s ", fwinfo->fwname, Util_hashstrn(fwinfo->fwname, MAXFWNAME), 
        fwinfo->fwmaj, fwinfo->fwmin, fwinfo->fwbuild, fwinfo->fwdate);
    int hwrev = BSP_getHwVer();
    if (hwrev==0) {
        (*pfn)("HW:vProto");
    } else if (hwrev<9) {
        (*pfn)("HW:v2rev%c",hwrev==1?'B':(hwrev==2?'C':(hwrev==3?'D':'?')));
    } else {
        (*pfn)("HW:v3rev%c",hwrev==10?'A':(hwrev==11?'B':(hwrev==12?'C':'?')));
    }
    (*pfn)("Lora:Region %d Joined:%s", fwinfo->loraregion, lora_api_isJoined()?"YES":"NO");
    (*pfn)("Batt:%d", SRMgr_getBatterymV());
    (*pfn)("Light:%d", SRMgr_getLight());
    (*pfn)("Logs: %s", get_log_level_str());
    (*pfn)("LastReset: %04x", RMMgr_getResetReasonCode());
    (*pfn)("LastAssert:[0x%08x]", RMMgr_getLastAssertCallerFn());
    (*pfn)("LastWELog:[0x%08x]", RMMgr_getLogFn(0));
    SRMgr_stop();
    return ATCMD_PROCESSED;
}
static ATRESULT atcmd_selftest(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    // check hw elements
    // flash/eeprom ok or would not have started
    APP_CORE_FW_t* fwinfo = AppCore_getFwInfo();
    (*pfn)("FW:%s, v%d/%d.%d @%s", fwinfo->fwname, fwinfo->fwmaj, fwinfo->fwmin, fwinfo->fwbuild, fwinfo->fwdate);
    // Check altimeter/battery/temp are 'reasonable'
    SRMgr_start();
    // delay a little
    // check accelero  
    (*pfn)("ACCEL:%s", (MMMgr_start() && MMMgr_check())?"OK":"NOK");
    // ? SX1272?
    (*pfn)("BATT[%d]:%s",SRMgr_getBatterymV(), (SRMgr_getBatterymV()>2000 && SRMgr_getBatterymV()<4000)?"OK":"NOK");
    (*pfn)("ALTI[%d]:%s", SRMgr_getPressurePa(), (SRMgr_getPressurePa()>90000 && SRMgr_getPressurePa()<120000)?"OK":"NOK");
    (*pfn)("TEMP[%d]:%s", SRMgr_getTempcC(), (SRMgr_getTempcC()>-4000 && SRMgr_getTempcC()<9000)?"OK":"NOK");
    SRMgr_stop();
    return ATCMD_PROCESSED;
}
static ATRESULT atcmd_hexline(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    // fota operation
    if (nargs!=4) {
        return ATCMD_GENERR;
    }
    // Parse args
    uint32_t addr;
    unsigned int crc1byte;
    if (sscanf(argv[1], "%lx", &addr)!=1) {
        return ATCMD_BADARG;
    }
    if (sscanf(argv[3], "%x", &crc1byte)!=1) {
        return ATCMD_BADARG;
    }
    // Up to 16 bytes in the hex (but allowed to have less)
    static uint8_t _fotadata[16];
    int len = Util_scanhex(argv[2], 16, &_fotadata[0]);
    // But not 0
    if (len==0) {
        return ATCMD_BADARG;
    }
    // give to fota for validation and action
// TODO    if (fota_newData(addr, data, len, crc1byte)) { return ATCMD_OK;} else { return ATCMD_GENERR; }
    // test echo the line
    (*pfn)("%s %08x %s %02x", argv[0],addr,argv[2],crc1byte);
    // Ack with OK (serves as flow control also)
    return ATCMD_OK;
}

/** LORA control commands and callbacks */
static const char* lwRes2Str(LORAWAN_RESULT_t res) {
    switch (res)
    {
    case LORAWAN_RES_OK:
    case LORAWAN_RES_JOIN_OK:
        return "OK";
    case LORAWAN_RES_NOT_JOIN:
        return "NOK : not joined";
    case LORAWAN_RES_DUTYCYCLE:
        return "NOK : duty cycle limited";
    case LORAWAN_RES_OCC:
        return "NOK : duty cycle limited";
    case LORAWAN_RES_NO_BW:
        return "NOK : no bandwidth?";
    case LORAWAN_RES_TIMEOUT: // what does this imply?
        return "NOK : timeout waiting for result";
    case LORAWAN_RES_BADPARAM:
        return "NOK : param rejected";
    case LORAWAN_RES_FWERR:
        return "NOK : fw err";
    case LORAWAN_RES_HWERR:
        return "NOK : hw err";
    default:
        return "NOK : unknown result";
    }
}
static void lora_join_cb(void *userctx, LORAWAN_RESULT_t res)
{
    PRINTLN_t pfn = (PRINTLN_t)userctx;
    (*pfn)("lora JOIN cb : result:%s", lwRes2Str(res));
    // TODO add get via api of all the config returned by join accept
}
static void lora_tx_cb(void *userctx, LORAWAN_RESULT_t res)
{
    PRINTLN_t pfn = (PRINTLN_t)userctx;
    (*pfn)("TX: %s", lwRes2Str(res));
}

#define RX_PRINT_SZ (20)
static void lora_rx_cb(void *userctx, LORAWAN_RESULT_t res, uint8_t port, int rssi, int snr, uint8_t *msg, uint8_t sz)
{
    PRINTLN_t pfn = (PRINTLN_t)userctx;
    static char rxstr[RX_PRINT_SZ*2+2];      // First X bytes of pkt
    for(int i=0;i<sz && i<RX_PRINT_SZ;i++) {
        sprintf(&rxstr[i*2], "%02x", msg[i]);
    }
    (*pfn)("RX OK Port[%d] RSSI[%d] SNR[%d] sz[%d] [%s]", port, rssi, snr, sz, rxstr);
}

static ATRESULT atcmd_join(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    // if SF / ADR params given setup lora layer
    LORAWAN_SF_t sf = LORAWAN_SF10;
    bool adr = false;
    if (nargs>1) {
        sf = atoi(argv[1]);
    }
    if (nargs>2) {
        adr = (atoi(argv[2])==1);
    }
    // TODO
    // lora_api_set_ADR(adr);

    LORAWAN_RESULT_t status = lora_api_join(lora_join_cb, sf, pfn);
    if (status == LORAWAN_RES_JOIN_OK)
    {
        // already joined (?) seems unlikely so warn about it
        (*pfn)("JOIN: already joined?!?");
    }
    else if (status != LORAWAN_RES_OK)
    {
        // Failed to start join process... this isn't great
        (*pfn)("JOIN : tx attempt failed immediately (%s)", lwRes2Str(status));
    } else 
    {
        (*pfn)("JOIN: trying with ADR[%s]...", adr?"enabled":"disabled");
    }
    return ATCMD_PROCESSED;
}
static int parseHexString(const char* str, uint8_t* buf, int maxsz) {
    if (str==NULL || buf==NULL || maxsz<1) {
        return -1;      // no can do
    }
    int sz = strlen(str)/2;
    if (sz>maxsz) {
        sz = maxsz;
    }
    if (Util_scanhex(str, sz, buf)!=sz) {
        // not parseable as hex bytes right to the end
        log_warn("failed to parse [%s] as hex", str);
        return -1;
    }
    return sz;
}
#define TX_PRINT_SZ (20)        // max size of tx buffer in bytes
static ATRESULT atcmd_tx(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    LORAWAN_SF_t sf = LORAWAN_SF10;
    uint8_t txPort = 3;
    bool ack = false;
    uint8_t txbuf[TX_PRINT_SZ];
    uint8_t txsz = 0;
    // Parse params for hex payload, sf, ackrequest
    if (nargs<2) {
        return ATCMD_BADARG;
    }
    txsz = parseHexString(argv[1], txbuf, TX_PRINT_SZ);
    if (txsz<0) {
        (*pfn)("ERROR: failed to parse hex data [%s]", argv[1]);
        return ATCMD_BADARG;
    }
    if (txsz==TX_PRINT_SZ) {
        (*pfn)("WARNING: tx data truncated to %d bytes", txsz);
    }
    if (nargs>2) {
        txPort = atoi(argv[2]);
    }
    if (nargs>3) {
        sf = atoi(argv[3]);
    }
    if (nargs>4) {
        ack = (atoi(argv[4])==1);
    }
    LORAWAN_RESULT_t txres = lora_api_send(sf, txPort, ack, true,
                                               txbuf, txsz, lora_tx_cb, pfn);
    if (txres == LORAWAN_RES_OK) {
        (*pfn)("TX: trying...");
    } else {
        (*pfn)("TX: failed: %d", txres);
    }
    return ATCMD_PROCESSED;
}
static ATRESULT atcmd_rx(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    int8_t rxport = -1;
    // Parse specific port to listen to
    if (nargs>1) {
        rxport = atoi(argv[1]);
    }
    LORAWAN_RESULT_t rxres = lora_api_registerRxCB(rxport, lora_rx_cb, pfn);
    if (rxres==LORAWAN_RES_OK) {
        (*pfn)("RX enabled on port[%d]", rxport);
    } else {
        (*pfn)("RX failed to enable on port [%d]", rxport);
    }
    return ATCMD_PROCESSED;
}
static ATRESULT atcmd_linfo(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    // Get and print much lora stuff
    (*pfn)("LoRa: Region[%d]", lora_api_getCurrentRegion());
    // devEUI/appEUI/appKey
//    uint8_t* deveui;
    //deveui = lora_api_get_devEUI();
//    (*pfn)("LoRa: devEUI[%02x%02x%02x%02x%02x%02x%02x%02x]", 
//        deveui[0], deveui[1], deveui[2], deveui[3], deveui[4], deveui[5], deveui[6], deveui[7]);

    // join status
    (*pfn)("LoRa Status: JOINED[%s]", lora_api_isJoined()?"YES":"NO");
    // current SF, tx power, ADR status
//    (*pfn)("LoRa SF[%d] TXPower[%d] ADR[%d]", lora_api_get_sf(), lora_api_get_txpower(), lora_api_get_adr());
    // devaddr/newkskey/appskey
//    (*pfn)("LoRa devAddr[%06x] nwkSkey[%08x] appSkey[%08x]", lora_api_get_devAddr(), lora_api_get_nwkSkey(), lora_api_get_appSkey());
    // fcnt up, down
//    (*pfn)("LoRa fcntUL[%d] fcntDL[%d]", lora_api_get_fcntUL(), lora_api_get_fcntDL());
    return ATCMD_PROCESSED;
}

static ATCMD_DEF_t ATCMDS[] = {
    { .cmd="AT", .desc="Wakeup", atcmd_hello},
    { .cmd="AT+HELLO", .desc="Wakeup", atcmd_hello},
    { .cmd="AT+WHO", .desc="Dsplay card type", atcmd_who},
    { .cmd="AT+HELP", .desc="List commands", atcmd_listcmds},
    { .cmd="AT?", .desc="List commands", atcmd_listcmds},
    { .cmd="ATZ", .desc="Reset card", atcmd_reset},
    { .cmd="AT+INFO", .desc="Show info", atcmd_info},
    { .cmd="AT+ST", .desc="HW self test", atcmd_selftest},
    { .cmd="AT+GETCFG", .desc="Show config", atcmd_getcfg},
    { .cmd="AT&V", .desc="Show config", atcmd_getcfg},
    { .cmd="AT+SETCFG", .desc="Set config", atcmd_setcfg},
    { .cmd="AT+GETMODS", .desc="Show modules and states", atcmd_getmods},
    { .cmd="AT+SETMODS", .desc="Set module state", atcmd_setmod},
    { .cmd="AT+RUN", .desc="Go for active cycle immediately", atcmd_runcycle},
    { .cmd="AT+LOG", .desc="Set logging level", atcmd_setlogs},
    { .cmd="AT+H", .desc="FOTA hex download", atcmd_hexline},
    { .cmd="AT+JOIN", .desc="LoRa JOIN", atcmd_join},
    { .cmd="AT+TX", .desc="LoRa TX", atcmd_tx},
    { .cmd="AT+RX", .desc="LoRa RX", atcmd_rx},
    { .cmd="AT+LINFO", .desc="LoRa info", atcmd_linfo},
};
static ATRESULT atcmd_listcmds(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    uint8_t cl = sizeof(ATCMDS)/sizeof(ATCMDS[0]);
    for(int i=0;i<cl;i++) {
        (*pfn)("%s: %s", ATCMDS[i].cmd, ATCMDS[i].desc);
    }
    return ATCMD_PROCESSED;
}

// Allow a function to execute an at command found in argv[0] of the parsed command line, and output to the given println fn
ATRESULT execConsoleCmd(PRINTLN_t pfn, uint8_t nargs, char* argv[]) {
    // find it in the list
    uint8_t cl = sizeof(ATCMDS)/sizeof(ATCMDS[0]);
    for(int i=0;i<cl;i++) {
        if (strcmp(argv[0], ATCMDS[i].cmd)==0) {
            // gotcha
            log_debug("got cmd %s with %d args", argv[0], nargs);
            // call the specific command processor function as registered
            return (*ATCMDS[i].fn)(pfn, nargs, argv);
        }
    }
    // not found
    return ATCMD_GENERR;
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