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
String activeSsid = "";

// ---------------------------------------------------------------------------
// Uplink manager (STA failover state machine)
// ---------------------------------------------------------------------------
enum UplinkState { UP_IDLE, UP_CONNECTING, UP_CONNECTED, UP_PAUSED };

struct Candidate { int slot; int chan; };

static UplinkState   upState = UP_IDLE;
static Candidate     cands[MAX_NETWORKS];
static int           candN = 0;
static int           candIdx = 0;
static unsigned long attemptStartMs = 0;
static unsigned long nextRescanMs = 0;
static unsigned long rescanBackoffMs = 30000;
static int           fallbackChan = 1;

static const unsigned long ATTEMPT_TIMEOUT_MS = 12000;
static const unsigned long BACKOFF_MAX_MS     = 120000;

// Flags raised by the Wi-Fi event task, consumed by loop(). The event
// callback runs on a different FreeRTOS task, so it must stay short.
static volatile bool    evConnected = false;
static volatile bool    evGotIp = false;
static volatile bool    evDisconnected = false;
static volatile uint8_t evReason = 0;

// Telemetry timers
static unsigned long lastTelemetryMs = 0;
static const unsigned long TELEMETRY_INTERVAL_MS = 3000;

void startSoftAP(int channel) {
    if (apStarted) return;

    Serial.printf("[AP] Initializing SoftAP LAN on Channel %d...\n", channel);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET, IPAddress(0, 0, 0, 0), AP_DNS);

    if (WiFi.softAP(storage.apSsid.c_str(), storage.apPass.c_str(), channel, AP_HIDDEN, AP_MAX_CONN)) {
        apStarted = true;
        Serial.printf("[AP] [OK] SSID '%s' online at %s (Channel %d)\n",
            storage.apSsid.c_str(), WiFi.softAPIP().toString().c_str(), channel);
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
            evConnected = true;
            break;

        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            evGotIp = true;
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            evReason = info.wifi_sta_disconnected.reason;
            evDisconnected = true;
            break;

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

static void tryCandidate() {
    const SavedNetwork& net = storage.networks[cands[candIdx].slot];
    int chan = cands[candIdx].chan;
    activeSsid = net.ssid;
    if (chan > 0) fallbackChan = chan;
    Serial.printf("[UPLINK] Attempt %d/%d: '%s' (Channel %d)\n",
        candIdx + 1, candN, net.ssid.c_str(), chan);
    WiFi.disconnect();
    if (chan > 0) WiFi.begin(net.ssid.c_str(), net.pass.c_str(), chan);
    else          WiFi.begin(net.ssid.c_str(), net.pass.c_str());
    attemptStartMs = millis();
    upState = UP_CONNECTING;
}

// Scan the air and build a candidate list of saved networks that are
// actually visible, ordered by LRU stamp (most recently used first).
// Iterates the raw scan results directly: no fixed-size copy, so a crowded
// RF environment can no longer push a saved network out of view.
static void uplinkRescan() {
    candN = 0;
    candIdx = 0;
    if (storage.count() == 0) { upState = UP_IDLE; return; }

    Serial.println("[UPLINK] Scanning for saved networks...");
    WiFi.disconnect();
    delay(100);
    int16_t n = WiFi.scanNetworks(false, false, false, 200);
    for (int i = 0; i < n; i++) {
        int slot = storage.find(WiFi.SSID(i));
        if (slot < 0) continue;
        bool dup = false;  // same SSID on several BSSIDs: keep first (strongest)
        for (int k = 0; k < candN; k++) if (cands[k].slot == slot) { dup = true; break; }
        if (dup || candN >= MAX_NETWORKS) continue;
        cands[candN++] = { slot, (int)WiFi.channel(i) };
    }
    WiFi.scanDelete();

    // Insertion sort by seq desc. N <= 5, so O(N^2) is cheaper than anything clever.
    for (int i = 1; i < candN; i++) {
        Candidate c = cands[i];
        int j = i - 1;
        while (j >= 0 && storage.networks[cands[j].slot].seq < storage.networks[c.slot].seq) {
            cands[j + 1] = cands[j];
            j--;
        }
        cands[j + 1] = c;
    }

    if (candN == 0) {
        Serial.printf("[UPLINK] No saved network in range. Retry in %lus\n", rescanBackoffMs / 1000);
        upState = UP_IDLE;
        nextRescanMs = millis() + rescanBackoffMs;
        rescanBackoffMs = min(rescanBackoffMs * 2, BACKOFF_MAX_MS);
        startSoftAP(fallbackChan);
        webDashboard.startCaptiveDNS();
        return;
    }
    tryCandidate();
}

static void advanceCandidate(const char* why) {
    Serial.printf("[UPLINK] '%s' failed (%s)\n", activeSsid.c_str(), why);
    if (++candIdx < candN) { tryCandidate(); return; }
    Serial.printf("[UPLINK] All candidates exhausted. Rescan in %lus\n", rescanBackoffMs / 1000);
    WiFi.disconnect();
    activeSsid = "";
    upState = UP_IDLE;
    nextRescanMs = millis() + rescanBackoffMs;
    rescanBackoffMs = min(rescanBackoffMs * 2, BACKOFF_MAX_MS);
    startSoftAP(fallbackChan);
    webDashboard.startCaptiveDNS();
}

// --- Public API used by web_dashboard.h ---
void uplinkConnect(const String& ssid, const String& pass, int chan) {
    int slot = storage.find(ssid);
    if (slot < 0) return;
    cands[0] = { slot, chan };
    candN = 1;
    candIdx = 0;
    rescanBackoffMs = 30000;
    tryCandidate();
}

void uplinkStop() {
    WiFi.disconnect();
    activeSsid = "";
    naptActive = false;
    candN = 0;
    upState = UP_IDLE;
    nextRescanMs = storage.count() > 0 ? millis() + rescanBackoffMs : 0;
    webDashboard.startCaptiveDNS();
}

void uplinkPause() {
    WiFi.disconnect();
    upState = UP_PAUSED;
}

void uplinkResume() {
    if (upState != UP_PAUSED) return;
    if (candN > 0) { candIdx = 0; tryCandidate(); }
    else { upState = UP_IDLE; nextRescanMs = millis(); }
}

static void uplinkLoop(unsigned long now) {
    if (evConnected) {
        evConnected = false;
        Serial.printf("[STA] Associated with '%s'\n", activeSsid.c_str());
    }

    if (evGotIp) {
        evGotIp = false;
        upState = UP_CONNECTED;
        rescanBackoffMs = 30000;
        Serial.printf("[STA] [SUCCESS] IP %s | GW %s | DNS %s | Channel %d\n",
            WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
            WiFi.dnsIP(0).toString().c_str(), WiFi.channel());

        storage.remember(activeSsid);   // NVS write from loop task, not event task
        fallbackChan = WiFi.channel();
        startSoftAP(fallbackChan);      // no-op if already up; radio follows STA channel
        webDashboard.stopCaptiveDNS();

        naptActive = WiFi.AP.enableNAPT(true);
        Serial.println(naptActive ? "[NAPT] [SUCCESS] NAT engine ACTIVE"
                                  : "[NAPT] [ERROR] Failed to activate LwIP NAPT");
    }

    if (evDisconnected) {
        evDisconnected = false;
        uint8_t r = evReason;
        naptActive = false;
        Serial.printf("[STA] Disconnected (reason %u)\n", r);

        if (upState == UP_CONNECTED) {
            // Link dropped after working: retry the same network first.
            candIdx = 0;
            int slot = storage.find(activeSsid);
            if (slot >= 0) { cands[0] = { slot, fallbackChan }; candN = 1; tryCandidate(); }
            else uplinkStop();
        } else if (upState == UP_CONNECTING) {
            // 201 NO_AP_FOUND, 202 AUTH_FAIL, 15/204 handshake timeout: these
            // will not fix themselves by waiting, so move on immediately.
            if (r == 201 || r == 202 || r == 15 || r == 204 || r == 2) {
                char why[24];
                snprintf(why, sizeof(why), "reason %u", r);
                advanceCandidate(why);
            }
        }
    }

    if (upState == UP_CONNECTING && now - attemptStartMs > ATTEMPT_TIMEOUT_MS) {
        advanceCandidate("timeout");
    }

    if (upState == UP_IDLE && nextRescanMs != 0 && (long)(now - nextRescanMs) >= 0) {
        nextRescanMs = 0;
        uplinkRescan();
    }
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
    WiFi.setAutoReconnect(false);  // the uplink manager owns reconnection

    // 3. Initialize Web Dashboard Engine
    webDashboard.begin();

    // 4. Connect Station (WAN): scan, rank saved networks by LRU, try in order.
    if (storage.count() > 0) {
        uplinkRescan();
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

    // 2. Drive the uplink failover state machine
    uplinkLoop(now);

    // 3. Fallback: guarantee the management AP exists even if association
    //    is still in progress. Use the candidate's channel so the single
    //    radio is not forced to hop between AP and STA channels.
    if (!apStarted && now > 6000) {
        startSoftAP(fallbackChan);
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
            staConnected ? activeSsid.c_str() : "NONE",
            staConnected ? WiFi.RSSI() : 0,
            naptActive ? "ACTIVE" : "OFF",
            stationCount,
            freeHeap,
            minFreeHeap);
    }

    // Yield CPU time to FreeRTOS watchdog & network tasks
    delay(10);
}
