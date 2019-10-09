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
#include "app-core/app_core.h"



// return true if parity is even, false if not for the given byte
static bool evenParity(uint8_t d) {
    bool ret=true;
    for(int i=0;i<8;i++) {
        if (d & (1<<i)) {
            ret = !ret;     // if checked bit is 1, i,nvert result
        }
    }
    return ret;
}
void app_core_msg_ul_init(APP_CORE_UL_t* ul) {
    memset(ul, 0, sizeof(APP_CORE_UL_t));
    ul->msgNbFilling=0;     // which message are we currently filling in?
    ul->msbNbTxing=-1;      // which message are we currently txing: -1as we inc in finalise before starting
    ul->msgs[ul->msgNbFilling].sz = 2;     // skip header which we add later
}
// Add TLV into payload if possible
bool app_core_msg_ul_addTLV(APP_CORE_UL_t* ul, uint8_t t, uint8_t l, void* v) {
    if ((ul->msgs[ul->msgNbFilling].sz + l + 2) > APP_CORE_UL_MAX_SZ) {
        ul->msgNbFilling++;
        if (ul->msgNbFilling>= APP_CORE_UL_MAX_NB) {
            // too full
            return false;
        }
        ul->msgs[ul->msgNbFilling].sz = 2;     // skip header which we add later
    }
    ul->msgs[ul->msgNbFilling].payload[ul->msgs[ul->msgNbFilling].sz++] = t;
    ul->msgs[ul->msgNbFilling].payload[ul->msgs[ul->msgNbFilling].sz++] = l;
    uint8_t* vd = (uint8_t*)v;
    for(int i=0;i<l;i++) {
        ul->msgs[ul->msgNbFilling].payload[ul->msgs[ul->msgNbFilling].sz++] = vd[i];
    }
    return true;
}
// Add TL and return pointer to V space unless too full
uint8_t* app_core_msg_ul_addTLgetVP(APP_CORE_UL_t* ul, uint8_t t, uint8_t l) {
    if ((ul->msgs[ul->msgNbFilling].sz + l + 2) > APP_CORE_UL_MAX_SZ) {
        ul->msgNbFilling++;
        if (ul->msgNbFilling>= APP_CORE_UL_MAX_NB) {
            // too full
            return false;
        }
        ul->msgs[ul->msgNbFilling].sz = 2;     // skip header which we add later
    }
    ul->msgs[ul->msgNbFilling].payload[ul->msgs[ul->msgNbFilling].sz++] = t;
    ul->msgs[ul->msgNbFilling].payload[ul->msgs[ul->msgNbFilling].sz++] = l;
    uint8_t* vp = &ul->msgs[ul->msgNbFilling].payload[ul->msgs[ul->msgNbFilling].sz];
    ul->msgs[ul->msgNbFilling].sz+=l;
    return vp;
}

// return the size of the final UL
uint8_t app_core_msg_ul_finalise(APP_CORE_UL_t* ul, uint8_t lastDLId, bool willListen) {
    uint8_t ret = 0;
    ul->msbNbTxing++;
    if (ul->msbNbTxing<APP_CORE_UL_MAX_NB) {
        // 2 byte fixed header: 
        //	0 : b0-3: ULrespid, b4-5: protocol version (0), b6: 1=listening for DL, 0=not listening, b7: force even parity for this byte
        //	1 : length of following TLV block
        //	- allows backend to reliably (mostly) detect this type of message - if 1st byte parity=0 and 2nd byte value+2=message length then its probably this format....
        //	- 00 00 is the most basic valid message
        ul->msgs[ul->msbNbTxing].payload[0] = (lastDLId & 0x0f) | 0x00 | (willListen?0x40:0x00);
        if (!evenParity(ul->msgs[ul->msbNbTxing].payload[0])) {
            ul->msgs[ul->msbNbTxing].payload[0] |= 0x80;        // not even, add parity bit
        }
        ul->msgs[ul->msbNbTxing].payload[1] = (ul->msgs[ul->msbNbTxing].sz)-2;      // length of the TLV section
        ret = ul->msgs[ul->msbNbTxing].sz;
        // Must have msgNbTxing pointing to the message we have finalised
    } // else we're done tx 
    return ret;
}

void app_core_msg_dl_init(APP_CORE_DL_t* dl) {
    memset(dl, 0, sizeof(APP_CORE_DL_t));
    dl->sz = 0;
}
// Decode DL message but don't do anything with content
bool app_core_msg_dl_decode(APP_CORE_DL_t* msg) {
    // decode TLVs to end to check its an ok message
    if (!evenParity(msg->payload[0])) {
        return false;       // not even parity
    }
    // Check protocol version
    uint8_t pver = ((msg->payload[0] >> 4) & 0x03);
    if (pver==0) {
        // dlid in [1] in top 4 bits, nb actions in bottom 4
        msg->dlId = ((msg->payload[1] >> 4) & 0x0f);
        msg->nbActions = (msg->payload[1] & 0x0f);
        return true;        // all decoded ok
    }
    // Don't know about any other versions
    return false;
}
// Execute a DL message
bool app_core_msg_dl_execute(APP_CORE_DL_t* msg) {
    // decode TLVs and execute relevant actions
    int curoff = 2;
    for(int i=0; i< msg->nbActions; i++) {
        uint8_t action = msg->payload[curoff++];
        uint8_t len = msg->payload[curoff++];
        if (curoff+len > msg->sz) {
            // badness - nb actions didn't match length of packet
            log_debug("DLA exec : %d len %d @ %d > %d", action, len, curoff, msg->sz);
            return false;
        }
        ACTIONFN_t afn = AppCore_findAction(action);
        if (afn!=NULL) {
            (*afn)(&msg->payload[curoff], len);
        }
        curoff+=len;
    }

    log_debug("DLA exec %d actions", msg->nbActions);
    return true;        // all executed ok
}

#ifdef UNITTEST
// TODO add unit tests of encoding DMs
#endif /* UNITTEST */