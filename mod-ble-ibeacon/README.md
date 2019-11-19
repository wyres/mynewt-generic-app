# BLE generic application modules Package Definition

This is the package containing the generic firmware module for BLE operation for navigation 

The source files are located in the src/ directory.

Header files are located in include/ 

pkg.yml contains the base definition of the package.

Any questions?  Please refer to the documentation at 
http://mynewt.apache.org/ or ask questions on dev@mynewt.apache.org

mod-ble-ibeacon
-------

This makes the BLE module into an ibeacon. The UUID/major/minor/period of emission/txPower are set in the PROM and read at startup or if changed via the DL or AT commands.

Useful Config keys:
------------------
0513 : emission period in millisecs

