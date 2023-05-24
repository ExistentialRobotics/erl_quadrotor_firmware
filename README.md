# PX4 Drone Autopilot


This repository holds a customized version of the [PX4 Flight Controller](https://github.com/PX4/PX4-Autopilot) (v1.10.1). This is a work in progress and there may be changes. 



## ERL Quad States Message 
A new message type was added to the firmware that has the following: 
```
# ERL Quad States 
uint64 timestamp                  # time since system start (microseconds)
float32[3] position               # position in (FRD) body-frame  
float32[4] orientation            # orientation in quaternion [w, x, y, z]
float32[3] velocity               # velocity in (FRD) body-frame 
float32[3] angular_velocity       # angular velocity in (FRD) body-frame 
float32[4] controls               # thrust, torque x, torque y, torque z 
float32[4] controls_scaled        # thrust, torque x, torque y, torque z (after battery scaling) 
```

For more information, take a look at this [Wiki](https://github.com/ExistentialRobotics/erl_quadrotor_firmware/wiki/Custom-Message) page. 

## Loading Firmware 
* Our custom px4 firmware can be found in ```~/firmware/px4_fmu-v3_default.px4```
* Follow this [link](https://docs.px4.io/main/en/config/firmware.html) to upload the new firmware 


## Building PX4 Software

1. Clone this repo 
```
$ git clone git@github.com:ExistentialRobotics/erl_quadrotor_firmware.git
```
2. Build the firmware 
```
$ cd erl_quadrotor_firmware
$ make px4_fmu-v3_default 
```
```
...
[206/206] Creating /home/abdullah/Downloads/erl_quad.../external/Build/px4io_firmware/px4_io-v2_default.px4
[1228/1228] Creating /home/abdullah/Downloads/erl_qu...ware/build/px4_fmu-v3_default/px4_fmu-v3_default.px4
```
3. Connect the [Pixhawk 2 (Cube Black) (FMUv3)](https://docs.px4.io/main/en/flight_controller/pixhawk-2.html) to your computer via USB

4. Upload the firmware 
```
make px4_fmu-v3_default upload 
```
you will see the following
```
Erase  : [====================] 100.0%
Program: [====================] 100.0%
Verify : [====================] 100.0%
Rebooting.

[100%] Built target upload
```
