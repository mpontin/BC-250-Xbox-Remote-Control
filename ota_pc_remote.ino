#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include "LittleFS.h"
#include <ArduinoJson.h>

#include "version.h"
#include "pins.h"
#include "xbox_simple.h"

// IMPORTANT: Include pc_control.h FIRST so PowerState is known
#include "pc_control.h"

// Global variables
WebServer server(80);

bool pcIsOn = false;
bool shutdownRequested = false;
bool forceShutdown = false;
unsigned long forceShutdownStartTime = 0;
const unsigned long forceShutdownDuration = 5000;

// WiFi variables
String wifiSSID = "";
String wifiPassword = "";
bool wifiConfigured = false;
bool apMode = false;

// Xbox variables
bool xboxEnabled = false;
bool xboxAutoConnect = false;

// Loop timing intervals
unsigned long lastPinRead = 0;
const unsigned long pinReadInterval = 50;

unsigned long lastServerHandle = 0;
const unsigned long serverHandleInterval = 20;

unsigned long lastPcStateHandle = 0;
const unsigned long pcStateHandleInterval = 50;

unsigned long lastButtonDebounce = 0;
const unsigned long debounceDelay = 50;

// Cached pin states
bool cachedButtonState = HIGH;
bool lastStableButtonState = HIGH;
bool buttonPressed = false;

// Filtered PC state
bool filteredPcState = false;
unsigned long lastPcChangeTime = 0;
const unsigned long pcStableDelay = 100;

// Power state machine variables
PowerState powerState = POWER_IDLE;
unsigned long powerStateStartTime = 0;

// Xbox instance
XboxSimple xboxSimple;

// ================ PROTOTYPES ================
bool getStablePcState();
void startPowerOn();
void startForceShutdown();
void startNormalShutdown();
void saveXboxConfig(bool enabled, bool autoConnect);

// ================ WEB SERVER (includes route handlers) ================
#include "web_server.h"

// ================ WiFi CONFIGURATION ================

void saveWiFiConfig(String ssid, String pass) {
    File file = LittleFS.open("/wifi_config.json", "w");
    if (!file) return;
    
    StaticJsonDocument<200> doc;
    doc["ssid"] = ssid;
    doc["password"] = pass;
    
    serializeJson(doc, file);
    file.close();
    
    wifiSSID = ssid;
    wifiPassword = pass;
    wifiConfigured = true;
}

void loadWiFiConfig() {
    if (!LittleFS.begin(true)) {
        wifiConfigured = false;
        apMode = true;
        return;
    }
    
    if (!LittleFS.exists("/wifi_config.json")) {
        wifiConfigured = false;
        apMode = true;
        return;
    }
    
    File file = LittleFS.open("/wifi_config.json", "r");
    if (!file) return;
    
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        wifiConfigured = false;
        apMode = true;
        return;
    }
    
    wifiSSID = doc["ssid"] | "";
    wifiPassword = doc["password"] | "";
    
    wifiConfigured = (wifiSSID.length() > 0);
    apMode = !wifiConfigured;
}

bool connectToWiFi() {
    loadWiFiConfig();
    
    if (!wifiConfigured || wifiSSID.length() == 0) {
        apMode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("BC-250-POWER-CONTROL", "bc250admin");
        return true;
    }

    WiFi.mode(WIFI_STA);
    String hostname = "bc250-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    WiFi.setHostname(hostname.c_str());
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(100);
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        apMode = false;
        return true;
    } else {
        apMode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("BC-250-POWER-CONTROL", "bc250admin");
        return false;
    }
}

// ================ PC STATE FILTERING ================



// ================ XBOX FUNCTIONS ================

void saveXboxConfig(bool enabled, bool autoConnect) {
    File file = LittleFS.open("/xbox_config.json", "w");
    if (!file) return;

    StaticJsonDocument<768> doc;
    doc["enabled"] = enabled;
    doc["autoConnect"] = autoConnect;

    JsonArray macs = doc.createNestedArray("macAddresses");
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        String mac = xboxSimple.getAllowedMac(i);
        if (mac.length() > 0) macs.add(mac);
    }

    JsonArray bl = doc.createNestedArray("blacklist");
    for (int i = 0; i < MAX_BLACKLIST; i++) {
        String mac = xboxSimple.getBlacklistedMac(i);
        if (mac.length() > 0) bl.add(mac);
    }

    serializeJson(doc, file);
    file.close();

    xboxEnabled = enabled;
    xboxAutoConnect = autoConnect;

    Serial.print("XBOX: Config saved - ");
    Serial.print(xboxSimple.getMacCount());
    Serial.print(" allowed, ");
    Serial.print(xboxSimple.getBlacklistCount());
    Serial.println(" blacklisted");
}

void loadXboxConfig() {
    xboxSimple.clearAllowedMacs();
    xboxSimple.clearBlacklist();

    if (!LittleFS.exists("/xbox_config.json")) {
        xboxEnabled = false;
        xboxAutoConnect = false;
        Serial.println("XBOX: No config found - all controllers allowed");
        return;
    }

    File file = LittleFS.open("/xbox_config.json", "r");
    if (!file) return;

    StaticJsonDocument<768> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        xboxEnabled = false;
        xboxAutoConnect = false;
        Serial.println("XBOX: Config invalid - all controllers allowed");
        return;
    }

    xboxEnabled   = doc["enabled"]     | false;
    xboxAutoConnect = doc["autoConnect"] | false;

    // Allowed list (new array format)
    if (doc.containsKey("macAddresses")) {
        JsonArray arr = doc["macAddresses"].as<JsonArray>();
        for (JsonVariant v : arr) xboxSimple.addAllowedMac(v.as<String>());
    }
    // Backwards compatibility: old single-field format
    else if (doc.containsKey("macAddress")) {
        String mac = doc["macAddress"] | "";
        if (mac.length() > 0) xboxSimple.addAllowedMac(mac);
    }

    // Blacklist
    if (doc.containsKey("blacklist")) {
        JsonArray arr = doc["blacklist"].as<JsonArray>();
        for (JsonVariant v : arr) xboxSimple.addToBlacklist(v.as<String>());
    }

    Serial.print("XBOX: Config loaded - ");
    Serial.print(xboxSimple.getMacCount());
    Serial.print(" allowed, ");
    Serial.print(xboxSimple.getBlacklistCount());
    Serial.print(" blacklisted, enabled: ");
    Serial.println(xboxEnabled);
}

// ================ SETUP ================

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== BC-250 STARTING ===");
    
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
    
    Serial.println("Loading WiFi config...");
    loadWiFiConfig();
    
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        Serial.printf("WiFi: client connected    — MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
            info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
            info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5]);
    }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        Serial.printf("WiFi: client disconnected — MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            info.wifi_ap_stadisconnected.mac[0], info.wifi_ap_stadisconnected.mac[1],
            info.wifi_ap_stadisconnected.mac[2], info.wifi_ap_stadisconnected.mac[3],
            info.wifi_ap_stadisconnected.mac[4], info.wifi_ap_stadisconnected.mac[5]);
    }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);

    Serial.println("Connecting to WiFi...");
    connectToWiFi();
    
    Serial.print("WiFi mode: ");
    Serial.println(apMode ? "AP" : "STA");
    if (!apMode) {
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.print("AP IP address: ");
        Serial.println(WiFi.softAPIP());
    }
    
    Serial.println("Mounting LittleFS...");
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed!");
    } else {
        Serial.println("LittleFS mounted");
    }

    Serial.println("Setting up BLE...");
    xboxSimple.setupBLE();

    Serial.println("Loading Xbox config...");
    loadXboxConfig();

    Serial.println("BLE ready - waiting for controller");
    
    Serial.println("Setting up web server...");
    setupWebServer();
    
    Serial.println("=== BC-250 READY ===\n");
}

void loop() {
    unsigned long now = millis();
    
    // ================ 2-HOUR IDLE RESTART ================
    static unsigned long pcOffStartTime = 0;

    // Track how long the PC has been off
    if (!pcIsOn && powerState == POWER_IDLE) {
        if (pcOffStartTime == 0) {
            pcOffStartTime = now;
            Serial.println("PC off - ESP32 will restart in 2 hours if PC stays off");
        }

        // IF PC HAS BEEN OFF FOR MORE THAN 2 HOURS, RESTART ESP32
        if (now - pcOffStartTime >= 7200000) { // 2 hours = 7200000 ms
            Serial.println("=== PC off for 2 hours - ESP32 restarting ===");
            delay(1000);
            ESP.restart();
        }
    } else {
        // PC is on or starting up — reset timer
        pcOffStartTime = 0;
    }
    
    // ================ POWER STATE DEBUG ================
    static unsigned long lastStatePrint = 0;
    static PowerState lastPowerState = POWER_IDLE;
    
    if (powerState != lastPowerState) {
        // State changed — print new state
        Serial.print("STATE: ");
        switch(powerState) {
            case POWER_IDLE: Serial.print("IDLE"); break;
            case POWER_ON_START: Serial.print("ON_START"); break;
            case POWER_ON_WAITING_RELAY2: Serial.print("ON_WAITING_RELAY2"); break;
            case POWER_ON_COMPLETE: Serial.print("ON_COMPLETE"); break;
            case POWER_OFF_START: Serial.print("OFF_START"); break;
            case POWER_OFF_WAITING: Serial.print("OFF_WAITING"); break;
            case POWER_OFF_WAITING_POWEROFF: Serial.print("OFF_WAITING_POWEROFF"); break;
            case POWER_FORCE_START: Serial.print("FORCE_START"); break;
            case POWER_FORCE_WAITING: Serial.print("FORCE_WAITING"); break;
            default: Serial.print("UNKNOWN"); break;
        }
        Serial.print(" (pcIsOn=");
        Serial.print(pcIsOn ? "ON" : "OFF");
        Serial.print(", monitor=");
        Serial.print(digitalRead(PC_MONITOR_PIN) ? "HIGH" : "LOW");
        Serial.println(")");
        lastPowerState = powerState;
        lastStatePrint = now;
    }
    
    // Print state every 60 seconds
    if (now - lastStatePrint >= 60000) {
        Serial.print("HEARTBEAT: ");
        Serial.print(millis() / 1000);
        Serial.print("s - State: ");
        switch(powerState) {
            case POWER_IDLE:
                Serial.print("IDLE");
                if (!pcIsOn) {
                    Serial.print(" (restart in ");
                    Serial.print((7200000 - (now - pcOffStartTime)) / 1000);
                    Serial.print("s)");
                }
                break;
            case POWER_ON_START: Serial.print("ON_START"); break;
            case POWER_ON_WAITING_RELAY2: Serial.print("ON_WAITING_RELAY2"); break;
            case POWER_ON_COMPLETE: Serial.print("ON_COMPLETE"); break;
            case POWER_OFF_START: Serial.print("OFF_START"); break;
            case POWER_OFF_WAITING: Serial.print("OFF_WAITING"); break;
            case POWER_OFF_WAITING_POWEROFF: Serial.print("OFF_WAITING_POWEROFF"); break;
            case POWER_FORCE_START: Serial.print("FORCE_START"); break;
            case POWER_FORCE_WAITING: Serial.print("FORCE_WAITING"); break;
            default: Serial.print("UNKNOWN"); break;
        }
        Serial.print(", PC: ");
        Serial.print(pcIsOn ? "ON" : "OFF");
        Serial.println();
        lastStatePrint = now;
    }

    // ================ PIN READ ================
    if (now - lastPinRead >= pinReadInterval) {
        bool newButtonState = digitalRead(BUTTON_PIN);
        if (newButtonState != cachedButtonState) {
            Serial.print("PIN: BUTTON_PIN (22) changed -> ");
            Serial.println(newButtonState ? "HIGH (released)" : "LOW (pressed)");
        }
        cachedButtonState = newButtonState;
        lastPinRead = now;
    }

    // Periodic pin status dump every 5 seconds
    static unsigned long lastPinDump = 0;
    if (now - lastPinDump >= 5000) {
        Serial.print("PINS: BUTTON(22)=");
        Serial.print(digitalRead(BUTTON_PIN) ? "HIGH" : "LOW");
        Serial.print("  PC_MONITOR(4)=");
        Serial.print(digitalRead(PC_MONITOR_PIN) ? "HIGH" : "LOW");
        Serial.print("  RELAY1(16)=");
        Serial.print(digitalRead(OPTO_PIN) ? "HIGH" : "LOW");
        Serial.print("  RELAY2(17)=");
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

    // ================ WEB SERVER ================
    server.handleClient();

    // ================ XBOX CONTROLLER HANDLING ================
    // Called every iteration: tracks PC shutdown, processes wake trigger.
    // BLE scanning itself runs in the background via the BLE stack task.
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

            // Check PC state (only in IDLE)
            if (powerState == POWER_IDLE) {
                bool pcOn = getStablePcState();

                if (pcOn) {
                    // PC IS ON
                    if (pressDuration >= 5000) {
                        // Long press (>5s) = FORCE SHUTDOWN
                        Serial.println("BUTTON: Long press (>5s) - FORCE SHUTDOWN");
                        startForceShutdown();
                    } else {
                        // Short press (<5s) = NORMAL SHUTDOWN
                        Serial.println("BUTTON: Short press (<5s) - NORMAL SHUTDOWN");
                        startNormalShutdown();
                    }
                } else {
                    // PC IS OFF
                    Serial.println("BUTTON: PC off - POWER ON");
                    startPowerOn();
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