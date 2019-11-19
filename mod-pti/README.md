# Environment module app core Package Definition

This is the package containing the PTI module used to handle alarm button, activation/deactivation, fall detection etc for app core.

The source files are located in the src/ directory.

Header files are located in include/ 

pkg.yml contains the base definition of the package.

Any questions?  Please refer to the documentation at 
http://mynewt.apache.org/ or ask questions on dev@mynewt.apache.org

Operation
---------

Button:
 - long press = alarm
 - short press twice in < 2s = activate or deactive (confirmed by led flash)

Accelero:
 - detect vertical or horizontal orientation
 - detect fall or shock


