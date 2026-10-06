/*
 * routerd-esp32
 * Embedded High-Performance Wi-Fi Gateway & Telemetry Core
 * Platform: Espressif ESP32 (Xtensa Dual-Core LX6 @ 240MHz)
 */

#include <WiFi.h>
#include "storage.h"
#include "web_dashboard.h"

// Global Managers
StorageManager storage;
WebDashboardManager webDashboard;

// Network addresses (LAN Subnet)
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GATEWAY(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);
static const IPAddress AP_DNS(1, 1, 1, 1); // Cloudflare DNS handed to clients via DHCP

// Hardware AP settings
static const int   AP_MAX_CONN = 4;
static const bool  AP_HIDDEN = false;

// State tracking
static bool apStarted = false;
static bool naptActive = false;

// Telemetry timers
static unsigned long lastTelemetryMs = 0;
static const unsigned long TELEMETRY_INTERVAL_MS = 3000;

void startSoftAP(int channel) {
    if (apStarted) return;

    Serial.printf("[AP] Initializing SoftAP LAN on Channel %d...\n", channel);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET, IPAddress(0, 0, 0, 0), AP_DNS);

    if (WiFi.softAP(storage.config.apSsid.c_str(), storage.config.apPass.c_str(), channel, AP_HIDDEN, AP_MAX_CONN)) {
        apStarted = true;
        Serial.printf("[AP] [OK] SSID '%s' online at %s (Channel %d)\n",
            storage.config.apSsid.c_str(), WiFi.softAPIP().toString().c_str(), channel);
        Serial.printf("[AP] Hardware MAC : %s\n", WiFi.softAPmacAddress().c_str());
    } else {
        Serial.println("[AP] [FAIL] SoftAP failed to start");
    }
}

// Event handler for Wi-Fi lifecycle (AP & STA)
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        // --- Station (WAN) Events ---
        case ARDUINO_EVENT_WIFI_STA_START:
            Serial.println("[STA] Station interface started");
            break;

        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.printf("[STA] Associated with upstream AP: '%s'\n", storage.config.staSsid.c_str());
            break;

        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("[STA] [SUCCESS] IP acquired: %s | GW: %s\n",
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str());
            Serial.printf("[STA] Primary DNS: %s | Channel: %d\n",
                WiFi.dnsIP(0).toString().c_str(), WiFi.channel());

            // Ensure SoftAP is active on matching radio channel
            startSoftAP(WiFi.channel());

            // Release DNS hijack so real internet queries pass cleanly
            webDashboard.stopCaptiveDNS();

            // Activate LwIP NAPT
            Serial.println("[NAPT] Enabling LwIP Network Address & Port Translation...");
            if (WiFi.AP.enableNAPT(true)) {
                naptActive = true;
                Serial.println("[NAPT] [SUCCESS] NAT engine ACTIVE. Downlink packets are now routable!");
            } else {
                naptActive = false;
                Serial.println("[NAPT] [ERROR] Failed to activate LwIP NAPT");
            }
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            naptActive = false;
            static unsigned long lastLog = 0;
            if (millis() - lastLog > 5000) {
                lastLog = millis();
                Serial.printf("[STA] Disconnected! Reason code: %d\n", info.wifi_sta_disconnected.reason);
            }
            break;
        }

        // --- Access Point (LAN) Events ---
        case ARDUINO_EVENT_WIFI_AP_START:
            Serial.println("[AP] SoftAP beaconing active");
            break;

        case ARDUINO_EVENT_WIFI_AP_STOP:
            Serial.println("[AP] SoftAP interface stopped");
            break;

        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.printf("[AP] Client joined: %02X:%02X:%02X:%02X:%02X:%02X | Total clients: %d\n",
                info.wifi_ap_staconnected.mac[0],
                info.wifi_ap_staconnected.mac[1],
                info.wifi_ap_staconnected.mac[2],
                info.wifi_ap_staconnected.mac[3],
                info.wifi_ap_staconnected.mac[4],
                info.wifi_ap_staconnected.mac[5],
                WiFi.softAPgetStationNum());
            break;

        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            Serial.printf("[AP] Client left: %02X:%02X:%02X:%02X:%02X:%02X | Remaining clients: %d\n",
                info.wifi_ap_stadisconnected.mac[0],
                info.wifi_ap_stadisconnected.mac[1],
                info.wifi_ap_stadisconnected.mac[2],
                info.wifi_ap_stadisconnected.mac[3],
                info.wifi_ap_stadisconnected.mac[4],
                info.wifi_ap_stadisconnected.mac[5],
                WiFi.softAPgetStationNum());
            break;

        default:
            break;
    }
}

void printBanner() {
    Serial.println();
    Serial.println("==================================================");
    Serial.println(" routerd-esp32 // Standalone Embedded Appliance   ");
    Serial.println(" Architecture : Xtensa Dual-Core LX6 @ 240MHz    ");
    Serial.println(" Target Board : ESP32 (4MB Flash / 520KB SRAM)   ");
    Serial.println(" Storage      : NVS Flash Key-Value (Preferences)");
    Serial.println(" Management   : http://192.168.4.1/              ");
    Serial.println("==================================================");
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    printBanner();

    // 1. Initialize Non-Volatile Storage (NVS)
    storage.begin();

    // 2. Configure Wi-Fi Lifecycle & Radio Mode
    WiFi.onEvent(onWiFiEvent);
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);

    // 3. Initialize Web Dashboard Engine
    webDashboard.begin();

    // 4. Connect Station (WAN) to saved uplink with channel hint
    if (storage.config.staSsid.length() > 0) {
        Serial.printf("[INIT] Connecting Station (WAN) to saved AP '%s' (Channel %d)...\n",
            storage.config.staSsid.c_str(), storage.config.staChannel);
        if (storage.config.staChannel > 0) {
            WiFi.begin(storage.config.staSsid.c_str(), storage.config.staPass.c_str(), storage.config.staChannel);
        } else {
            WiFi.begin(storage.config.staSsid.c_str(), storage.config.staPass.c_str());
        }
    } else {
        Serial.println("[INIT] No uplink configured. Starting SoftAP on Channel 1...");
        startSoftAP(1);
        webDashboard.startCaptiveDNS();
    }

    Serial.println();
    Serial.println("[READY] Router engine running. Waiting for events...");
    Serial.println("--------------------------------------------------");
}

void loop() {
    unsigned long now = millis();

    // 1. Service HTTP and DNS captive portal requests
    webDashboard.loop();

    // 2. Fallback: Start SoftAP if STA association takes longer than 6 seconds
    if (!apStarted && now > 6000) {
        startSoftAP(storage.config.staChannel > 0 ? storage.config.staChannel : 11);
        webDashboard.startCaptiveDNS();
    }

    // 3. Periodic telemetry snapshot to Serial
    if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
        lastTelemetryMs = now;

        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t minFreeHeap = ESP.getMinFreeHeap();
        uint8_t stationCount = WiFi.softAPgetStationNum();
        unsigned long uptimeSec = now / 1000;
        bool staConnected = (WiFi.status() == WL_CONNECTED);

        Serial.printf("[TELEMETRY] Up: %lus | WAN: %s (%s | %d dBm) | NAT: %s | Clients: %d | Heap: %u B | MinHeap: %u B\n",
            uptimeSec,
            staConnected ? WiFi.localIP().toString().c_str() : "DISCONNECTED",
            staConnected ? storage.config.staSsid.c_str() : "NONE",
            staConnected ? WiFi.RSSI() : 0,
            naptActive ? "ACTIVE" : "OFF",
            stationCount,
            freeHeap,
            minFreeHeap);
    }

    // Yield CPU time to FreeRTOS watchdog & network tasks
    delay(10);
}
