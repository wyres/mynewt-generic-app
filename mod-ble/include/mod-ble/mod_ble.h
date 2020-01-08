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

#ifdef __cplusplus
}
#endif

#endif  /* H_MOD_BLE_H */
