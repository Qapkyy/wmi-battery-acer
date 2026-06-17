# Acer Nitro V16 Battery Health Control Driver [For now!]

[![License: GPL-2.0-only](https://img.shields.io/badge/License-GPL--2.0--only-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)
[![Linux Kernel](https://img.shields.io/badge/Linux-Kernel%20%E2%89%A5%206.1-red.svg)](https://kernel.org)

A lightweight Linux kernel WMI driver to manage battery features on the **Acer Nitro V16 (ANV16-71)** laptop. This driver communicates with the laptop's ACPI firmware via WMI (Windows Management Instrumentation) interfaces to toggle battery health (charging limit) and calibration modes, as well as read internal battery temperature.

## Features

- **Battery Health Mode:** Toggle the standard 80% charging limit to prolong battery lifespan.
- **Calibration Mode:** Enable/disable battery gauge calibration.
- **Temperature Monitoring:** Read real-time internal battery temperature in milli-Celsius.
- **Sysfs Interface:** Simple integration through the standard Linux `/sys` directory.
- **Module Parameter Support:** Set the default health mode behavior at boot time via kernel arguments.

## Compatibility

- **Hardware:** Acer Nitro V16 series (Tested specifically on ANV16-71 variants).
- **OS/Kernel:** Modern Linux Distributions. Built-in support for backwards compatibility down to kernel `< 6.12.0` and fully compliant with newer kernels.

## Installation

### Prerequisites

Ensure you have the necessary kernel headers and build tools installed on your distribution.

- **Ubuntu / Debian / Pop!_OS:**
  '''
  sudo apt update
  sudo apt install build-essential linux-headers-$(uname -r)
  '''
- **Fedora:**
  '''
  sudo dnf update
  sudo dnf install kernel-devel kernel-headers development-tools
  '''
- **Arch Linux:**
 '''
 sudo pacman -S base-devel linux-headers
 '''

## Clone and Compile
Clone this repository and compile the kernel module using the provided Makefile:
 '''
 git clone https://github.com/Qapkyy/wmi-battery-acer.git](https://github.com/Qapkyy/wmi-battery-acer.git)
 cd wmi-battery-acer
 make
'''
## Load the Module Manually
You can test the module by inserting it into the running kernel:
'sudo insmod wmi-battery-acer.ko'

## Usage
Once the driver is successfully loaded, it creates device nodes in the sysfs system under the acer-wmi-battery driver path:
'/sys/bus/wmi/drivers/wmi-battery-acer/'

## Author
_Qapky - qapkyy3@gmail.com_
