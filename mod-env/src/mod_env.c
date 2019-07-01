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
 * Environment sensor handling generic app module
 */

#include "os/os.h"

#include "wyres-generic/wutils.h"

#include "app-core/app_core.h"
#include "app-core/app_msg.h"
#include "mod-env/mod_env.h"

// My api functions
static uint32_t start() {
    log_debug("start env data collection");
    return 1*1000;
}

static void stop() {
    log_debug("done with env data collection");
}
static void sleep() {

}
static void deepsleep() {

}
static void getData(APP_CORE_UL_t* ul) {
    log_debug("added env data collection to UL");
}

static APP_CORE_API_t _api = {
    .startCB = &start,
    .stopCB = &stop,
    .sleepCB = &sleep,
    .deepsleepCB = &deepsleep,
    .getULDataCB = &getData,    
};
// Initialise module
void mod_env_init(void) {
    // hook app-core for ble scan - serialised as competing for UART
    AppCore_registerModule(APP_MOD_ENV, &_api, EXEC_PARALLEL);
//    log_debug("mod-env initialised");
}