# BLE generic application modules Package Definition

This is the package containing the generic firmware module for BLE connection to the console.

The source files are located in the src/ directory.

Header files are located in include/ 

pkg.yml contains the base definition of the package.

Any questions?  Please refer to the documentation at 
http://mynewt.apache.org/ or ask questions on dev@mynewt.apache.org

mod-ble-wconsole
-------

mod_ble_wconsole runs at each data collection time in serial mode.

It specifies an indefinite collection time, and initiates the connection to the BLE module using wbleuart.
This lets it check the state of the module. In the event it is already in the 'cross-connected' (NUS-UART) state
as initiated by the remote BLE app, it will start the wconsole on the uart connection and run the appcore
atcmd set. Exit from this state is done when the remote user issues a AT+DISC command.



