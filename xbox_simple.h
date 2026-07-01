#ifndef XBOX_SIMPLE_H
#define XBOX_SIMPLE_H

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Arduino.h>

#include "pc_control.h"

#define MAX_CONTROLLERS    5
#define MAX_BLACKLIST      10
#define AUTO_PAIR_RSSI     -65    // dBm: auto-save only when controller is within ~2m
#define WAKE_COOLDOWN_MS   15000  // ms between wake triggers
#define SHUTDOWN_COOLDOWN_MS 60000 // ms of deafness after PC turns off

extern bool xboxEnabled;
extern bool xboxAutoConnect;
extern bool getStablePcState();
extern void startPowerOn();
extern PowerState powerState;
extern void saveXboxConfig(bool enabled, bool autoConnect);

// Free function used as BLE scan-complete callback — auto-restarts the scan
static void xboxScanComplete(BLEScanResults results) {
    BLEDevice::getScan()->clearResults();
    BLEDevice::getScan()->start(1, xboxScanComplete, false);
}

class XboxSimple : public BLEAdvertisedDeviceCallbacks {

    // ----------------------------------------------------------------
    // BLE callback — runs in the BLE FreeRTOS task (not the main loop)
    // Keep side-effects minimal: only set volatile flags and char buffers.
    // All String/file operations are deferred to handle() on the main loop.
    // ----------------------------------------------------------------
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        String mac = advertisedDevice.getAddress().toString().c_str();
        int rssi  = advertisedDevice.getRSSI();

        // Blacklist: silently drop banned MACs
        for (int i = 0; i < MAX_BLACKLIST; i++) {
            if (blacklistedMacs[i].length() > 0 &&
                mac.equalsIgnoreCase(blacklistedMacs[i])) {
                return;
            }
        }

        // Shutdown cooldown: ignore everything for 60s after PC turns off
        if (shutdownCooldownActive()) return;

        // Don't act while PC is on or transitioning
        if (getStablePcState() || powerState != POWER_IDLE) return;

        if (!xboxEnabled) return;

        // Check allowed list
        bool allowed = false;
        if (!hasAnyMac()) {
            // No saved controllers — auto-pair if close enough (RSSI filter)
            if (rssi >= AUTO_PAIR_RSSI && !macAutoSaved) {
                strncpy(pendingAutoSaveMac, mac.c_str(), sizeof(pendingAutoSaveMac) - 1);
                pendingAutoSave = true;
                macAutoSaved = true;  // prevent re-entry from the next packet
            }
            allowed = true;
        } else {
            String macClean = cleanMac(mac);
            for (int i = 0; i < MAX_CONTROLLERS; i++) {
                if (allowedMacs[i] == macClean) { allowed = true; break; }
            }
        }

        if (allowed) {
            // Record last-seen for the web UI connected-mac endpoint
            strncpy(lastSeenMacBuf, mac.c_str(), sizeof(lastSeenMacBuf) - 1);
            lastSeenTime = millis();

            // Schedule a wake if the cooldown has elapsed
            if (millis() - lastWakeTime > WAKE_COOLDOWN_MS) {
                triggerWake  = true;
                lastWakeTime = millis();
            }
        }
    }

private:
    // Allowed-list (read by BLE callback, written by main loop)
    String allowedMacs[MAX_CONTROLLERS];

    // Blacklist (read by BLE callback, written by main loop)
    String blacklistedMacs[MAX_BLACKLIST];

    // Thread-safe BLE-callback → main-loop communication
    volatile bool          triggerWake         = false;
    volatile bool          pendingAutoSave      = false;
    char                   pendingAutoSaveMac[18] = {0};
    char                   lastSeenMacBuf[18]  = {0};
    volatile unsigned long lastSeenTime        = 0;
    volatile unsigned long lastWakeTime        = 0;
    bool                   macAutoSaved        = false;

    // Shutdown cooldown state (written by main loop, read by BLE task via shutdownCooldownActive)
    volatile unsigned long lastShutdownTime = 0;
    bool          lastPcWasOn      = false;

    // ---- helpers ----

    bool shutdownCooldownActive() {
        if (lastShutdownTime == 0) return false;
        return (millis() - lastShutdownTime < SHUTDOWN_COOLDOWN_MS);
    }

    String cleanMac(String mac) {
        mac.replace(":", "");
        mac.replace("-", "");
        mac.toUpperCase();
        return mac;
    }

    String formatMac(String clean) {
        if (clean.length() != 12) return "";
        String f = "";
        for (int i = 0; i < 12; i += 2) {
            if (i > 0) f += ":";
            f += clean.substring(i, i + 2);
        }
        return f;
    }

public:

    // ----------------------------------------------------------------
    // Setup — call once from Arduino setup()
    // ----------------------------------------------------------------
    void setupBLE() {
        BLEDevice::init("");
        BLEScan* pScan = BLEDevice::getScan();
        pScan->setAdvertisedDeviceCallbacks(this);
        pScan->setActiveScan(true);
        pScan->setInterval(100);
        pScan->setWindow(99);
        pScan->start(1, xboxScanComplete, false);
        Serial.println("XBOX: BLE scan started (non-blocking, auto-restart)");
    }

    // ----------------------------------------------------------------
    // Main loop handler — call every loop() iteration
    // ----------------------------------------------------------------
    void handle() {
        bool pcOn = getStablePcState();

        // Detect PC shutdown transition → start cooldown
        if (lastPcWasOn && !pcOn) {
            lastShutdownTime = millis();
            if (lastShutdownTime == 0) lastShutdownTime = 1;
            macAutoSaved = false;
            Serial.println("XBOX: PC turned off — 60s cooldown active");
        }
        lastPcWasOn = pcOn;

        // Process auto-save deferred from BLE callback
        if (pendingAutoSave && pendingAutoSaveMac[0] != 0) {
            pendingAutoSave = false;
            String mac = String(pendingAutoSaveMac);
            memset(pendingAutoSaveMac, 0, sizeof(pendingAutoSaveMac));
            addAllowedMac(mac);
            saveXboxConfig(xboxEnabled, xboxAutoConnect);
            Serial.println("✅ XBOX: Auto-saved MAC: " + mac);
        }

        // Process pending wake trigger
        if (triggerWake) {
            triggerWake = false;
            if (!pcOn && powerState == POWER_IDLE && !shutdownCooldownActive()) {
                Serial.println("XBOX: Triggering PC power on");
                startPowerOn();
            }
        }

        // Periodic status print
        static unsigned long lastPrint = 0;
        unsigned long now = millis();
        if (now - lastPrint > 10000 && xboxEnabled) {
            if (shutdownCooldownActive()) {
                Serial.print("XBOX: Shutdown cooldown (");
                Serial.print((SHUTDOWN_COOLDOWN_MS - (now - lastShutdownTime)) / 1000);
                Serial.println("s remaining)");
            } else if (isConnected()) {
                Serial.println("XBOX: Authorized controller nearby: " + getConnectedMac());
            } else {
                Serial.print("XBOX: Scanning... (");
                Serial.print(getMacCount());
                Serial.println(" saved)");
            }
            lastPrint = now;
        }
    }

    // ----------------------------------------------------------------
    // Allowed-MAC management
    // ----------------------------------------------------------------
    void clearAllowedMacs() {
        for (int i = 0; i < MAX_CONTROLLERS; i++) allowedMacs[i] = "";
        macAutoSaved = false;
        Serial.println("XBOX: Allowed list cleared");
    }

    bool addAllowedMac(String mac) {
        mac = cleanMac(mac);
        if (mac.length() == 0 || mac == "000000000000") return false;
        for (int i = 0; i < MAX_CONTROLLERS; i++)
            if (allowedMacs[i] == mac) return true;  // already present
        for (int i = 0; i < MAX_CONTROLLERS; i++) {
            if (allowedMacs[i].length() == 0) {
                allowedMacs[i] = mac;
                Serial.println("XBOX: Allowed slot " + String(i) + ": " + mac);
                return true;
            }
        }
        Serial.println("XBOX: Allowed list full (" + String(MAX_CONTROLLERS) + " max)");
        return false;
    }

    bool removeAllowedMac(String mac) {
        mac = cleanMac(mac);
        for (int i = 0; i < MAX_CONTROLLERS; i++) {
            if (allowedMacs[i] == mac) {
                allowedMacs[i] = "";
                Serial.println("XBOX: Removed from allowed: " + mac);
                return true;
            }
        }
        return false;
    }

    // Returns formatted "XX:XX:XX:XX:XX:XX" or "" if slot empty
    String getAllowedMac(int index) {
        if (index < 0 || index >= MAX_CONTROLLERS) return "";
        return formatMac(allowedMacs[index]);
    }

    int getMacCount() {
        int n = 0;
        for (int i = 0; i < MAX_CONTROLLERS; i++)
            if (allowedMacs[i].length() > 0) n++;
        return n;
    }

    bool hasAnyMac() { return getMacCount() > 0; }

    // ----------------------------------------------------------------
    // Blacklist management
    // ----------------------------------------------------------------
    void clearBlacklist() {
        for (int i = 0; i < MAX_BLACKLIST; i++) blacklistedMacs[i] = "";
        Serial.println("XBOX: Blacklist cleared");
    }

    bool addToBlacklist(String mac) {
        mac.trim();
        if (mac.length() == 0) return false;
        for (int i = 0; i < MAX_BLACKLIST; i++)
            if (blacklistedMacs[i].equalsIgnoreCase(mac)) return true;
        for (int i = 0; i < MAX_BLACKLIST; i++) {
            if (blacklistedMacs[i].length() == 0) {
                blacklistedMacs[i] = mac;
                Serial.println("XBOX: Blacklisted: " + mac);
                return true;
            }
        }
        Serial.println("XBOX: Blacklist full (" + String(MAX_BLACKLIST) + " max)");
        return false;
    }

    bool removeFromBlacklist(String mac) {
        mac.trim();
        for (int i = 0; i < MAX_BLACKLIST; i++) {
            if (blacklistedMacs[i].equalsIgnoreCase(mac)) {
                blacklistedMacs[i] = "";
                Serial.println("XBOX: Removed from blacklist: " + mac);
                return true;
            }
        }
        return false;
    }

    // Returns stored MAC string at index, "" if empty
    String getBlacklistedMac(int index) {
        if (index < 0 || index >= MAX_BLACKLIST) return "";
        return blacklistedMacs[index];
    }

    int getBlacklistCount() {
        int n = 0;
        for (int i = 0; i < MAX_BLACKLIST; i++)
            if (blacklistedMacs[i].length() > 0) n++;
        return n;
    }

    // ----------------------------------------------------------------
    // Status
    // ----------------------------------------------------------------
    String getConnectedMac() {
        if (!isConnected()) return "";
        return String(lastSeenMacBuf);
    }

    bool isConnected() {
        if (lastSeenMacBuf[0] == 0) return false;
        return (millis() - lastSeenTime < 5000);
    }

    void disconnect() {}

    void resetControllerData() {
        lastSeenMacBuf[0] = 0;
        lastSeenTime  = 0;
        macAutoSaved  = false;
    }
};

extern XboxSimple xboxSimple;

#endif
