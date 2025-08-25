# Introduction

<a href="url"><img src="./PIC/MiniLoRa-pic.png" height="auto" width="auto" style="border-radius:40px"></a>

---

Discord server dedicated to this project :
<a href="https://discord.gg/ZdWrauP4Yt"><img alt="Discord" src="https://img.shields.io/badge/-Join-lightblue?logo=discord"></a>
---

## Table of Contents

- [✨ What's MiniLoRa?](#-whats-MiniLoRa)
- [⚙️ Hardware Overview](#-hardware-overview)
- [💾 Software Overview](#-software-overview)
- [🚀To Do and Next Steps](#-to-do-and-next-steps)

## ✨ What's MiniLoRa?

Small size multi-purpose **ESP32-C6** board with **LoRa** and **2.4 GHz** connectivity.

**Main focus**

* Low power consumption for long battery life
* Multiple wireless connectivity
* Small size

### Features

* ESP32-C6
  * 32 bits RISC-V Main processor : 160 MHz
  * 32 bits RISC-V Low power processor : 20 Mhz
  * 512 KB RAM
  * 4 MB FLASH
  * Wi-Fi 6 (2.4 GHz)
  * Bluetooth 5 (BLE)
  * Zigbee / Matter / Thread (802.15.4)
    Internal or external antenna
* LoRa modem
  * LoRaWan capable
  * External Antenna
* GNSS with integrated antenna
* Temperature and humidity sensor
* Luminosity sensor
* 3-axis accelerometer
* 2 user accessible LED
* 1 user accessible button
* Voltage reading of the battery
* Integrated BMS and JST connector



## ⚙️ Hardware Overview

<a href="url"><img src="./PIC/MiniLora-pcb.png" height="auto" width="auto" style="border-radius:40px"></a>
---

**PCB dimension**

| D| mm|
| --- | --- |
| L | 50 |
| W | 32 |
| H | 12,5* |

* including GNSS module and JST connector

**PCB Info**

Design with Kicad 9.0

* Full schematic and design can be found under **HW\PCB**
* Production files for the current batch are also included
* Libraries used can be found under **HW\PCB\lib**

**Enclosure Info**

Design with FreeCad 1.0

* Very basic and not finished
* Can be 3D printed with basic HW
* Should be improved in the future
* Thought to be attached to backpack

## 💾 Software Overview

## 🚀To Do and Next Steps

Here are the idea for the next improvement and developpement :

### Software

* Create a first full version in Arduino
* Create a better / smaller / mor realiable version directly with FreeRTOS
* User inteface with one button
* BLE app for phones and PC
* App for configuration via USB-C

### Hardware

* Size can me optimized
* I2C pads for external sensors
* External BMS for energy harversting
* Pressure sensor (or Temp / Hum / Pressure)
* 1-PPS connection to MCU from GPS
* GNSS module with external flat antenna







