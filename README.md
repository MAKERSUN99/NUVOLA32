# NUVOLA32 — ESP32-S3 Breakout Board

> **DREAM. CODE. CREATE.**  
> A compact, all-in-one development board by [MakerSun](https://makersun.net), manufactured by PCBWay.

![NUVOLA32](https://img.shields.io/badge/ESP32--S3-N16R8-blue?style=flat-square&logo=espressif)
![Wi-Fi](https://img.shields.io/badge/Wi--Fi-802.11%20b%2Fg%2Fn-brightgreen?style=flat-square)
![Bluetooth](https://img.shields.io/badge/Bluetooth-5%20%2F%20BLE-blue?style=flat-square)
![License](https://img.shields.io/badge/Open%20Source-Schematics%20Available-orange?style=flat-square)

<p align="center">
  <img src="docs/nuvola32-hero.jpg" alt="NUVOLA32 — ESP32-S3 Breakout Board" width="600"/>
</p>

---

## What is NUVOLA32?

NUVOLA32 is a powerful development board based on the **ESP32-S3**, designed for makers, hobbyists, and professional developers who want to rapidly prototype IoT devices, audio systems, gaming consoles, and display interfaces — all on a single compact board.

It combines a high-performance wireless microcontroller with a rich set of on-board peripherals: TFT display, audio DAC, speaker, SD card, I2C buttons, CharliePlexed LEDs, LiPo battery management, and a 12-pin expansion header — ready to use out of the box.

---

## Features

| Feature | Details |
|---|---|
| **Module** | ESP32S3-WROOM-1 N16R8 |
| **Flash** | 16 MB |
| **PSRAM** | 8 MB |
| **Wireless** | Wi-Fi 802.11 b/g/n + Bluetooth 5 / BLE |
| **USB** | Type-C (CH340C) — Arduino IDE & ESP-IDF compatible |
| **Auto-Reset** | Built-in auto-reset circuit for easy firmware upload |
| **Power** | USB-C 5V or LiPo battery — automatic power selection |
| **Battery Charger** | MCP73831, charging via USB-C + charge LED indicator |
| **Battery Monitor** | GPIO6 |
| **Display** | ILI9341 2.4" TFT — 320×240 px, SPI1 |
| **Audio** | PCM5102 I2S DAC + Class-D 3W amplifier + side volume potentiometer |
| **Speaker** | Built-in 3W speaker |
| **Storage** | MicroSD card slot (SPI0) |
| **Buttons** | 8× tactile via PCF8574 (I2C) + RESET + BOOT |
| **LEDs** | 6× CharliePlexing on GPIO 3, 9, 10 |
| **Switch** | Physical ON/OFF |
| **Case** | Custom blue protective enclosure |
| **Manufacturer** | [PCBWay](https://www.pcbway.com) |

---

## GPIO Expansion Header (12-pin)

```
Pin 1  — VCC
Pin 2  — GND
Pin 3  — MISO  (SPI0, shared with SD card)
Pin 4  — CLK   (SPI0, shared with SD card)
Pin 5  — MOSI  (SPI0, shared with SD card)
Pin 6  — GND
Pin 7  — SCL   (I2C)
Pin 8  — SDA   (I2C)
Pin 9  — GPIO8
Pin 10 — GPIO2
Pin 11 — GPIO1
Pin 12 — GND
```

---

## Use Cases

### 🌐 IoT & Smart Home
Connect sensors, automate your home, push data to cloud dashboards. Built-in Wi-Fi is ready for MQTT, HTTP, and WebSocket out of the box.

### 🎮 Portable Retro Console
TFT display + 8 tactile buttons + 3W audio = a complete retro gaming platform. No extra hardware required.

### 🎵 Audio Station
Audiophile-grade PCM5102 I2S DAC for clean, high-fidelity audio output. Supports Wi-Fi streaming, SD card playback, and DSP equalization. Side volume knob for direct control.

### ⚡ Rapid Prototyping
RESET, BOOT, auto-reset, and USB-C: zero configuration needed. Go from idea to running firmware in minutes with Arduino IDE or ESP-IDF.

---

## Getting Started

### Requirements

- [Arduino IDE](https://www.arduino.cc/en/software) or [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/)
- USB-C cable
- ESP32-S3 board support package (via Arduino Board Manager or ESP-IDF)

### Arduino IDE Setup

1. Open **Arduino IDE** → Preferences → add the ESP32 boards URL:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
2. Go to **Tools → Board → Boards Manager**, search for `esp32`, install the Espressif package.
3. Select **ESP32S3 Dev Module** (or compatible) as board.
4. Connect NUVOLA32 via USB-C — the CH340C handles auto-reset automatically.
5. Select the correct COM port and upload your sketch.

---

### Pin Assignments

| Peripheral | Interface | Pins |
|---|---|---|
| ILI9341 Display | SPI1 | Custom SPI1 pins |
| MicroSD Card | SPI0 | Custom SPI0 (shared with expansion) |
| PCM5102 Audio | I2S | Standard I2S |
| PCF8574 Buttons | I2C | SCL / SDA |
| Battery Monitor | ADC | GPIO6 |
| CharliePlex LEDs | GPIO | GPIO3, GPIO9, GPIO10 |

### GPIO

| Peripheral | Interface | Pins |
|---|---|---|
| ILI9341 Display | SPI1 | CLK-16 MOSI-17 MISO-11 DC-38 CD-39 RST-48 |
| MicroSD Card | SPI0 | CLK-14 MISO-12 MOSI-13 CS-15 |
| PCM5102 Audio | I2S | LRCK-4 BCLK-5 DATA-7 |
| PCF8574 Buttons | I2C | ADDRESS 0x38 SCL-18 SDA-21 |
| Battery Monitor | ADC | GPIO6 |
| CharliePlex LEDs | GPIO | GPIO3, GPIO9, GPIO10 |
| PCF8574 | GPIO | UP-5 DOWN-1 LEFT-7 RIGHT-6 SW1-0 SW2-3 SW3-2 SW4-4 |

> Refer to the schematic for exact pin mappings.

---

## Gallery

<p align="center">
  <img src="docs/nuvola32-front.jpg" alt="NUVOLA32 front view — 8 buttons, TFT display, RST/BOOT, power switch" width="48%"/>
  &nbsp;
  <img src="docs/nuvola32-pcb-back.jpg" alt="NUVOLA32 PCB back — ESP32-S3, speaker, SD card slot, LiPo battery connector" width="48%"/>
</p>

<p align="center">
  <img src="docs/nuvola32-display.jpg" alt="NUVOLA32 ILI9341 2.4-inch TFT display and 12-pin expansion header" width="60%"/>
</p>

---

## Open Source

Schematics and design files are available in this repository.  
Feel free to fork, modify, and build on top of NUVOLA32.

---

## Who Is It For?

- 🛠 **Makers & Hobbyists** — plug-and-play peripherals, no soldering required
- 🎓 **Students & Researchers** — great for university projects and embedded systems courses
- 💼 **Professional IoT Developers** — production-ready hardware with quality manufacturing
- 👾 **Retrogaming Enthusiasts** — everything you need for a handheld console in one board
- 🚀 **Rapid Prototypers** — go from concept to firmware in minutes

---

## Where to Buy

- 🌐 **Official Website:** [makersun.net](https://makersun.net)
- 🏭 **Manufacturer:** [PCBWay](https://www.pcbway.com)

---

## License

This project uses dual licensing:

- **Firmware & Software** — [MIT License](LICENSE/LICENSE-firmware.md) — use freely, even in commercial projects, just keep the copyright notice.
- **Hardware (schematics, PCB files)** — [CERN OHL v2 Permissive](LICENSE/LICENSE-hardware.md) — open hardware license, derivatives encouraged to credit MakerSun.

---

*NUVOLA32 — by MakerSun · DREAM. CODE. CREATE.*
