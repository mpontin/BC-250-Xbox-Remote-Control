#include <Arduino.h>
#include "LittleFS.h"
#include <ArduinoJson.h>

#include "version.h"
#include "pins.h"
#include "xbox_simple.h"

#include "pc_control.h"

// Global variables
bool pcIsOn = false;
bool forceShutdown = false;
unsigned long forceShutdownStartTime = 0;

// Xbox variables (always enabled — no web UI)
bool xboxEnabled = true;

// Loop timing intervals
unsigned long lastPinRead = 0;
const unsigned long pinReadInterval = 50;

unsigned long lastPcStateHandle = 0;
const unsigned long pcStateHandleInterval = 50;

unsigned long lastButtonDebounce = 0;
const unsigned long debounceDelay = 50;

// Cached pin states
bool cachedButtonState = HIGH;

// Filtered PC state
bool filteredPcState = false;
const unsigned long pcStableDelay = 100;

// Power state machine variables
PowerState powerState = POWER_IDLE;
unsigned long powerStateStartTime = 0;

// Pairing mode
bool pairingMode = false;
unsigned long pairingModeStart = 0;
const unsigned long PAIRING_TIMEOUT_MS = 30000;

// Xbox instance
XboxSimple xboxSimple;

// ================ PROTOTYPES ================
bool getStablePcState();
void startPowerOn();
void startForceShutdown();
void startNormalShutdown();
void saveXboxConfig();

// ================ XBOX FUNCTIONS ================

void saveXboxConfig() {
    File file = LittleFS.open("/xbox_config.json", "w");
    if (!file) return;

    StaticJsonDocument<1024> doc;

    JsonArray macs = doc["macAddresses"].to<JsonArray>();
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        String mac = xboxSimple.getAllowedMac(i);
        if (mac.length() > 0) macs.add(mac);
    }

    JsonArray bl = doc["blacklist"].to<JsonArray>();
    for (int i = 0; i < MAX_BLACKLIST; i++) {
        String mac = xboxSimple.getBlacklistedMac(i);
        if (mac.length() > 0) bl.add(mac);
    }

    serializeJson(doc, file);
    file.close();

    Serial.print("XBOX: Config saved - ");
    Serial.print(xboxSimple.getMacCount());
    Serial.println(" controller(s)");
}

void loadXboxConfig() {
    xboxSimple.clearAllowedMacs();
    xboxSimple.clearBlacklist();

    if (!LittleFS.exists("/xbox_config.json")) {
        Serial.println("XBOX: No config found - hold button 5s to pair a controller");
        return;
    }

    File file = LittleFS.open("/xbox_config.json", "r");
    if (!file) return;

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.println("XBOX: Config invalid");
        return;
    }

    // New array format
    if (doc.containsKey("macAddresses")) {
        JsonArray arr = doc["macAddresses"].as<JsonArray>();
        for (JsonVariant v : arr) xboxSimple.addAllowedMac(v.as<String>());
    }
    // Backwards compatibility: old single-field format
    else if (doc.containsKey("macAddress")) {
        String mac = doc["macAddress"] | "";
        if (mac.length() > 0) xboxSimple.addAllowedMac(mac);
    }

    if (doc.containsKey("blacklist")) {
        JsonArray arr = doc["blacklist"].as<JsonArray>();
        for (JsonVariant v : arr) xboxSimple.addToBlacklist(v.as<String>());
    }

    Serial.print("XBOX: Config loaded - ");
    Serial.print(xboxSimple.getMacCount());
    Serial.println(" controller(s)");
}

// ================ PAIRING MODE ================

void startPairingMode() {
    pairingMode = true;
    pairingModeStart = millis();
    xboxSimple.enterPairingMode();
    Serial.println("PAIRING: Mode started (30s) - press Guide button on controller");
}

// ================ SETUP ================

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== BC-250 STARTING ===");
    Serial.print("Firmware version: ");
    Serial.println(VERSION);

    initPins();
    Serial.println("Pins initialized");

    Serial.print("PC_MONITOR_PIN (4): ");
    Serial.println(digitalRead(PC_MONITOR_PIN) ? "HIGH" : "LOW");
    Serial.print("OPTO_PIN (16): ");
    Serial.println(digitalRead(OPTO_PIN) ? "HIGH" : "LOW");
    Serial.print("EXTRA_PIN (17): ");
    Serial.println(digitalRead(EXTRA_PIN) ? "HIGH" : "LOW");

    filteredPcState = digitalRead(PC_MONITOR_PIN);
    pcIsOn = filteredPcState;

    Serial.println("Mounting LittleFS...");
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed!");
    } else {
        Serial.println("LittleFS mounted");
    }

    Serial.println("Loading Xbox config...");
    loadXboxConfig();

    Serial.println("Setting up BLE...");
    xboxSimple.setupBLE();

    Serial.println("=== BC-250 READY ===");
    Serial.println("  Short press        : Power on / Normal shutdown");
    Serial.println("  Long press 5s (off): Pairing mode");
    Serial.println("  Long press 5s (on) : Force shutdown");
}

void loop() {
    unsigned long now = millis();

    // ================ 2-HOUR IDLE RESTART ================
    static unsigned long pcOffStartTime = 0;

    if (!pcIsOn && powerState == POWER_IDLE) {
        if (pcOffStartTime == 0) {
            pcOffStartTime = now;
            Serial.println("PC off - ESP32 will restart in 2 hours if PC stays off");
        }
        if (now - pcOffStartTime >= 7200000) {
            Serial.println("=== PC off for 2 hours - ESP32 restarting ===");
            delay(1000);
            ESP.restart();
        }
    } else {
        pcOffStartTime = 0;
    }

    // ================ POWER STATE DEBUG ================
    static unsigned long lastStatePrint = 0;
    static PowerState lastPowerState = POWER_IDLE;

    if (powerState != lastPowerState) {
        Serial.print("STATE: ");
        switch (powerState) {
            case POWER_IDLE:                Serial.print("IDLE"); break;
            case POWER_ON_START:            Serial.print("ON_START"); break;
            case POWER_ON_WAITING_RELAY2:   Serial.print("ON_WAITING_RELAY2"); break;
            case POWER_ON_COMPLETE:         Serial.print("ON_COMPLETE"); break;
            case POWER_OFF_START:           Serial.print("OFF_START"); break;
            case POWER_OFF_WAITING:         Serial.print("OFF_WAITING"); break;
            case POWER_OFF_WAITING_POWEROFF:Serial.print("OFF_WAITING_POWEROFF"); break;
            case POWER_FORCE_START:         Serial.print("FORCE_START"); break;
            case POWER_FORCE_WAITING:       Serial.print("FORCE_WAITING"); break;
            default:                        Serial.print("UNKNOWN"); break;
        }
        Serial.print(" (pcIsOn=");
        Serial.print(pcIsOn ? "ON" : "OFF");
        Serial.print(", monitor=");
        Serial.print(digitalRead(PC_MONITOR_PIN) ? "HIGH" : "LOW");
        Serial.println(")");
        lastPowerState = powerState;
        lastStatePrint = now;
    }

    // Print heartbeat every 60 seconds
    if (now - lastStatePrint >= 60000) {
        Serial.print("HEARTBEAT: ");
        Serial.print(now / 1000);
        Serial.print("s - State: ");
        switch (powerState) {
            case POWER_IDLE:
                Serial.print("IDLE");
                if (!pcIsOn) {
                    Serial.print(" (restart in ");
                    Serial.print((7200000 - (now - pcOffStartTime)) / 1000);
                    Serial.print("s)");
                }
                break;
            case POWER_ON_START:            Serial.print("ON_START"); break;
            case POWER_ON_WAITING_RELAY2:   Serial.print("ON_WAITING_RELAY2"); break;
            case POWER_ON_COMPLETE:         Serial.print("ON_COMPLETE"); break;
            case POWER_OFF_START:           Serial.print("OFF_START"); break;
            case POWER_OFF_WAITING:         Serial.print("OFF_WAITING"); break;
            case POWER_OFF_WAITING_POWEROFF:Serial.print("OFF_WAITING_POWEROFF"); break;
            case POWER_FORCE_START:         Serial.print("FORCE_START"); break;
            case POWER_FORCE_WAITING:       Serial.print("FORCE_WAITING"); break;
            default:                        Serial.print("UNKNOWN"); break;
        }
        Serial.print(", PC: ");
        Serial.println(pcIsOn ? "ON" : "OFF");
        lastStatePrint = now;
    }

    // ================ PAIRING MODE ================
    if (pairingMode) {
        // Blink power LED rapidly every 200ms
        digitalWrite(POWER_LED_PIN, (now / 200) % 2);

        if (xboxSimple.wasPairingCompleted()) {
            pairingMode = false;
            digitalWrite(POWER_LED_PIN, LOW);
            Serial.println("PAIRING: Controller saved successfully");
        }

        if (now - pairingModeStart > PAIRING_TIMEOUT_MS) {
            pairingMode = false;
            xboxSimple.exitPairingMode();
            digitalWrite(POWER_LED_PIN, LOW);
            Serial.println("PAIRING: Timeout - no controller found");
        }
    }

    // ================ PIN READ ================
    if (now - lastPinRead >= pinReadInterval) {
        bool newButtonState = digitalRead(BUTTON_PIN);
        if (newButtonState != cachedButtonState) {
            Serial.print("PIN: BUTTON_PIN changed -> ");
            Serial.println(newButtonState ? "HIGH (released)" : "LOW (pressed)");
        }
        cachedButtonState = newButtonState;
        lastPinRead = now;
    }

    // Periodic pin status dump every 5 seconds
    static unsigned long lastPinDump = 0;
    if (now - lastPinDump >= 5000) {
        Serial.print("PINS: BUTTON=");
        Serial.print(digitalRead(BUTTON_PIN) ? "HIGH" : "LOW");
        Serial.print("  PC_MONITOR=");
        Serial.print(digitalRead(PC_MONITOR_PIN) ? "HIGH" : "LOW");
        Serial.print("  RELAY1=");
        Serial.print(digitalRead(OPTO_PIN) ? "HIGH" : "LOW");
        Serial.print("  RELAY2=");
        Serial.println(digitalRead(EXTRA_PIN) ? "HIGH" : "LOW");
        lastPinDump = now;
    }

    // ================ PC STATE HANDLING ================
    if (now - lastPcStateHandle >= pcStateHandleInterval) {
        handlePcStates();
        lastPcStateHandle = now;
    }

    // ================ POWER STATE HANDLING ================
    handlePowerStates();

    // ================ XBOX CONTROLLER HANDLING ================
    xboxSimple.handle();

    // ================ BUTTON HANDLING ================
    static unsigned long buttonPressStartTime = 0;
    static bool buttonPressDetected = false;
    static bool lastStableButtonState = HIGH;

    // Check button state with debounce
    if (cachedButtonState != lastStableButtonState) {
        lastButtonDebounce = now;
        lastStableButtonState = cachedButtonState;
    }

    // State is stable (debounce complete)
    if ((now - lastButtonDebounce) > debounceDelay) {

        // Button pressed down (LOW)
        if (cachedButtonState == LOW && !buttonPressDetected) {
            buttonPressDetected = true;
            buttonPressStartTime = now;
            Serial.println("BUTTON: Pressed down");
        }

        // Button released (HIGH)
        if (cachedButtonState == HIGH && buttonPressDetected) {
            unsigned long pressDuration = now - buttonPressStartTime;
            buttonPressDetected = false;

            Serial.print("BUTTON: Released - duration: ");
            Serial.print(pressDuration);
            Serial.println(" ms");

            if (powerState == POWER_IDLE) {
                bool pcOn = getStablePcState();

                if (pcOn) {
                    // PC IS ON
                    if (pressDuration >= 5000) {
                        Serial.println("BUTTON: Long press - FORCE SHUTDOWN");
                        startForceShutdown();
                    } else {
                        Serial.println("BUTTON: Short press - NORMAL SHUTDOWN");
                        startNormalShutdown();
                    }
                } else {
                    // PC IS OFF
                    if (pressDuration >= 5000) {
                        Serial.println("BUTTON: Long press - PAIRING MODE");
                        startPairingMode();
                    } else {
                        Serial.println("BUTTON: Short press - POWER ON");
                        startPowerOn();
                    }
                }
            } else {
                Serial.print("BUTTON: Power state not IDLE - command rejected. Current state: ");
                Serial.println(powerState);
            }
        }
    }

    // ================ SMALL DELAY ================
    delay(1);
}
