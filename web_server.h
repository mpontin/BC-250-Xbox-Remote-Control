#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WebServer.h>
#include <Update.h>
#include "LittleFS.h"
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include "pc_control.h"
#include "version.h"
#include "pins.h"

extern WebServer server;
extern bool pcIsOn;
extern bool shutdownRequested;
extern bool forceShutdown;
extern PowerState powerState;
extern unsigned long powerStateStartTime;
extern bool xboxEnabled;
extern bool xboxAutoConnect;
extern String wifiSSID;
extern String wifiPassword;
extern bool wifiConfigured;
extern bool apMode;
extern XboxSimple xboxSimple;

// Function prototypes
void saveWiFiConfig(String ssid, String pass);
bool getStablePcState();
void startPowerOn();
void startNormalShutdown();
void startForceShutdown();
void saveXboxConfig(bool enabled, bool autoConnect);

String indexHtml = "";
String updateHtml = "";
String setupHtml = "";
String styleCss = "";
bool filesLoaded = false;

void loadFiles() {
    if (filesLoaded) return;
    
    Serial.println("Loading web files from LittleFS...");
    
    if (LittleFS.exists("/index.html")) {
        File file = LittleFS.open("/index.html", "r");
        indexHtml = file.readString();
        file.close();
        Serial.println("  - index.html loaded");
    } else {
        Serial.println("  - index.html NOT FOUND!");
    }
    
    if (LittleFS.exists("/update.html")) {
        File file = LittleFS.open("/update.html", "r");
        updateHtml = file.readString();
        file.close();
        Serial.println("  - update.html loaded");
    } else {
        Serial.println("  - update.html NOT FOUND!");
    }
    
    if (LittleFS.exists("/setup.html")) {
        File file = LittleFS.open("/setup.html", "r");
        setupHtml = file.readString();
        file.close();
        Serial.println("  - setup.html loaded");
    } else {
        Serial.println("  - setup.html NOT FOUND!");
    }
    
    if (LittleFS.exists("/style.css")) {
        File file = LittleFS.open("/style.css", "r");
        styleCss = file.readString();
        file.close();
        Serial.println("  - style.css loaded");
    } else {
        Serial.println("  - style.css NOT FOUND!");
    }
    
    filesLoaded = true;
}

void setupWebServer() {
    loadFiles();
    
    Serial.println("Setting up web server routes...");
    
    // ========== IMPORTANT: POST routes BEFORE GET routes! ==========

    // FIRMWARE UPDATE - POST (handles file upload)
    server.on("/update", HTTP_POST, []() {
        // Runs when upload is complete
        if (Update.hasError()) {
            server.send(500, "text/plain", "Update failed!");
            Serial.println("Update failed!");
        } else {
            server.send(200, "text/plain", "OK");
            Serial.println("Update successful! Rebooting...");
            delay(1000);
            ESP.restart();
        }
    }, []() {
        // Runs during upload
        HTTPUpload& upload = server.upload();
        
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("Update Start: %s (size: %u bytes)\n", 
                         upload.filename.c_str(), upload.totalSize);
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
            // Print progress every 10%
            static unsigned int lastProgress = 0;
            unsigned int progress = (Update.progress() * 100) / Update.size();
            if (progress / 10 != lastProgress / 10) {
                Serial.printf("Progress: %u%%\n", progress);
                lastProgress = progress;
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf("Update Success: %u bytes\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
        }
    });
    
    // FIRMWARE UPDATE - GET (serves the update page)
    server.on("/update", HTTP_GET, []() {
        server.send(200, "text/html", updateHtml);
    });
    
    // Home page
    server.on("/", []() {
        server.send(200, "text/html", indexHtml);
    });

    // Setup page
    server.on("/setup", []() {
        server.send(200, "text/html", setupHtml);
    });

    // CSS styles
    server.on("/style.css", []() {
        server.send(200, "text/css", styleCss);
    });

    // SVG-logo
    server.on("/steam-machines.svg", []() {
        if (LittleFS.exists("/steam-machines.svg")) {
            File file = LittleFS.open("/steam-machines.svg", "r");
            server.streamFile(file, "image/svg+xml");
            file.close();
        } else {
            server.send(200, "image/svg+xml", 
                "<svg width='180' height='50' xmlns='http://www.w3.org/2000/svg'>"
                "<text x='10' y='35' font-family='Share Tech Mono' font-size='24' fill='#00d9ff'>BC-250</text>"
                "</svg>");
        }
    });

    // API: Bluetooth MAC-osoite
    server.on("/api/bluetooth/mac", HTTP_GET, []() {
        String btMac = BLEDevice::getAddress().toString().c_str();

        StaticJsonDocument<100> doc;
        doc["macAddress"] = btMac;

        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    // API: WiFi-asetukset - GET
    server.on("/api/wifi/config", HTTP_GET, []() {
        StaticJsonDocument<200> doc;
        doc["ssid"] = wifiSSID;
        doc["configured"] = wifiConfigured;
        doc["apMode"] = apMode;
        
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });
    
    // API: WiFi-asetukset - POST
    server.on("/api/wifi/config", HTTP_POST, []() {
        if (!server.hasArg("plain")) {
            server.send(400, "text/plain", "Body missing");
            return;
        }
        
        String body = server.arg("plain");
        
        StaticJsonDocument<200> doc;
        DeserializationError error = deserializeJson(doc, body);
        
        if (error) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }
        
        const char* ssid = doc["ssid"];
        const char* password = doc["password"];
        
        if (!ssid || strlen(ssid) == 0) {
            server.send(400, "text/plain", "SSID required");
            return;
        }
        
        String passStr = password ? String(password) : "";
        
        saveWiFiConfig(String(ssid), passStr);
        server.send(200, "text/plain", "OK");
        delay(1000);
        ESP.restart();
    });
    
    // API: WiFi skannaus
    server.on("/api/wifi/scan", HTTP_GET, []() {
        int n = WiFi.scanComplete();
        if (n == -2) {
            WiFi.scanNetworks(true);
            StaticJsonDocument<100> doc;
            doc["scanning"] = true;

            String response;
            serializeJson(doc, response);
            server.send(200, "application/json", response);
        } else if (n == -1) {
            // Scan already running
            StaticJsonDocument<100> doc;
            doc["scanning"] = true;
            String response;
            serializeJson(doc, response);
            server.send(200, "application/json", response);
        } else if (n >= 0) {
            StaticJsonDocument<2000> doc;
            JsonArray networks = doc.to<JsonArray>();
            
            for (int i = 0; i < n; ++i) {
                JsonObject net = networks.createNestedObject();
                net["ssid"] = WiFi.SSID(i);
                net["rssi"] = WiFi.RSSI(i);
                net["encryption"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? 1 : 0;
            }
            
            String response;
            serializeJson(doc, response);
            WiFi.scanDelete();
            server.send(200, "application/json", response);
        }
    });

    // API: Status
    server.on("/api/status", HTTP_GET, []() {
        bool currentMonitor = getStablePcState();
        bool currentOpto = digitalRead(OPTO_PIN);
        bool currentExtra = digitalRead(EXTRA_PIN);
        
        StaticJsonDocument<300> doc;
        doc["pcOn"] = currentMonitor;
        doc["shutdownRequested"] = shutdownRequested;
        doc["forceShutdown"] = forceShutdown;
        doc["optoState"] = currentOpto;
        doc["extraPinState"] = currentExtra;
        doc["monitorState"] = currentMonitor;
        doc["version"] = VERSION;
        
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    // API: Power ON
    server.on("/power/on", HTTP_POST, []() {
        Serial.println("API: Power ON requested");
        if (getStablePcState() == LOW) {
            startPowerOn();
            server.send(200, "text/plain", "OK");
        } else {
            server.send(200, "text/plain", "Already on");
        }
    });

    // API: Power OFF (pakkosammutus)
    server.on("/power/off", HTTP_POST, []() {
        Serial.println("API: Power OFF requested");
        if (getStablePcState() == HIGH) {
            startForceShutdown();
            server.send(200, "text/plain", "OK");
        } else {
            server.send(200, "text/plain", "Already off");
        }
    });

    // API: Force shutdown
    server.on("/power/force", HTTP_POST, []() {
        Serial.println("API: Force shutdown requested");
        if (getStablePcState() == HIGH) {
            startForceShutdown();
            server.send(200, "text/plain", "OK");
        } else {
            server.send(200, "text/plain", "Already off");
        }
    });

    // API: Xbox configuration - GET
    server.on("/api/xbox/config", HTTP_GET, []() {
        StaticJsonDocument<512> doc;
        doc["enabled"] = xboxEnabled;
        doc["autoConnect"] = xboxAutoConnect;
        doc["macCount"] = xboxSimple.getMacCount();
        doc["maxControllers"] = MAX_CONTROLLERS;

        JsonArray macs = doc.createNestedArray("macAddresses");
        for (int i = 0; i < MAX_CONTROLLERS; i++) {
            String mac = xboxSimple.getAllowedMac(i);
            if (mac.length() > 0) macs.add(mac);
        }

        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });
    
        // LittleFS OTA update
        server.on("/update-fs", HTTP_POST, []() {
            server.sendHeader("Connection", "close");
            server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
            delay(1000);
            ESP.restart();
        }, []() {
            HTTPUpload& upload = server.upload();
            
            if (upload.status == UPLOAD_FILE_START) {
                Serial.printf("LittleFS Update: %s\n", upload.filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {  // HUOM: U_SPIFFS!
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (Update.end(true)) {
                    Serial.printf("LittleFS Update Success: %u bytes\n", upload.totalSize);
                } else {
                    Update.printError(Serial);
                }
            }
        });

    // API: Xbox configuration - POST (full replace of MAC list)
    server.on("/api/xbox/config", HTTP_POST, []() {
        if (!server.hasArg("plain")) {
            server.send(400, "text/plain", "Body missing");
            return;
        }

        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }

        bool enabled = doc["enabled"] | false;
        bool autoConnect = doc["autoConnect"] | false;

        // Only replace the MAC list if macAddresses key is present
        if (doc.containsKey("macAddresses")) {
            xboxSimple.clearAllowedMacs();
            JsonArray arr = doc["macAddresses"].as<JsonArray>();
            for (JsonVariant v : arr) {
                xboxSimple.addAllowedMac(v.as<String>());
            }
        }

        saveXboxConfig(enabled, autoConnect);
        server.send(200, "text/plain", "OK");
        Serial.println("Xbox config saved");
    });

    // API: Add a single MAC to the allowed list
    server.on("/api/xbox/add-mac", HTTP_POST, []() {
        if (!server.hasArg("plain")) {
            server.send(400, "text/plain", "Body missing");
            return;
        }

        StaticJsonDocument<100> doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }

        const char* mac = doc["macAddress"];
        if (!mac || strlen(mac) == 0) {
            server.send(400, "text/plain", "macAddress required");
            return;
        }

        if (xboxSimple.getMacCount() >= MAX_CONTROLLERS) {
            server.send(400, "text/plain", "Maximum controllers reached (" + String(MAX_CONTROLLERS) + ")");
            return;
        }

        if (xboxSimple.addAllowedMac(String(mac))) {
            saveXboxConfig(xboxEnabled, xboxAutoConnect);
            server.send(200, "text/plain", "OK");
        } else {
            server.send(400, "text/plain", "Failed to add MAC");
        }
    });

    // API: Remove a single MAC from the allowed list
    server.on("/api/xbox/remove-mac", HTTP_POST, []() {
        if (!server.hasArg("plain")) {
            server.send(400, "text/plain", "Body missing");
            return;
        }

        StaticJsonDocument<100> doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }

        const char* mac = doc["macAddress"];
        if (!mac) {
            server.send(400, "text/plain", "macAddress required");
            return;
        }

        xboxSimple.removeAllowedMac(String(mac));
        saveXboxConfig(xboxEnabled, xboxAutoConnect);
        server.send(200, "text/plain", "OK");
    });

    // API: Xbox status
    server.on("/api/xbox/status", HTTP_GET, []() {
        String stateStr = "unknown";

        if (!xboxEnabled) {
            stateStr = "disabled";
        } else if (xboxSimple.isConnected()) {
            stateStr = "connected";
        } else {
            stateStr = "disconnected";
        }

        StaticJsonDocument<200> doc;
        doc["state"] = stateStr;
        doc["macCount"] = xboxSimple.getMacCount();
        doc["btAllowed"] = !getStablePcState();
        doc["connectedMac"] = xboxSimple.getConnectedMac();

        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    // API: Get connected controller MAC address
    server.on("/api/xbox/connected-mac", HTTP_GET, []() {
        StaticJsonDocument<200> doc;

        if (xboxSimple.isConnected()) {
            String controllerMac = xboxSimple.getConnectedMac();

            if (controllerMac.length() > 0) {
                doc["connected"] = true;
                doc["macAddress"] = controllerMac;
                doc["note"] = "MAC address retrieved successfully";
            } else {
                doc["connected"] = true;
                doc["macAddress"] = "";
                doc["note"] = "Connected but MAC not available - enter manually";
            }
        } else {
            doc["connected"] = false;
            doc["macAddress"] = "";
            doc["note"] = "No controller connected";
        }

        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    // API: Remove all MAC locks
    server.on("/api/xbox/unlock", HTTP_POST, []() {
        Serial.println("XBOX: Removing all MAC locks");

        xboxSimple.clearAllowedMacs();
        saveXboxConfig(xboxEnabled, xboxAutoConnect);

        StaticJsonDocument<100> doc;
        doc["status"] = "ok";
        doc["message"] = "All MAC locks removed - any controller can wake the PC";

        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    // API: Blacklist - GET
    server.on("/api/xbox/blacklist", HTTP_GET, []() {
        StaticJsonDocument<512> doc;
        doc["count"] = xboxSimple.getBlacklistCount();
        doc["maxBlacklist"] = MAX_BLACKLIST;
        JsonArray bl = doc.createNestedArray("blacklist");
        for (int i = 0; i < MAX_BLACKLIST; i++) {
            String mac = xboxSimple.getBlacklistedMac(i);
            if (mac.length() > 0) bl.add(mac);
        }
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    // API: Blacklist - add
    server.on("/api/xbox/blacklist/add", HTTP_POST, []() {
        if (!server.hasArg("plain")) { server.send(400, "text/plain", "Body missing"); return; }
        StaticJsonDocument<100> doc;
        if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "text/plain", "Invalid JSON"); return; }
        const char* mac = doc["macAddress"];
        if (!mac || strlen(mac) == 0) { server.send(400, "text/plain", "macAddress required"); return; }
        if (xboxSimple.getBlacklistCount() >= MAX_BLACKLIST) {
            server.send(400, "text/plain", "Blacklist full (" + String(MAX_BLACKLIST) + " max)");
            return;
        }
        if (xboxSimple.addToBlacklist(String(mac))) {
            saveXboxConfig(xboxEnabled, xboxAutoConnect);
            server.send(200, "text/plain", "OK");
        } else {
            server.send(400, "text/plain", "Failed");
        }
    });

    // API: Blacklist - remove
    server.on("/api/xbox/blacklist/remove", HTTP_POST, []() {
        if (!server.hasArg("plain")) { server.send(400, "text/plain", "Body missing"); return; }
        StaticJsonDocument<100> doc;
        if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "text/plain", "Invalid JSON"); return; }
        const char* mac = doc["macAddress"];
        if (!mac) { server.send(400, "text/plain", "macAddress required"); return; }
        xboxSimple.removeFromBlacklist(String(mac));
        saveXboxConfig(xboxEnabled, xboxAutoConnect);
        server.send(200, "text/plain", "OK");
    });

    // 404 - Not found
    server.onNotFound([]() {
        server.send(404, "text/plain", "404: Not Found");
    });

    server.begin();
    Serial.println("Web server started!");
    Serial.print("  - IP: ");
    Serial.println(apMode ? WiFi.softAPIP() : WiFi.localIP());
}

#endif // WEB_SERVER_H