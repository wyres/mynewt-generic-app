# BLE generic application modules Package Definition

This is the package containing the generic firmware module for BLE operation for tag 'proximity' scanning and ibeaconning

The source files are located in the src/ directory.

Header files are located in include/ 

pkg.yml contains the base definition of the package.

Any questions?  Please refer to the documentation at 
http://mynewt.apache.org/ or ask questions on dev@mynewt.apache.org

mod-ble-scan-proximity
-------

mod_ble_scan_proximity [module id = X]: scans for BLE beacons with the MSB of the major = 0x82
When it is not scanning (ie during idle) it keeps the BLE module active in ibeaconning mode (to be seen by other proximity tags)
 
Useful Config keys:
------------------
0501 : scan time in millisecs
050B : exit timeout in minutes
0528 : contact time to consider 'significant'
0529 : rssi limit to consider 'significant'
0510 : UUID for beacons for this function
0511 : my major (high byte is ignored and set to 0x82 in tx)
0512 : my minor

