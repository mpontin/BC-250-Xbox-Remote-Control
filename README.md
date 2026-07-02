# BC-250 REMOTE PSU POWER CONTROLLER

ESP32 2-Channel 5V Relay Module-based device for remote control of the BC-250 ATX power supply with Xbox controller support.

## Key Features

- **Physical Button Control:** Short press to power on/off, long press for force shutdown
- **Xbox Controller Wake:** Press the Xbox (Guide) button to power on the PC via BLE proximity detection
- **Multiple Controllers:** Save up to 5 controllers — any of them will wake the PC
- **Controller Blacklist:** Permanently block up to 10 specific controllers by MAC address
- **Shutdown Cooldown:** 60-second ignore window after PC shuts down, preventing accidental immediate re-wake
- **Button Pairing Mode:** Hold the physical button for 5 seconds (PC off) to pair a new controller

## Hardware

- ESP32 2-Channel 5V Relay Module
- Momentary LED push button
- BC-250 ATX PSU

## Wiring

| ESP32 GPIO | Purpose |
|-----------|---------|
| 16 | Relay 1 — PS_ON hold (PSU enable) |
| 17 | Relay 2 — Power button pulse |
| 22 | Power button LED |
| 23 | Momentary button switch |
| 4 | BC-250 TPMS1 pin 9 (PC state monitor) |
| 2 | Status LED (optional) |

**Relay outputs:**

| Relay | COM | NO | Connects to |
|-------|-----|----|-------------|
| Relay 1 | PS_ON (green wire) | GND | BC-250 PSU connector |
| Relay 2 | Power button pad (P) | TPMS1 pin 17 (GND) | BC-250 power button |

**Button wiring:**
- Switch NO → GPIO 23
- Switch COM → GND
- LED + → GPIO 22
- LED − → GND

**PSU connector:**
- +5VSB → board VCC (always-on standby power)
- PS_ON → Relay 1 COM
- GND → board GND

## Installation

### 1. Board setup

Install the standard ESP32 Arduino core.

Open Arduino IDE and go to **File > Preferences**.
In "Additional Boards Manager URLs" add:
```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```
Go to **Tools > Board > Boards Manager**, search for `esp32`, and install **esp32 by Espressif Systems**.

### 2. Install libraries

- ArduinoJson

The BLE libraries (`BLEDevice`, `BLEScan`) and LittleFS are included with the ESP32 Arduino core — no separate install needed.

### 3. Partition scheme

Go to **Tools > Partition Scheme** and select:
```
Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)
```
WiFi + BLE together require more flash than the default partition allows.

### 4. Flash

Upload the sketch. No filesystem upload step is required.

## Button Operation

| Action | PC state | Result |
|--------|----------|--------|
| Short press | Off | Power on |
| Short press | On | Normal shutdown |
| Hold 5s | Off | Pairing mode |
| Hold 5s | On | Force shutdown |

## Pairing a Controller

1. With the PC off, **hold the button for 5 seconds**
2. The power LED blinks rapidly — pairing mode is active (30 second window)
3. Press the **Xbox (Guide) button** on your controller to make it advertise over BLE
4. Hold the controller close to the ESP32
5. The LED stops blinking when the controller MAC is saved

Up to 5 controllers can be saved. Repeat the process to add more.

To clear all saved controllers, delete `/xbox_config.json` from the ESP32's LittleFS filesystem.

## Wake Behaviour

- BLE scanning runs continuously in the background
- When a saved controller is detected nearby, the PC powers on (15-second cooldown between triggers)
- For 60 seconds after the PC shuts down, all controller wake events are ignored
- When the PC is already on, the controller has no effect