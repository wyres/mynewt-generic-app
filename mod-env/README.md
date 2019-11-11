# Environment module app core Package Definition

This is the package containing the generic environment data module used to get data for app core.

The source files are located in the src/ directory.

Header files are located in include/ 

pkg.yml contains the base definition of the package.

Any questions?  Please refer to the documentation at 
http://mynewt.apache.org/ or ask questions on dev@mynewt.apache.org

Operation
---------

For the first N ULs after reboot, the module will add reboot/assert/build info to the UL.

At each read cycle, mod-env will read data from the SensorMgr, MovementMgr.
Each individual value will be added to the UL if it has 'changed' significantly since the last read time (as determined by the XMgr)
Every X minutes, values of 'direct' sensors (temp, pressure etc) will be added to the UL EVEN if they have not been deemed to have changed... This does not apply to 'trigger' sensors like movement or fall detection.


