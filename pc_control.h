#ifndef PC_CONTROL_H
#define PC_CONTROL_H

#include "pins.h"
#include <Arduino.h>

// Power state machine states
enum PowerState {
    POWER_IDLE,
    POWER_ON_START,
    POWER_ON_WAITING_RELAY2,
    POWER_ON_COMPLETE,
    POWER_OFF_START,
    POWER_OFF_WAITING,
    POWER_OFF_WAITING_POWEROFF,
    POWER_FORCE_START,
    POWER_FORCE_WAITING
};

extern bool pcIsOn;
extern bool forceShutdown;
extern unsigned long forceShutdownStartTime;

// Filter variables
extern bool filteredPcState;
extern const unsigned long pcStableDelay;

// Power state machine variables
extern PowerState powerState;
extern unsigned long powerStateStartTime;

// ================ GLOBAL DEBOUNCE VARIABLES ================
bool debounceLastRaw = false;
unsigned long debounceLastChange = 0;
bool debounceStableState = false;

// ================ DEBOUNCE FUNCTION ================
bool debouncePcState(bool rawState) {
    if (rawState != debounceLastRaw) {
        debounceLastRaw = rawState;
        debounceLastChange = millis();
        Serial.print("DEBOUNCE: Raw state changed -> ");
        Serial.println(rawState ? "HIGH" : "LOW");
    }

    if (millis() - debounceLastChange >= pcStableDelay) {
        if (debounceStableState != rawState) {
            debounceStableState = rawState;
            Serial.print("DEBOUNCE: Filtered state stabilised -> ");
            Serial.println(debounceStableState ? "HIGH" : "LOW");
        }
    }

    return debounceStableState;
}

// ================ getStablePcState ================
bool getStablePcState() {
    return filteredPcState;
}

void initPins() {
    pinMode(OPTO_PIN, OUTPUT);
    pinMode(STATUS_LED_PIN, OUTPUT);
    pinMode(POWER_LED_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(PC_MONITOR_PIN, INPUT_PULLDOWN);
    pinMode(EXTRA_PIN, OUTPUT);

    bool initial = digitalRead(PC_MONITOR_PIN);
    digitalWrite(OPTO_PIN, initial ? HIGH : LOW);
    digitalWrite(POWER_LED_PIN, initial ? HIGH : LOW);
    digitalWrite(STATUS_LED_PIN, HIGH);
    digitalWrite(EXTRA_PIN, LOW);

    // Initialise debounce variables
    debounceLastRaw = initial;
    debounceStableState = initial;
    filteredPcState = initial;

    if (powerState == POWER_IDLE) {
        pcIsOn = initial;
    }

    powerState = POWER_IDLE;
}

// ================ updatePcState() ================
void updatePcState() {
    bool currentMonitor = digitalRead(PC_MONITOR_PIN);
    bool newFilteredState = debouncePcState(currentMonitor);

    if (newFilteredState != filteredPcState) {
        Serial.print(">>> PC STATE CHANGE: ");
        Serial.print(filteredPcState ? "ON" : "OFF");
        Serial.print(" -> ");
        Serial.print(newFilteredState ? "ON" : "OFF");
        Serial.print(" (powerState=");
        Serial.print(powerState);
        Serial.println(")");
        filteredPcState = newFilteredState;
    }

    // Handle PC state changes
    if (filteredPcState != pcIsOn) {

        if (filteredPcState == HIGH) {
            // PC TURNED ON
            Serial.println("*** PC TURNED ON ***");
            pcIsOn = true;
            forceShutdown = false;

            if (powerState == POWER_IDLE) {
                digitalWrite(OPTO_PIN, HIGH);
                digitalWrite(POWER_LED_PIN, HIGH);
            }
        } else {
            // PC TURNED OFF
            Serial.println("*** PC TURNED OFF ***");
            pcIsOn = false;
            forceShutdown = false;

            if (powerState == POWER_IDLE) {
                digitalWrite(OPTO_PIN, LOW);
                digitalWrite(POWER_LED_PIN, LOW);
            }
        }
    }
}

// POWER ON
void startPowerOn() {
    if (filteredPcState == HIGH) {
        Serial.println("PC already on");
        return;
    }

    if (powerState != POWER_IDLE) {
        Serial.println("Power operation already in progress");
        return;
    }

    Serial.println("=== POWER ON SEQUENCE STARTED ===");
    powerState = POWER_ON_START;
    powerStateStartTime = millis();
}

// NORMAL SHUTDOWN
void startNormalShutdown() {
    if (filteredPcState == LOW) {
        Serial.println("PC already off");
        return;
    }

    if (powerState != POWER_IDLE) {
        Serial.println("Power operation already in progress");
        return;
    }

    Serial.println("=== NORMAL SHUTDOWN STARTED ===");
    powerState = POWER_OFF_START;
    powerStateStartTime = millis();
}

// FORCE SHUTDOWN
void startForceShutdown() {
    if (filteredPcState == LOW) {
        Serial.println("PC already off");
        return;
    }

    if (powerState != POWER_IDLE) {
        Serial.println("Power operation already in progress");
        return;
    }

    Serial.println("=== FORCE SHUTDOWN STARTED ===");
    powerState = POWER_FORCE_START;
    powerStateStartTime = millis();
}

// POWER STATE MANAGEMENT
void handlePowerStates() {
    unsigned long now = millis();

    switch (powerState) {
        case POWER_ON_START:
            Serial.println("POWER ON START - Setting relays");
            digitalWrite(OPTO_PIN, HIGH);
            digitalWrite(EXTRA_PIN, HIGH);
            digitalWrite(POWER_LED_PIN, HIGH);
            powerState = POWER_ON_WAITING_RELAY2;
            powerStateStartTime = now;
            break;

        case POWER_ON_WAITING_RELAY2:
            if (now - powerStateStartTime >= 1000) {
                Serial.println("RELAY 2: 1s passed - turning EXTRA_PIN OFF");
                digitalWrite(EXTRA_PIN, LOW);
                powerState = POWER_ON_COMPLETE;
                powerStateStartTime = now;
            }
            break;

        case POWER_ON_COMPLETE:
            if (now - powerStateStartTime >= 8000) {
                if (filteredPcState == HIGH) {  // USE FILTERED STATE
                    Serial.println("PC power-on confirmed - PC is now running");
                } else {
                    Serial.println("WARNING: PC did not power on! Turning relay OFF");
                    digitalWrite(OPTO_PIN, LOW);
                }
                powerState = POWER_IDLE;
            }
            break;

        case POWER_OFF_START:
            Serial.println("Normal shutdown - EXTRA_PIN HIGH for 500ms");
            digitalWrite(EXTRA_PIN, HIGH);
            digitalWrite(POWER_LED_PIN, LOW);
            powerState = POWER_OFF_WAITING;
            powerStateStartTime = now;
            break;

        case POWER_OFF_WAITING:
            if (now - powerStateStartTime >= 500) {
                Serial.println("Shutdown pulse complete - releasing EXTRA_PIN");
                digitalWrite(EXTRA_PIN, LOW);
                powerState = POWER_OFF_WAITING_POWEROFF;
                powerStateStartTime = now;
            }
            break;

        case POWER_OFF_WAITING_POWEROFF:
            // Wait for PC to shut down (filteredPcState goes LOW) and stay low for 4 seconds
            if (filteredPcState == LOW) {  // USE FILTERED STATE
                if (now - powerStateStartTime >= 4000) {
                    Serial.println("PC power-off confirmed - turning relay OFF");
                    digitalWrite(OPTO_PIN, LOW);
                    digitalWrite(POWER_LED_PIN, LOW);
                    powerState = POWER_IDLE;

                    Serial.println("PC OFF - Controller reset handled elsewhere");
                }
            } else {
                // PC has not shut down yet — reset timer
                powerStateStartTime = now;
            }
            break;

        case POWER_FORCE_START:
            Serial.println("Force shutdown - OPTO_PIN HIGH for 5000ms");
            digitalWrite(OPTO_PIN, HIGH);
            digitalWrite(POWER_LED_PIN, HIGH);
            powerState = POWER_FORCE_WAITING;
            powerStateStartTime = now;
            break;

        case POWER_FORCE_WAITING:
            if (now - powerStateStartTime >= 5000) {
                Serial.println("Force shutdown pulse complete - waiting for PC to power off");
                digitalWrite(OPTO_PIN, LOW);
                forceShutdown = true;
                forceShutdownStartTime = now;
                powerState = POWER_OFF_WAITING_POWEROFF;
                powerStateStartTime = now;
            }
            break;

        default:
            break;
    }
}

void handlePcStates() {
    if (forceShutdown) {
        if (filteredPcState == LOW) {
            forceShutdown = false;
            digitalWrite(OPTO_PIN, LOW);
            digitalWrite(POWER_LED_PIN, LOW);
        }
    }

    // BLE scanning is managed by xbox_simple.h — onResult() checks
    // getStablePcState() directly, so no explicit enable/disable needed here.

    updatePcState();
}

#endif // PC_CONTROL_H
