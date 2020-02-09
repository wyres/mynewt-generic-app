# Environment module app core Package Definition

This is the package containing the production post-flash test. 

The source files are located in the src/ directory.

Header files are located in include/ 

pkg.yml contains the base definition of the package.

Any questions?  Please refer to the documentation at 
http://mynewt.apache.org/ or ask questions on dev@mynewt.apache.org

Operation
---------

This module assumes that if it gets to the start() case, then the app-core must have managed to boot, test and join.
It starts the accelero/sensor packages to ensure that no issues exist with them and gets the env data.

It produces UL data with the env data (good or bad) and sends this to the backend to be checked and added to the endpoint info.

After doing this twice, it hangs flashing the leds in a 'success' pattern (slow flash both) to let the operator know its all good... 

If it detects errors with the sensors etc then it hangs flahsing its leds in a 'failure' pattern (fast flash alternating)

If the target sets the mynewt config flag:
 - 'DCARD_BLE', then it will test the dcard BLE operation is ok
 - 'DCARD_BLEGPS' then it will test the BLE/GPS dcard operation is ok

Targets
-------
Targets are of the form
wbasev<X>_prodtest_<region>_<dcard type>_dev
The base card version, region and dcard type are set as usual. The syscfg for the target will set appropriate BSP, lora and DCARD_BLE/DCARD_BLEGPS flags. Console may be disabled?
Only 'dev' targets exist as this allows potential debug via the logging etc.

Config
------
The card must be flashed with a config containing:
 - the special fixed production devEUI/appKey (0101/0103)
 - appEUI (0102) equal to the devEUI that will be assigned to the card (allows the card to include this in its report message to be identified)
 - idle times (0401/0402) set to 0
 - join config:
    - time set to 16s (0409)
    - retry attempts set to 5 (040E)
    - rapid retry time set to 5 (040D)
