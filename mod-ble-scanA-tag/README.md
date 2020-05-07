# BLE generic application modules Package Definition

This is the package containing the generic firmware module for BLE operation for tag scanning when used 
in a powered mode, so scanning is performed continuously. This implies a reduction in the detection latency
but also a need for a more sophisticated algo for deciding when to send up the data (as the duty cycle is
still a limiting factor)

The source files are located in the src/ directory.

Header files are located in include/ 

pkg.yml contains the base definition of the package.

Any questions?  Please refer to the documentation at 
http://mynewt.apache.org/ or ask questions on dev@mynewt.apache.org

mod-ble-scanA-tag
-------

mod_ble_scanA_tag [module id = 6]: scans for BLE beacons with the MSB of the major indicating either presence, or count types.

Maximum numbers of tags scanned:
 - type/count : 100 in zone at same time (all types)
 - enter/exit : 16 in zone at same time
 These values are configured in the syscfg.yml for the target so are hardcoded for the firmware image. (as they define static array sizes to avoid malloc). They can be increase but it is likely the max RAM will be reached (eg at build time or runtime)
 
The scans are done in periods of 5s, and the data analysed after each period. The RSSIs are averaged for previously seen beacons, and
a algo based on change in RSSI is used to decide if the tag rssi data should also be send to backend (as well as its presence flag)

For presence types, a new TLV is used to signal the 'significant' changes in rssi for specific beacons.

Useful Config keys:
------------------
0501 : scan time in millisecs for the cycle
050X : delta required to consider 'significant' change in RSSI

