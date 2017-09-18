![ODrive Logo](https://static1.squarespace.com/static/58aff26de4fcb53b5efd2f02/t/59bf2a7959cc6872bd68be7e/1505700483663/Odrive+logo+plus+text+black.png?format=1000w)

This project is all about accurately driving brushless motors, for cheap. The aim is to make it possible to use inexpensive brushless motors in high performance robotics projects, like [this](https://www.youtube.com/watch?v=WT4E5nb3KtY).

This repository contains configuration and analysis scripts that run on a PC. The other related repositories are:
* [ODriveFirmware](https://github.com/madcowswe/ODriveFirmware): Firmware that runs on the board.
* [ODriveHardware](https://github.com/madcowswe/ODriveHardware): Circuit board design.

There is also [ODriveFPGA](https://github.com/madcowswe/ODriveFPGA), which contains the FPGA logic and software that runs on the FPGA based ODrive. This is not currently in development, but may be resumed at some later date.

## Getting Started
*References to hardware is with respect to v3.2. Other versions may still apply, but component designators may differ*

It is perfectly fine, and even recommended, to start testing with just a single motor and encoder.
Make sure you have a good mechanical connection between the encdoer and the motor, slip can cause disasterous oscillations.
All non-power I/O is 3.3V output and 5V tolerant on input, except:

* GPIO 3 and GPIO 4 are NOT 5V tolerant on ODrive v3.2 and earlier.

You need one or two [brushless motors](https://hackaday.io/project/11583-odrive-high-performance-motor-control/log/37666-hobby-motors-in-your-robots), [quadrature incremental encoder(s)](https://discourse.odriverobotics.com/t/which-encoders-to-choose/63/2), and a power resistor.

*The power resistor values you need depends on your motor setup, and peak/average decelleration power. A good starting point would be a [0.47 ohm, 50W resistor](https://www.digikey.com/product-detail/en/te-connectivity-passive-product/HSA50R47J/A102181-ND/2056131).*

Wire up the motor phases into the 3-phase screw terminals, and the power resistor to the AUX terminal. Wire up the power source (12-24V) to the DC terminal, make sure to pay attention to the polarity. Do not apply power just yet.

![Image of ODrive all hooked up](https://static1.squarespace.com/static/58aff26de4fcb53b5efd2f02/t/5901933ee58c62307a19be6b/1493275460219/Odrive.jpg)

Wire up the encoder(s) to J4. The A,B phases are required, and the Z (index pulse) is optional. The A,B and Z lines have 1k pull up resistors, for use with open-drain encoder outputs. For single ended push-pull signals with weak drive current (\<4mA), you may want to desolder the pull-ups.

The currently supported command modes are USB and step/direction.
* If you are sending commands over USB, you can plug in a cable into the micro-USB port.
* If you are using step/direction, please see [setting up step/direction](#setting-up-stepdirection)

You can now:
* [Download and build the firmware](https://github.com/madcowswe/ODriveFirmware)
* [Configure the firmware parameters](https://github.com/madcowswe/ODriveFirmware#configuring-parameters)
* [Flash the board](https://github.com/madcowswe/ODriveFirmware#flashing-the-firmware)

### Startup procedure
The startup procedure is demonstrated [here](https://www.youtube.com/watch?v=VCX1bA2xnuY). 

Note: the rotor must be allowed to rotate without any biased load during startup. That means mass and weak friction loads are fine, but gravity or spring loads are not okay. Also note that in the video, the motors spin after initalisation, but in the current software the default behaviour is to do position control to position 0 (i.e. the position at startup)

### Sending USB commands
Sending USB commands is documented [here](https://github.com/madcowswe/ODriveFirmware#communicating-over-usb)

### Setting up Step/Direction
Pinout:
* GPIO 1: M0 step
* GPIO 2: M0 dir
* GPIO 3: M1 step
* GPIO 4: M1 dir

Please note that GPIO_3 and GPIO_4 are NOT 5v tolerant on ODrive v3.2 and earlier, so 3.3V signals only!
ODrive v3.3 and onward have 5V tolerant GPIO pins.

There is also a new config variable called `counts_per_step`, which specifies how many encoder counts a "step" corresponds to. It can be any floating point value.
The maximum step rate is pending tests, but it should handle at least 16kHz. If you want's to test it, please be aware that the failure mode on too high step rates is expected to be that the motors shuts down and coasts.

Please be aware that there is no enable line right now.

The step/direction interface is enabled by default, and remains active as long as the ODrive is in position control mode. By default the ODrive starts in position control mode, so you don't need to send any commands over USB to get going. You can still send USB commands if you want to.

### Getting help
If you have any issues or any questions please get in touch. The [ODrive Community](https://discourse.odriverobotics.com/) warmly welcomes you.