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

