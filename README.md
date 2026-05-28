# Headless-Display-ESP32-to-ESP32
Display to monitor a Bambu P1S printer using x2  ESP32s and a 1.8inch TFT LCD Display. 



# Bambu P1S Headless Monitor

A two-node ESP32 system that monitors a Bambu Labs P1S 3D printer and displays live status on a local screen — no cloud access, no open ports, no Bambu Connect required.

## Overview

This project replaces the need to expose your printer to Bambu's cloud infrastructure. One ESP32 sits at the printer and receives data locally; a second drives a small TFT display showing printer state, environmental conditions, and a live clock. Communication between nodes is handled via ESP-NOW, keeping everything on your local network.

## Short Disclaimer/Headsup

I am using this project to learn.  There are most certaintly a plethora of much better projects on this topic out there but hopefully some of lessons learned can help others down the road.

**Current status:** Display node fully operational. Bridge node in development.

---

## System Architecture

```
Bambu P1S
    ↓ (local network)
ESP32 #1 — Bridge Node
    - Receives printer data via MQTT (local broker)
    - Forwards to display via ESP-NOW
    ↓ (ESP-NOW, wireless)
ESP32 #2 — Display Node
    - ST7735S 1.8" TFT (128×160)
    - Shows printer state, clock, environmental data
    - Subscribes to MQTT for sensor data (Environmental Tracker Project)
```

---

## Hardware

### Display Node (ESP32 #2)
| Component | Details |
|---|---|
| Microcontroller | ESP32 DevKit (any standard 38-pin variant) |
| Display | ST7735S 1.8" TFT, 128×160, ST7735_BLACKTAB |
| Sensors | BMP280 (pressure/temp), MQ-135 (AQI) via MQTT |

### Display Wiring (locked config)
| Display Pin | ESP32 Pin | Notes |
|---|---|---|
| BLK | IO15 | Backlight |
| CS | IO5 | |
| DC | IO27 | Do NOT use IO2 — strapping pin |
| RST | IO4 | |
| SDA (MOSI) | IO23 | |
| SCL (SCLK) | IO18 | |
| VDD | 3.3V | |
| GND | GND | |

> ⚠️ **Strapping pin warning:** IO2 is an ESP32 strapping pin, I believe there are others as well but this is the one I kept having issues with. Assigning DC to IO2 causes it to sit at 0V on boot, silently blocking all pixel data. Switching to IO27 for DC resolved the issue.
>I was able to confirm the issue with a multimeter on the DC pin.

---

## Software

- **Framework:** Arduino (via PlatformIO)
- **Display library:** TFT_eSPI
- **Protocol:** MQTT (PubSubClient), ESP-NOW
- **Time:** NTP sync

### Dependencies (platformio.ini)
```ini
lib_deps =
    bodmer/TFT_eSPI
    knolleary/PubSubClient
```

### Required build flags
All TFT configuration must be set via build flags. Do **not** rely on `User_Setup.h` in PlatformIO — it is unreliable.

```ini
build_flags =
    -DUSER_SETUP_LOADED
    -DST7735_DRIVER
    -DST7735_BLACKTAB
    -DTFT_WIDTH=128
    -DTFT_HEIGHT=160
    -DTFT_MOSI=23
    -DTFT_SCLK=18
    -DTFT_CS=5
    -DTFT_DC=27
    -DTFT_RST=4
    -DTFT_BL=15
    -DSPI_FREQUENCY=10000000
    -DLOAD_GLCD
    -DLOAD_FONT2
    -DLOAD_FONT4
    -DLOAD_FONT6
    -DLOAD_FONT7
    -DLOAD_FONT8
    -DLOAD_GFXFF
    -DDRAW_CHARS_CGRAM
```

> ⚠️ **SPI frequency:** 27MHz is too aggressive for breadboard wiring. Corruption will occur. 10MHz is stable.

> ⚠️ **Font loading:** Font libraries must be explicitly declared with `-DLOAD_` flags. Without them, text rendering silently fails with no error.

---

## Setup

### Prerequisites
- [PlatformIO](https://platformio.org/) (VS Code extension recommended)
- Mosquitto MQTT broker running on your local network
- Bambu P1S on the same local network (I'm sure this can be used for other devices)


## Current Display Screens

| Screen | Trigger | Content |
|---|---|---|
| Idle | Printer connected, not printing | Header, clock, temp, pressure, AQI, IP |

| Printing | Active print job | Print progress, temps, time remaining |

| No signal | Bridge node unreachable | Connection lost indicator |

---

## Lessons Learned

These cost real debugging time and are documented here so you don't repeat them.

| Issue | Cause | Fix |
|---|---|---|
| Blank screen, no pixels | IO2 strapping pin held DC at 0V | Move DC to IO27 |

| Corrupted display output | SPI at 27MHz unstable on breadboard | Reduce to 10MHz |

| Wrong colors / no init | ILI9341 driver selected for ST7735S display in the library itself, comment out the ILI9341 and uncomment ST7725S| Use ST7735_DRIVER + ST7735_BLACKTAB |

| Pin config ignored | User_Setup.h unreliable in PlatformIO | Move all config to platformio.ini build flags |

| Text silently missing | Font libraries not declared | Add all -DLOAD_ flags explicitly |

---

## Roadmap

- [ ] Sprite-based rendering on idle screen (eliminate flicker)
- [ ] Consistent sprite rendering across all screens
- [ ] Bridge node — receive Bambu printer data, forward via ESP-NOW
- [ ] Test printing screen with live bridge data
- [ ] Migraine event indicator from environmental sensor MQTT feed

---

## Related Projects

- [Environmental Monitoring System](https://github.com/jamus40/Environmental-Monitoring-System) — the sensor node feeding environmental data to this display
