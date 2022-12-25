# PX4 Drone Autopilot


This repository holds a customized version of the [PX4 Flight Controller](https://github.com/PX4/PX4-Autopilot) (v1.10.1). This is a work in progress and there may be changes. 



## ERL Quad States Message 
A new message type was added to the firmware that has the following: 
```
uint64 timestamp		# time since system start (microseconds)
float32[3] position             # position x, y, z in world-frame 
float32[4] orientation          # orientation in quaternion 
float32[3] velocity             # velocity in body-frame 
float32[3] angular_velocity     # angular velocity in body-frame 
float32[4] controls	        # thrust, torque x, torque y, torque z 
float32[4] controls_actual	# thrust, torque x, torque y, torque z (after clamp) 
```

For more information, take a look at this [Wiki](https://github.com/ExistentialRobotics/erl_quadrotor_firmware/wiki/Custom-Message) page. 


## Building PX4 Software

1. Clone this repo 
```
git clone git@github.com:ExistentialRobotics/erl_quadrotor_firmware.git
```
2. Build the firmware 
```
cd erl_quadrotor_firmware
make px4_fmu-v3_default 
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
