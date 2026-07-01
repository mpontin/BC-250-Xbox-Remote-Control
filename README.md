# BC-250 REMOTE PSU POWER CONTROLLER
ESP32 2-Channel 5V Relay Module-based device for remote control of the BC-250 ATX power supply with web interface and Xbox controller support.

## Key Features

- **Remote Power Control:** Turn PC on/off via web interface or physical button
- **Xbox Controller Wake:** Press the Xbox (Guide) button to power on the PC via BLE proximity detection
- **Multiple Controllers:** Save up to 5 controllers — any of them will wake the PC
- **Controller Blacklist:** Permanently block up to 10 specific controllers by MAC address
- **Shutdown Cooldown:** 60-second ignore window after PC shuts down, preventing accidental immediate re-wake
- **WiFi Configuration:** Web-based setup with network scanning
- **Over-the-Air Updates:** Firmware and filesystem updates via web interface

## Installation

### 1. Board setup

Install the standard ESP32 Arduino core (no Bluepad32 platform required).

Open Arduino IDE and go to **File > Preferences**.  
In "Additional Boards Manager URLs" add:
```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```
Go to **Tools > Board > Boards Manager**, search for `esp32`, and install **esp32 by Espressif Systems**.

### 2. Install libraries

- [LittleFS (for ESP32)](https://github.com/lorol/LITTLEFS)
- ArduinoJson

The BLE libraries (`BLEDevice`, `BLEScan`) are included with the ESP32 Arduino core — no separate install needed.

### 3. Upload filesystem

Upload the `data/` folder to LittleFS using the **Arduino ESP32 LittleFS Data Upload** tool before flashing the firmware.

## Setup Notes

- On first boot the device starts in **WiFi AP mode** (`BC-250-POWER-CONTROL`, no password). Connect and open `http://192.168.4.1` to configure.
- AP mode can be slow — be patient. HTTPS is not supported.
- Testing without the TPMS1 pin 9 signal connected causes instability.

## Controller Management

Controllers are managed from the **Setup** page (`/setup`).

**Adding a controller:**
1. Enable Xbox Controller Support and save.
2. Press the Xbox (Guide) button on the controller to make it advertise over BLE.
3. Click **ADD CURRENT CONTROLLER** — the nearby controller's MAC is saved automatically.
4. Up to 5 controllers can be saved. Any saved controller will wake the PC.

**Auto-pairing:**  
If the saved controller list is empty, the first controller seen within ~2 metres is saved automatically. This only triggers once — subsequent controllers require the Add button.

**Blacklist:**  
Block a specific controller permanently under the **Controller Blacklist** section. Blocked controllers are ignored even if they appear in the allowed list. You can block the currently nearby controller or enter a MAC address manually.

**Removing a controller:**  
Click the ✕ **REMOVE** button next to any saved MAC in the list, or use **REMOVE ALL** to clear the entire list (which re-enables auto-pairing).

## Wake Behaviour

- BLE scanning runs continuously in the background (non-blocking).
- When a saved controller is detected, the PC powers on after a 15-second cooldown between triggers.
- For 60 seconds after the PC shuts down, all controller wake events are ignored.
- When the PC is already on, the controller has no effect.

<img width="294" height="542" alt="kuva" src="https://github.com/user-attachments/assets/1544a9e2-1a29-4ba2-bede-efac3149f9f3" />
