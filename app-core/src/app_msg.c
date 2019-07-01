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
 * This module builds an app-core UL format message 
 */

#include <string.h>
#include <assert.h>
#include "os/os.h"

#include "wyres-generic/wutils.h"
#include "app-core/app_msg.h"

// ul response id : holds the last DL id we received. Sent in each UL to inform backend we got its DLs
static uint8_t _ulRespId=0;

// Generate 1 or 0 to create even parity for the given byte
static uint8_t evenParity(uint8_t d) {
    uint8_t ret=0x00;
    for(int i=0;i<8;i++) {
        if (d & (1<<i)) {
            ret ^= 0x01;     // flip the bit with xor if checked bit is 1
        }
    }
    return ret;
}
void app_core_msg_ul_init(APP_CORE_UL_t* ul) {
    memset(ul, 0, sizeof(APP_CORE_UL_t));
    ul->sz = 2;     // skip header which we add later
}
bool app_core_msg_ul_addTLV(APP_CORE_UL_t* ul, uint8_t t, uint8_t l, void* v) {
    if ((ul->sz + l + 2) > APP_CORE_MSG_MAX_SZ) {
        return false;
    }
    ul->payload[ul->sz++] = t;
    ul->payload[ul->sz++] = l;
    uint8_t* vd = (uint8_t*)v;
    for(int i=0;i<l;i++) {
        ul->payload[ul->sz++] = vd[i];
    }
    return true;
}

// return the size of the final UL
uint8_t app_core_msg_ul_finalise(APP_CORE_UL_t* ul) {
    // 2 byte fixed header: 
	//	0 : b0-3: ULrespid, b4-5: protocol version (0), b6: RFU(0), b7: force even parity for this byte
	//	1 : length of following TLV block
	//	- allows backend to reliably (mostly) detect this type of message - if 1st byte parity=0 and 2nd byte value+2=message length then its probably this format....
	//	- 00 00 is the most basic valid message
    ul->payload[0] = (_ulRespId & 0x0f) | 0x00 | 0x00;
    ul->payload[0] |= evenParity(ul->payload[0]);
    ul->payload[1] = (ul->sz)-2;      // length of the TLV section
    return ul->sz;
}

void app_core_msg_dl_init(APP_CORE_DL_t* dl) {
    memset(dl, 0, sizeof(APP_CORE_DL_t));
    dl->sz = 0;
}
// Decode DL message but don't do anything with content
bool app_core_msg_dl_decode(APP_CORE_DL_t* msg) {
    // TODO - decode TLVs to end to check its an ok message
    return true;        // all decoded ok
}
// Execute a DL message
bool app_core_msg_dl_execute(APP_CORE_DL_t* msg) {
    // TODO - decode TLVs and execute relevant actions
    return true;        // all executed ok
}

#ifdef UNITTEST
// TODO add unit tests of encoding DMs
#endif /* UNITTEST */