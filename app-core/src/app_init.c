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
#include "wyres-generic/L96I2Ccomm.h"
#include "wyres-generic/uartLineMgr.h"
#include "wyres-generic/timemgr.h"
#include "wyres-generic/rebootMgr.h"
#include "wyres-generic/uartSelector.h"

/**
 *  create devices for app level. Called once base system is up, but before modules/core are initialised
 */
void app_core_init(void) {
    // no console
#if MYNEWT_VAL(CONSOLE_UART) 
    log_init_console(true);
#else
    log_init_console(false);
#endif
    // initialise devices
//    assert(L96_I2C_comm_create("L96_0", "I2C0", MYNEWT_VAL(L96_0_I2C_ADDR), MYNEWT_VAL(L96_0_PWRIO)));
    assert(uart_line_comm_create(UART0_DEV, MYNEWT_VAL(GPS_UART_BAUDRATE)));
    // If specific device for logging, create its wskt driver driver
#if (MYNEWT_VAL(UART_DBG))
    assert(uart_line_comm_create(UARTDBG_DEV, MYNEWT_VAL(LOG_UART_BAUDRATE)));
#endif
    // If logging to a uart is required, tell logging system
#if (MYNEWT_VAL(LOG_UART_ENABLED))
    log_init_uart(MYNEWT_VAL(LOG_UART), MYNEWT_VAL(LOG_UART_BAUDRATE), MYNEWT_VAL(LOG_UART_SELECT));       // Uart define names are as per STM32 doc
#endif
//    log_init_dbg(0);      // dont do blocking tx please

    log_warn("app init - reset %s, assert from [0x%08x]", 
        RMMgr_getResetReason(), 
        RMMgr_getLastAssertCallerFn());

}
