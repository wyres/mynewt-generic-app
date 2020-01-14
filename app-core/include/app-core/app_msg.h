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
#ifndef H_APP_MSG_H
#define H_APP_MSG_H

#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// message definitions for uplink and downlink
#define APP_CORE_UL_MAX_SZ (50)     // so always fits
#define APP_CORE_UL_MAX_NB (4)     // up to 4 UL messages per round to get 200 bytes
#define APP_CORE_DL_MAX_SZ (250)    // as we don't control it
#define LORAWAN_UL_PORT 3
#define LORAWAN_DL_PORT 3
#define APP_CORE_MSGS_VERSION_UL (1)        // our first usable version is v1
#define APP_CORE_MSGS_VERSION_DL (0)

// UL Message : 1st 2 bytes are header, then TLV blocks (1 byte T, 1byte L, n bytes V)
// 2 byte fixed header: 
//	0 : b0-3: UL respid, b4-5: protocol version, b6: 1=listening for DL, 0=not listening, b7: force even parity for this byte
//	1 : length of following TLV block
typedef struct {
    struct {
        uint8_t payload[APP_CORE_UL_MAX_SZ];
        uint8_t sz;
    } msgs[APP_CORE_UL_MAX_NB];
    uint8_t msgNbFilling;
    uint8_t msbNbTxing;
} APP_CORE_UL_t;

// First 2 bytes are header, then 'actions'
// Byte 0 : b0-3 : msgtype = 0x6, b4-5 : protocol version, b6 : RFU, b7 : even parity bit
// byte 1 : b0-3 : number of elements TLV in this message (not the length in bytes), b4-7 : dlId for this DL
typedef struct {
    uint8_t payload[APP_CORE_DL_MAX_SZ];
    uint8_t dlId;
    uint8_t nbActions;
    uint8_t sz;     // in bytes of message
} APP_CORE_DL_t;

typedef void (*ACTIONFN_t)(uint8_t* v, uint8_t l);
typedef struct {
    uint8_t id;
    ACTIONFN_t fn;
} ACTION_t;

void app_core_msg_ul_init(APP_CORE_UL_t* msg);
bool app_core_msg_ul_addTLV(APP_CORE_UL_t* msg, uint8_t t, uint8_t l, void* v);
uint8_t* app_core_msg_ul_addTLgetVP(APP_CORE_UL_t* ul, uint8_t t, uint8_t l) ;
/*
 * get the max continugous data block we know how to send in UL
 * <returns>Returns maximum continuous block size in bytes that a message can ever hold</returns>
 */
uint8_t app_core_msg_ul_maxBlockSz();
/*
 * Return number of bytes still available in this UL
 * <returns>Returns remaining size in bytes that this message can hold</returns>
 */
uint8_t app_core_msg_ul_remainingSz(APP_CORE_UL_t* ul);
/*
 * Force switch to next UL, and return number of bytes allowed in it
 * returns 0 if no more ULs available... (and does NOT switch in this case in case someelse wants to use them)
 * */
uint8_t app_core_msg_ul_requestNextUL(APP_CORE_UL_t* ul);
/*
 * get total space available cumulated in al the remaining UL messages available
 */
uint8_t app_core_msg_ul_getTotalSpaceAvailable(APP_CORE_UL_t* ul);
/*
 * Step back 1 in current tx set to allow next finalise call to resend it 
 */
void app_core_msg_ul_retry(APP_CORE_UL_t* ul);
/* 
 * finalise next UL tx message (header etc) ready for tx
 */
uint8_t app_core_msg_ul_finalise(APP_CORE_UL_t* msg, uint8_t lastDLId, bool willListen);
/*
 * Get pointer to payload for current 'to tx' message
 */
uint8_t* app_core_msg_ul_getTxPayload(APP_CORE_UL_t* ul);

void app_core_msg_dl_init(APP_CORE_DL_t* msg);
bool app_core_msg_dl_decode(APP_CORE_DL_t* msg);
bool app_core_msg_dl_execute(APP_CORE_DL_t* msg);

#ifdef __cplusplus
}
#endif

#endif  /* H_APP_MSG_H */
