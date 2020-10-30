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
#include "wyres-generic/uartlinemgr.h"
#include "wyres-generic/timemgr.h"
#include "wyres-generic/rebootmgr.h"
#include "wyres-generic/uartselector.h"

/**
 *  create devices for app level. Called once base system is up, but before modules/core are initialised
 */
void app_core_init(void) {
    bool res = true;
    // initialise devices that are common to device (note its ok if this is done elsewhere also)
#if MYNEWT_VAL(UART_0)
    res=uart_line_comm_create(UART0_DEV, 19200);        // specific users of the uart will change the baud rate as required
    assert(res);
#endif
#if MYNEWT_VAL(UART_1)
    res=uart_line_comm_create(UART1_DEV, 19200);        // specific users of the uart will change the baud rate as required
    assert(res);
#endif
#if MYNEWT_VAL(UART_2)
    res=uart_line_comm_create(UART2_DEV, 19200);        // specific users of the uart will change the baud rate as required
    assert(res);
#endif
#if MYNEWT_VAL(UART_DBG)
    res=uart_line_comm_create(UARTDBG_DEV, 19200);        // specific users of the uart will change the baud rate as required
    assert(res);
#endif
    log_info("app init - reset %04x, last assert at [0x%08x], res[%d]", 
        RMMgr_getResetReasonCode(), 
        RMMgr_getLastAssertCallerFn(), res);

}
