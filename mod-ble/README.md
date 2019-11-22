# BLE generic application modules Package Definition

This is the package containing the generic firmware module for BLE operation for navigation and/or tag scanning

The source files are located in the src/ directory.

Header files are located in include/ 

pkg.yml contains the base definition of the package.

Any questions?  Please refer to the documentation at 
http://mynewt.apache.org/ or ask questions on dev@mynewt.apache.org

mod-ble
-------

This is the basic BLE definition that only defines the syscfg and ble major number allocations. The other BLE scanning modules
depend on this one.

NOTE: if using a BLE on the UART without the UART switcher, then note that the console UART  will work at bootup for 30s as usual, but if you leave the console uart connection after that then the communication with the BLE module will NOT work. Unplug the console uart to have the BLE work correctly. (due to the console uart being in parallel with the BLE module, it distrupts the rx/tx when both are active)