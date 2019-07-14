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
#define APP_CORE_MSG_MAX_SZ (50)
#define APP_CORE_MSG_MAX_NB (4)     // up to 4 messages per round
#define LORAWAN_UL_PORT 3
#define LORAWAN_DL_PORT 3

// 1st 2 bytes are header, then TLV blocks (1 byte T, 1byte L, n bytes V)
// byte 0 : b0-3 : lastDLId, b4-5 : protocol version, b6 : config stock, b7 : even parity bit
// byte 1 : length of following TLV section
typedef struct {
    struct {
        uint8_t payload[APP_CORE_MSG_MAX_SZ];
        uint8_t sz;
    } msgs[APP_CORE_MSG_MAX_NB];
    uint8_t msgNbFilling;
    uint8_t msbNbTxing;
} APP_CORE_UL_t;
typedef struct {
    uint8_t payload[APP_CORE_MSG_MAX_SZ];
    uint8_t type;
    uint8_t sz;
} APP_CORE_DL_t;

void app_core_msg_ul_init(APP_CORE_UL_t* msg);
bool app_core_msg_ul_addTLV(APP_CORE_UL_t* msg, uint8_t t, uint8_t l, void* v);
uint8_t* app_core_msg_ul_addTLgetVP(APP_CORE_UL_t* ul, uint8_t t, uint8_t l) ;
uint8_t app_core_msg_ul_finalise(APP_CORE_UL_t* msg);
void app_core_msg_dl_init(APP_CORE_DL_t* msg);
bool app_core_msg_dl_decode(APP_CORE_DL_t* msg);
bool app_core_msg_dl_execute(APP_CORE_DL_t* msg);

#ifdef __cplusplus
}
#endif

#endif  /* H_APP_MSG_H */
