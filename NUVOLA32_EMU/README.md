## NUVOLA32 EMU

<p align="center">
  <img src="docs/nuvola32_emu_logo.jpg" alt="NUVOLA32 — ESP32-S3 Breakout Board" width="600"/>
</p>

---


NUVOLA32 EMU is a multi-console retro emulator firmware that runs natively on the NUVOLA32 board. Load your ROMs onto a microSD card, plug in the board, and start playing — no extra hardware required.

---

### Supported Consoles

| Console | Status |
|---|---|
| 🎮 **Nintendo Entertainment System (NES)** | ✅ Supported |
| 🕹 **Sega Genesis / Mega Drive** | ✅ Supported |
| 🎨 **Game Boy Color (GBC)** | ✅ Supported |
| 📺 **Sega Master System** | ✅ Supported |
| 🌀 **Neo Geo Pocket Color (NGPC)** | ✅ Supported |
| 📡 **Wi-Fi SD Explorer** | ✅ Browse & manage SD card over Wi-Fi |

### How It Works

1. Copy ROM files to the microSD card
2. Insert the SD card into NUVOLA32
3. Power on — the EMU launcher appears on the TFT display
4. Use the 8 tactile buttons to navigate and play
5. Audio output via the built-in 3W speaker and PCM5102 DAC
6. Use **Wi-Fi SD Explorer** to transfer ROMs wirelessly from any browser

### Controls

| Button | Action |
|---|---|
| D-Pad (4 buttons) | Directional input |
| A / B | Action buttons |
| Start / Select | Menu navigation |
| RST | Reset emulator |

> Button mapping may vary per emulated console. Refer to the firmware documentation for details.

---

## License

This project uses dual licensing:

- **Firmware & Software** — [MIT License](LICENSE-firmware.md) — use freely, even in commercial projects, just keep the copyright notice.
- **Hardware (schematics, PCB files)** — [CERN OHL v2 Permissive](LICENSE-hardware.md) — open hardware license, derivatives encouraged to credit MakerSun.

---

*NUVOLA32 — by MakerSun · DREAM. CODE. CREATE.*
