# BLE generic application modules Package Definition

This is the package containing the generic firmware module for BLE operation for proximity alerts 

The source files are located in the src/ directory.

Header files are located in include/ 

pkg.yml contains the base definition of the package.

Any questions?  Please refer to the documentation at 
http://mynewt.apache.org/ or ask questions on dev@mynewt.apache.org

mod-ble-scan-alert
-------

mod_ble_scan_alert [module id = 2] : scans for BLE ibeacons with the MSB of the major=0 ie navigation use. 
After the scan time, it selects the top 3 RSSI and sends these in the UL IFF the list has changed.
By default the scan time is 5s, and the idle time is 0s ie it continuously scans! 
This should give a behavious where any change to the BLE beacons seen is notified via UL with a maximum latency of 5s 
for new beacons (entering a zone) and 5 minutes (to leave a zone)

Useful Config keys:
------------------
0501 : scan time in millisecs

