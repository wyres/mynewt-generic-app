# BLE generic application modules Package Definition

This is the package containing the generic firmware module for BLE operation for navigation 

The source files are located in the src/ directory.

Header files are located in include/ 

pkg.yml contains the base definition of the package.

Any questions?  Please refer to the documentation at 
http://mynewt.apache.org/ or ask questions on dev@mynewt.apache.org

mod-ble-scan-nav
-------

mod_ble_scan_nav [module id = 2] : scans for BLE ibeacons with the MSB of the major=0 ie navigation use. 
After the scan time, it selects the top 3 RSSI and sends these in the UL.

Useful Config keys:
------------------
0501 : scan time in millisecs

