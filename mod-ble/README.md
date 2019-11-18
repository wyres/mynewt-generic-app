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
