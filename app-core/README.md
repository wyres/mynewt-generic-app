# Generic application package

This is the package containing the generic application core.

The source files are located in the src/ directory.

Header files are located in include/ 

pkg.yml contains the base definition of the package.

Any questions?  Please refer to the documentation at 
http://mynewt.apache.org/ or ask questions on dev@mynewt.apache.org
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

The app-core framework codifies a standard state machine for the code operation of a lora based device.
It has the 'app-core' control, and 1 or more 'mod-X' packages which define independant modules,
which will be called at the appropriate points to collect their data ready for UL.

The 'app-core' package governs the central execution, with 5 specific phases:
- phase 1 : init and startup
This initialisation phase, using the mynewt sysinit mechanisme, to allow the 'modules' to be initialised
based on their pkg.yml sysinit values, and to register with the app-core system as both modules to be
executed and as the destination of DL actions. Registeration of a module essentially consists of it indicating the set of 
functions that implement the API required.
Once sysinit has finished, app-core state machine enters the 'startup' state, which activates the console for a 
specific period of time for config AT commands. Once expired, the SM goes to the 1st collection cycle state.
- phase 2a : data collection from modules that must be executed in 'serial' mode ie when no other module is also executing.
This lets a module be sure its use of hardware specific elements is not in competition with other modules. The execution time 
each module requires is returned from its start() method, although a module can indicate an earlier termination at any time.
- phase 2b : data collection from modules that can execute in parallel. This lasts as long as the longest module timeout.
- phase 3 : lorawan UL : the collected data in 1 or more messages is sent as UL messages. Any DL packet received is decoded and the actions within are interpreted.
- phase 4 : idleness : the machine sleeps globally for the configured amount of time. It actually wakes every 60s and checks for movement as the idle time may be set differently for the moving/not moving cases.
A module may also register a 'tic' hook ie a function which is called during these regular wakeups to perform an action.
During the first 10s of the idle phase, if configured, the console is active for AT commands. The rest of the time it is inactive to achieve the lowest current consumation.

LoRa Operation
--------------
The KLK wrapper round the stackforce stack is re-wrapped by the loraapp.c code in generic. Note that this auto-joins if not already joined on the first UL.

An UL is not neccessarily sent every data collection loop - each module indicates if it has 'critical' data in its collection, and if no module is critical then no UL is sent. The config key MAXTIME_UL_MIN (0405) sets the maximum time that can elapse without an uplink (default 120 minutes) after which the UL data is sent anyway.

AT Command console
-------------------
The AppCore console is activated for all build profiles for 30s post-boot on the standard UART interface. If no 'AT' command
is received during this time, it transitions to the usual operation. If an AT command is received, the code stays permanently in the console, and the user must explicitly exit either via a reboot (ATZ) or a run (AT+RUN).

Useful AT commands:
- AT - wake the console
- AT+HELP - list the available commands
- ATZ - reboot
- AT+RUN - execute the data collection loop
- AT+INFO - some basic card info
- AT+GETCFG <config group> - show config keys for this group
- AT+SETCFG <4 digit key> <value> - set a config value
- AT+GETMODS/AT+SETMODS - see/change the set of activated modules. See app_core.h for the module ids.

The console is also active for 10s at the start of each idle period (signalled by 1Hz flash of both leds)

AppCore module config keys
---------------------------
See app_core.h for the list. Some key ones:

| module | config identifier | length |  description | 
| --------: | :--------: | :--------:  | :--------: |
| UTIL | 0001 | 8 | Reboot reason |
| UTIL | 0002 | 64 | store fn tracker buffer in case of reboot |
| UTIL | 0003 | 4 | Assert caller |
| LORA | 0101 | - | Dev EUI |
| LORA | 0102 | - | App EUI |
| LORA | 0103 | - | App KEY |
| LORA | 0104 | - | Dev ADDR |
| LORA | 0105 | - | Network session key |
| LORA | 0106 | - | App session key |
| LORA | 0107 | - | ADR enabled |
| LORA | 0108 | - | Acknoledgement |
| LORA | 0109 | - | data rate |
| LORA | 0110 | - | tx power |
| LORA | 0111 | - | port for TX |
| LORA | 0112 | - | port for RX |
| APP | 0201 | - | CFG_UTIL_KEY_CHECKINTERVAL repos\generic\generic\include\wyres-generic\appConfigKeys.h  used ? |
| WYRES | 0301 | - | CFG_WYRES_KEY_TAG_SYNC_DM_INTERVAL  used ? |
| WYRES | 0302 | - | CFG_WYRES_KEY_LORA_TXPOWER  used ? |
| WYRES | 0303 | - | CFG_WYRES_KEY_BLE_SCANWINDOW  used ? |
| WYRES | 0304 | - | CFG_WYRES_KEY_TAG_NONSYNC_DM_INTERVAL  used ? |
| WYRES | 0305 | - | CFG_WYRES_KEY_FOTA_VER  used ? |
| APP_CORE | 0401 | - | idle time when moving (in seconds) |
| APP_CORE | 0402 | - | idle time not moving (in minutes) |
| APP_CORE | 0403 | - | MODSETUP_TIME_SECS |
| APP_CORE | 0404 | - | MODS_ACTIVE_MASK |
| APP_CORE | 0405 | - | Maximal time between uplink in minutes |
| APP_CORE | 0406 | - | Downlink id (dlid) |
| APP_CORE | 0407 | - | idle period check time (in seconds, 60s default) |
| APP_MOD | 0501 | - | BLE scan duration un ms |
| APP_MOD | 0502 | - | GPS cold time in seconds |
| APP_MOD | 0503 | - | GPS warm time in seconds |
| APP_MOD | 0504 | - | GPS power mode |
| APP_MOD | 0505 | - | GPS fix mode |


DL Action handling
------------------
App-core handles the reception and decoding of the DL packets. These consist of a set of 'actions', each with a 1 byte key (defined in app_core.h). Modules can register to execute specific action keys at startup - only 1 module can register for each key and the system will assert() if more than one tries.
Most of the core actions are handled by the app_core.c file, including reset, get/setcfg and setting UTCTime.
