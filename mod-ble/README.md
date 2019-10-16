# BLE generic application modules Package Definition

This is the package containing the generic firmware module for BLE operation for navigation and/or tag scanning

The source files are located in the src/ directory.

Header files are located in include/ 

pkg.yml contains the base definition of the package.

Any questions?  Please refer to the documentation at 
http://mynewt.apache.org/ or ask questions on dev@mynewt.apache.org

mod-ble
-------

Two modules are defined in this package. Both use the BLE scanner hardware via the UART, but deal with the received ids differently.

mod_ble_scan_nav [module id = 2] : scans for BLE ibeacons with the MSB of the major=0 ie navigation use. 
After the scan time, it selects the top 3 RSSI and sends these in the UL.

mod_ble_scan_tag [module id = 3]: scans for BLE beacons with the MSB of the major !=0, ie both enter/exit and count types.
It takes all the found ids, and creates the enter/exit list based on a list it keeps between scans, and the count of each type. These are added to the UL packets. If there is too much data for the UL, currently no action is taken to avoid losing data...

Useful Config keys:
------------------
0501 : scan time in millisecs

