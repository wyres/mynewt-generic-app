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

#ifndef H_MOD_BLE_H
#define H_MOD_BLE_H

#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_TYPE_NAV   (0x00)
#define BLE_TYPE_COUNTABLE_START   (0x01)
#define BLE_TYPE_COUNTABLE_END   (0x7F)
#define BLE_TYPE_ENTEREXIT   (0x80)
#define BLE_TYPE_PRESENCE   (0x81)
#define BLE_TYPE_PROXIMITY   (0x82)
#define UUID_SZ (16)

// BLE error bitmasks
// BLE comm failed
#define EM_BLE_COMM_FAIL    (0x01)
// BLE scanner gave us majors we didn't ask for
#define EM_BLE_RX_BADMAJ    (0x02)
// No space (no next UL or no space in this UL)
#define EM_UL_NONEXTUL      (0x04)
#define EM_UL_NOSPACE       (0x08)
// BLE historical table is full (which may lead to missing targets in scan)
#define EM_BLE_TABLE_FULL   (0x10)


#ifdef __cplusplus
}
#endif

#endif  /* H_MOD_BLE_H */
