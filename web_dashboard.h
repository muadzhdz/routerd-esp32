#pragma once

#include <WebServer.h>
#include <DNSServer.h>
#include <WiFi.h>
#include "storage.h"

extern String activeSsid;

// Uplink (STA) manager, implemented in routerd-esp32.ino. Single owner of
// WiFi.begin/disconnect so the failover state machine never gets confused.
void uplinkConnect(const String& ssid, const String& pass, int chan);
void uplinkStop();
void uplinkPause();
void uplinkResume();

class WebDashboardManager {
private:
    WebServer server{80};
    DNSServer dnsServer;
    bool captiveDnsRunning = false;
    const byte DNS_PORT = 53;

    static const char DASHBOARD_HTML[] PROGMEM;
    static const char LOGO_SVG[] PROGMEM;

public:
    void begin() {
        server.on("/", HTTP_GET, [this]() { handleRoot(); });
        server.on("/api/status", HTTP_GET, [this]() { handleApiStatus(); });
        server.on("/scan", HTTP_GET, [this]() { handleScan(); });
        server.on("/save", HTTP_POST, [this]() { handleSave(); });
        server.on("/clear", HTTP_POST, [this]() { handleClear(); });
        server.on("/forget", HTTP_POST, [this]() { handleForget(); });
        server.on("/reboot", HTTP_POST, [this]() { handleReboot(); });
        server.on("/favicon.svg", HTTP_GET, [this]() { handleFavicon(); });

        // Captive Portal Probe Redirects
        server.on("/generate_204", HTTP_GET, [this]() { handleRedirect(); });
        server.on("/gen_204", HTTP_GET, [this]() { handleRedirect(); });
        server.on("/hotspot-detect.html", HTTP_GET, [this]() { handleRedirect(); });
        server.on("/ncsi.txt", HTTP_GET, [this]() { handleRedirect(); });
        server.on("/connecttest.txt", HTTP_GET, [this]() { handleRedirect(); });

        server.onNotFound([this]() { handleNotFound(); });

        server.begin();
        Serial.println("[HTTP] Web Dashboard active on http://192.168.4.1:80");
    }

    void startCaptiveDNS() {
        if (!captiveDnsRunning) {
            dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
            captiveDnsRunning = true;
            Serial.println("[DNS] Captive Portal DNS interceptor ACTIVE (Port 53)");
        }
    }

    void stopCaptiveDNS() {
        if (captiveDnsRunning) {
            dnsServer.stop();
            captiveDnsRunning = false;
            Serial.println("[DNS] Captive Portal DNS stopped (Internet routing engaged)");
        }
    }

    void loop() {
        if (captiveDnsRunning) {
            dnsServer.processNextRequest();
        }
        server.handleClient();
    }

private:
    // RFC 8259: inside a JSON string, '"', '\' and control chars < 0x20
    // must be escaped. SSIDs are raw bytes from the air, so assume hostile.
    static String jsonEscape(const String& in) {
        String out;
        out.reserve(in.length() + 8);
        for (size_t i = 0; i < in.length(); i++) {
            char c = in[i];
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if ((uint8_t)c < 0x20) {
                char buf[7];
                snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
                out += buf;
            } else out += c;
        }
        return out;
    }

    static String htmlEscape(const String& in) {
        String out;
        out.reserve(in.length() + 8);
        for (size_t i = 0; i < in.length(); i++) {
            switch (in[i]) {
                case '&': out += "&amp;";  break;
                case '<': out += "&lt;";   break;
                case '>': out += "&gt;";   break;
                case '"': out += "&quot;"; break;
                case '\'': out += "&#39;"; break;
                default:  out += in[i];
            }
        }
        return out;
    }

    // The HTTP server listens on every interface, including the WAN (STA)
    // side. Anyone on the upstream LAN could otherwise wipe or reboot us.
    // Only accept state-changing requests that arrived via the SoftAP.
    bool requireLan() {
        if (server.client().localIP() == WiFi.softAPIP()) return true;
        server.send(403, "text/plain", "Forbidden: manage routerd from its own AP");
        return false;
    }

    void handleRoot() {
        server.send_P(200, "text/html", DASHBOARD_HTML);
    }

    void handleFavicon() {
        server.send_P(200, "image/svg+xml", LOGO_SVG);
    }

    void handleRedirect() {
        server.sendHeader("Location", "http://192.168.4.1/", true);
        server.send(302, "text/plain", "");
    }

    void handleNotFound() {
        if (server.hostHeader() != "192.168.4.1") {
            handleRedirect();
        } else {
            server.send(404, "text/plain", "Not Found");
        }
    }

    void handleApiStatus() {
        bool connected = (WiFi.status() == WL_CONNECTED);
        String json = "{";
        json += "\"uplink_ssid\":\"" + jsonEscape(activeSsid) + "\",";
        json += "\"uplink_connected\":" + String(connected ? "true" : "false") + ",";
        json += "\"uplink_ip\":\"" + (connected ? WiFi.localIP().toString() : "--") + "\",";
        json += "\"uplink_rssi\":" + String(connected ? WiFi.RSSI() : 0) + ",";
        json += "\"uplink_chan\":" + String(connected ? WiFi.channel() : 0) + ",";
        json += "\"ap_ssid\":\"" + jsonEscape(storage.apSsid) + "\",";
        json += "\"ap_clients\":" + String(WiFi.softAPgetStationNum()) + ",";
        json += "\"saved_count\":" + String(storage.count()) + ",";
        json += "\"saved\":[";
        int shown = 0;
        for (int i = 0; i < MAX_NETWORKS; i++) {
            if (!storage.networks[i].used) continue;
            if (shown++ > 0) json += ",";
            json += "\"" + jsonEscape(storage.networks[i].ssid) + "\"";
        }
        json += "],";
        json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
        json += "\"uptime\":" + String(millis() / 1000);
        json += "}";
        server.send(200, "application/json", json);
    }

    void handleScan() {
        Serial.println("[SCAN] Initiating 2.4GHz RF spectrum scan...");
        // A connected STA can scan between beacons without dropping the
        // link. Only an STA stuck in a reconnect loop blocks the scanner
        // (returns WIFI_SCAN_RUNNING), so we only pause it in that case.
        bool wasConnected = (WiFi.status() == WL_CONNECTED);
        if (!wasConnected) {
            uplinkPause();
            delay(100);
        }

        int16_t n = WiFi.scanNetworks(false, false, false, 150);
        if (n < 0) {
            WiFi.scanDelete();
            delay(100);
            n = WiFi.scanNetworks(false, false, false, 200);
        }

        Serial.printf("[SCAN] Discovered %d networks\n", n);
        String json = "[";
        int validCount = 0;
        for (int i = 0; i < n; ++i) {
            String raw = WiFi.SSID(i);
            if (raw.length() == 0) continue;
            if (validCount > 0) json += ",";
            json += "{";
            json += "\"ssid\":\"" + jsonEscape(raw) + "\",";
            json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
            json += "\"chan\":" + String(WiFi.channel(i)) + ",";
            json += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + ",";
            json += "\"saved\":" + String(storage.isSaved(raw) ? "true" : "false");
            json += "}";
            validCount++;
        }
        json += "]";
        WiFi.scanDelete();
        server.send(200, "application/json", json);

        if (!wasConnected) uplinkResume();
    }

    void handleSave() {
        if (!requireLan()) return;
        if (!server.hasArg("ssid")) {
            server.send(400, "text/plain", "Missing SSID parameter");
            return;
        }

        String newSsid = server.arg("ssid");
        String newPass = server.hasArg("pass") ? server.arg("pass") : "";
        int chan = server.hasArg("chan") ? server.arg("chan").toInt() : 0;

        // 802.11: SSID is 1..32 bytes. WPA2-PSK passphrase is 8..63 chars
        // (or empty for an open network). Reject early instead of letting
        // the supplicant fail silently with reason 15 (4-way handshake).
        if (newSsid.length() == 0 || newSsid.length() > 32) {
            server.send(400, "text/plain", "SSID must be 1-32 bytes");
            return;
        }
        if (newPass.length() != 0 && (newPass.length() < 8 || newPass.length() > 63)) {
            server.send(400, "text/plain", "Password must be empty or 8-63 chars");
            return;
        }
        if (chan < 0 || chan > 13) chan = 0;

        Serial.printf("[HTTP] Uplink config received: SSID='%s' Chan=%d\n", newSsid.c_str(), chan);
        storage.upsert(newSsid, newPass);

        String response = "<!DOCTYPE html><html lang='id' data-theme='dark'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><link rel='icon' href='/favicon.svg' type='image/svg+xml'><style>:root{--bg:#0a0a0a;--panel:#111111;--line:rgba(255,255,255,0.1);--heading:#f0f0f0;--mono:ui-monospace,monospace;}body{background:var(--bg);color:var(--heading);font-family:var(--mono);padding:32px 16px;text-align:center;margin:0;box-sizing:border-box;min-height:100vh;display:flex;}.box{max-width:400px;width:100%;margin:auto;background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:24px;}.brand{font-size:13px;letter-spacing:1px;color:#888;margin-bottom:12px;}.title{font-size:16px;font-weight:700;margin-bottom:8px;}</style></head><body><div class='box'><div class='brand'>ROUTERD // EMBEDDED</div><div class='title'>KREDENSIAL DISIMPAN</div><p style='color:#aaa;font-size:13px;'>Menyambungkan ke <b>" + htmlEscape(newSsid) + "</b>...</p><p style='color:#666;font-size:12px;margin-top:12px;'>Membuka dashboard dalam 4 detik...</p></div><script>setTimeout(()=>{window.location.href='/';},4000);</script></body></html>";
        server.send(200, "text/html", response);

        delay(100);
        uplinkConnect(newSsid, newPass, chan);
    }

    void handleClear() {
        if (!requireLan()) return;
        storage.clearAll();
        uplinkStop();
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    }

    void handleForget() {
        if (!requireLan()) return;
        if (!server.hasArg("ssid")) {
            server.send(400, "text/plain", "Missing SSID parameter");
            return;
        }
        String ssid = server.arg("ssid");
        bool gone = storage.forget(ssid);
        if (activeSsid == ssid) uplinkStop();
        Serial.printf("[HTTP] forget '%s' -> %s\n", ssid.c_str(), gone ? "removed" : "not found");
        server.send(gone ? 200 : 404, "application/json", gone ? "{\"removed\":true}" : "{\"removed\":false}");
    }

    void handleReboot() {
        if (!requireLan()) return;
        server.send(200, "text/html", "Restarting ESP32 Gateway...");
        delay(500);
        ESP.restart();
    }
};

extern WebDashboardManager webDashboard;

const char WebDashboardManager::DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id" data-theme="dark">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Routerd // Embedded Gateway</title>
  <link rel="icon" href="/favicon.svg" type="image/svg+xml">
  <style>
    :root {
      color-scheme: dark;
      --bg: #0a0a0a;
      --panel: #111111;
      --panel-2: #161616;
      --line: rgba(255,255,255,0.07);
      --line-strong: rgba(255,255,255,0.13);
      --line-soft: rgba(255,255,255,0.04);
      --heading: #f0f0f0;
      --text: #d4d4d4;
      --muted: #888888;
      --faint: #444444;
      --accent: #ffffff;
      --accent-contrast: #0a0a0a;
      --mono: ui-monospace, "SF Mono", "Cascadia Code", "Fira Code", monospace;
      --sans: "Inter", -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      --radius-sm: 4px;
      --radius: 6px;
      --radius-md: 8px;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: var(--bg);
      color: var(--text);
      font-family: var(--sans);
      min-height: 100vh;
      display: flex;
      justify-content: center;
      padding: 16px;
      -webkit-font-smoothing: antialiased;
    }
    .wrapper {
      width: 100%;
      max-width: 520px;
      display: flex;
      flex-direction: column;
      gap: 14px;
    }

    .market-header {
      background: var(--panel);
      border: 1px solid var(--line-strong);
      border-radius: var(--radius-md);
      padding: 12px 16px;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .market-brand {
      display: flex;
      align-items: center;
      gap: 10px;
    }
    .brand-title {
      font-family: var(--mono);
      font-size: 13px;
      font-weight: 800;
      letter-spacing: 0.05em;
      color: var(--heading);
    }
    .brand-badge {
      font-family: var(--mono);
      font-size: 10px;
      font-weight: 600;
      padding: 2px 6px;
      background: rgba(255,255,255,0.06);
      border: 1px solid var(--line);
      border-radius: var(--radius-sm);
      color: var(--muted);
      letter-spacing: 0.04em;
    }
    .status-pill {
      font-family: var(--mono);
      font-size: 10px;
      font-weight: 700;
      padding: 3px 8px;
      border-radius: var(--radius-sm);
      background: rgba(255,255,255,0.05);
      border: 1px solid var(--line);
      color: var(--muted);
    }
    .status-pill.online {
      color: var(--accent);
      border-color: rgba(255,255,255,0.3);
      background: rgba(255,255,255,0.1);
    }

    /* ── TELEMETRY GRID ── */
    .telemetry-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 8px;
    }
    .metric-card {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: var(--radius);
      padding: 12px 14px;
      display: flex;
      flex-direction: column;
      gap: 4px;
    }
    .metric-label {
      font-family: var(--mono);
      font-size: 10px;
      color: var(--muted);
      letter-spacing: 0.05em;
    }
    .metric-val {
      font-family: var(--mono);
      font-size: 13px;
      font-weight: 700;
      color: var(--heading);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    /* ── PANELS & CARDS ── */
    .panel {
      background: var(--panel);
      border: 1px solid var(--line-strong);
      border-radius: var(--radius-md);
      padding: 16px;
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    .panel-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      border-bottom: 1px solid var(--line);
      padding-bottom: 10px;
    }
    .panel-title {
      font-family: var(--mono);
      font-size: 11px;
      font-weight: 700;
      letter-spacing: 0.06em;
      color: var(--heading);
    }

    /* ── WI-FI SCAN LIST (CYBER CARD STYLE) ── */
    .wifi-grid {
      display: flex;
      flex-direction: column;
      gap: 6px;
      max-height: 260px;
      overflow-y: auto;
      padding-right: 2px;
    }
    .wifi-item {
      background: var(--panel-2);
      border: 1px solid var(--line);
      border-radius: var(--radius);
      padding: 10px 12px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      cursor: pointer;
      transition: border-color 0.15s, background-color 0.15s;
    }
    .wifi-item:hover {
      border-color: var(--line-strong);
      background: rgba(255,255,255,0.03);
    }
    .wifi-item.selected {
      border-color: var(--accent);
      background: rgba(255,255,255,0.06);
    }
    .wifi-ssid {
      font-family: var(--mono);
      font-size: 12px;
      font-weight: 700;
      color: var(--heading);
      margin-bottom: 4px;
    }
    .wifi-meta {
      display: flex;
      gap: 6px;
      align-items: center;
    }
    .tag {
      font-family: var(--mono);
      font-size: 9px;
      padding: 1px 5px;
      border-radius: 3px;
      background: rgba(255,255,255,0.05);
      border: 1px solid var(--line);
      color: var(--muted);
    }
    .wifi-rssi {
      font-family: var(--mono);
      font-size: 10px;
      color: var(--muted);
    }
    .tag.saved {
      border-color: var(--accent);
      color: var(--accent);
      background: rgba(255,255,255,0.08);
    }
    .wifi-actions {
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .btn-forget {
      font-family: var(--mono);
      font-size: 9px;
      padding: 2px 6px;
      color: var(--muted);
      background: transparent;
      border: 1px solid var(--line);
      border-radius: 3px;
      cursor: pointer;
    }
    .btn-forget:hover {
      color: #ff7a7a;
      border-color: rgba(255,122,122,0.5);
    }

    /* ── FORM ELEMENTS ── */
    .form-group {
      display: flex;
      flex-direction: column;
      gap: 6px;
    }
    .form-label {
      font-family: var(--mono);
      font-size: 10px;
      color: var(--muted);
      letter-spacing: 0.04em;
    }
    input {
      background: var(--bg);
      border: 1px solid var(--line-strong);
      border-radius: var(--radius);
      padding: 10px 12px;
      color: var(--heading);
      font-family: var(--mono);
      font-size: 12px;
      outline: none;
      transition: border-color 0.15s;
    }
    input:focus {
      border-color: var(--accent);
    }

    /* ── BUTTONS ── */
    button {
      font-family: var(--mono);
      font-size: 11px;
      font-weight: 700;
      letter-spacing: 0.04em;
      border-radius: var(--radius);
      padding: 10px 14px;
      cursor: pointer;
      border: 1px solid var(--line-strong);
      background: var(--panel-2);
      color: var(--heading);
      transition: opacity 0.15s, border-color 0.15s;
    }
    button:active { opacity: 0.8; }
    .btn-sm {
      padding: 4px 8px;
      font-size: 10px;
    }
    .btn-primary {
      background: var(--accent);
      color: var(--accent-contrast);
      border: none;
      padding: 12px;
      font-size: 12px;
    }
    .btn-subtle {
      background: transparent;
      border: 1px solid var(--line);
      color: var(--muted);
    }
    .btn-subtle:hover {
      border-color: var(--line-strong);
      color: var(--text);
    }
    .btn-danger {
      background: rgba(248,81,73,0.08);
      border: 1px solid rgba(248,81,73,0.25);
      color: #f85149;
    }

    .footer-actions {
      display: flex;
      gap: 8px;
    }
    .empty-state {
      padding: 24px;
      text-align: center;
      font-family: var(--mono);
      font-size: 11px;
      color: var(--muted);
      border: 1px dashed var(--line);
      border-radius: var(--radius);
    }
    .footer {
      text-align: center;
      font-family: var(--mono);
      font-size: 10px;
      color: var(--faint);
      padding: 8px;
    }

    /* ── VERTICAL CENTERING OF CARD STACK ── */
    .wrapper { margin: auto 0; }
    .wifi-arrow { font-family: var(--mono); font-size: 11px; color: var(--muted); }
  </style>
</head>
<body>
  <div class="wrapper">
    <header class="market-header">
      <div class="market-brand">
        <span class="brand-title">ROUTERD</span>
        <span class="brand-badge">EMBEDDED GATEWAY</span>
      </div>
      <div id="status-pill" class="status-pill">OFFLINE</div>
    </header>

    <!-- TELEMETRY MATRIX -->
    <div class="telemetry-grid">
      <div class="metric-card">
        <div class="metric-label">// UPLINK WAN</div>
        <div class="metric-val" id="stat-uplink">MEMINDAI...</div>
      </div>
      <div class="metric-card">
        <div class="metric-label">// WAN IP</div>
        <div class="metric-val" id="stat-ip">--</div>
      </div>
      <div class="metric-card">
        <div class="metric-label">// KLIEN TERHUBUNG</div>
        <div class="metric-val" id="stat-clients">0</div>
      </div>
      <div class="metric-card">
        <div class="metric-label">// FREE SRAM</div>
        <div class="metric-val" id="stat-heap">--</div>
      </div>
    </div>

    <!-- WI-FI SPECTRUM SCANNER -->
    <div class="panel">
      <div class="panel-header">
        <div class="panel-title">// PELACAK SPEKTRUM WI-FI SEKITAR</div>
        <button type="button" class="btn-sm" id="btn-scan" onclick="scanWifi()">[ SCAN SPEKTRUM ]</button>
      </div>

      <div id="wifi-list" class="wifi-grid">
        <div class="empty-state">Tekan tombol [ SCAN SPEKTRUM ] untuk mendeteksi sinyal...</div>
      </div>
    </div>

    <!-- CONNECTION FORM -->
    <form class="panel" action="/save" method="POST" id="connect-form">
      <div class="panel-title">// KONFIGURASI JARINGAN UPLINK</div>
      <input type="hidden" id="chan" name="chan" value="0">

      <div class="form-group">
        <label class="form-label">TARGET SSID</label>
        <input type="text" id="ssid" name="ssid" placeholder="Pilih Wi-Fi dari hasil scan di atas..." required>
      </div>

      <div class="form-group">
        <label class="form-label">PASSWORD</label>
        <input type="password" id="pass" name="pass" placeholder="Password Wi-Fi...">
      </div>

      <button type="submit" class="btn-primary">[ SAMBUNGKAN KE JARINGAN ]</button>
    </form>

    <!-- FOOTER CONTROLS -->
    <div class="footer-actions">
      <form action="/clear" method="POST" style="flex:1;">
        <button type="submit" class="btn-subtle" style="width:100%;">[ HAPUS SEMUA JARINGAN ]</button>
      </form>
      <form action="/reboot" method="POST" style="flex:1;">
        <button type="submit" class="btn-danger" style="width:100%;">[ REBOOT GATEWAY ]</button>
      </form>
    </div>

    <div class="footer">
      ESP32 Xtensa Dual-Core &bull; LwIP NAPT Engine &bull; routerd v2.2
    </div>
  </div>

  <script>
    function updateStatus() {
      fetch('/api/status')
        .then(r => r.json())
        .then(d => {
          const pill = document.getElementById('status-pill');
          if (d.uplink_connected) {
            pill.innerText = 'ONLINE';
            pill.className = 'status-pill online';
            document.getElementById('stat-uplink').innerText = d.uplink_ssid;
          } else {
            pill.innerText = 'STANDALONE';
            pill.className = 'status-pill';
            document.getElementById('stat-uplink').innerText = 'OFFLINE';
          }
          document.getElementById('stat-ip').innerText = d.uplink_ip;
          document.getElementById('stat-clients').innerText = d.ap_clients;
          document.getElementById('stat-heap').innerText = Math.round(d.heap / 1024) + ' KB';
        })
        .catch(() => {});
    }

    function scanWifi() {
      const btn = document.getElementById('btn-scan');
      const list = document.getElementById('wifi-list');
      btn.innerText = '[ MEMINDAI... ]';
      list.innerHTML = '<div class="empty-state">Sedang memindai spektrum 2.4 GHz...</div>';

      fetch('/scan')
        .then(r => r.json())
        .then(data => {
          btn.innerText = '[ SCAN SPEKTRUM ]';
          if (!data || data.length === 0) {
            list.innerHTML = '<div class="empty-state">Tidak ada sinyal Wi-Fi ditemukan. Coba scan ulang.</div>';
            return;
          }
          // SSIDs are attacker-controlled (anyone can broadcast one), so they
          // must never be parsed as HTML. Build nodes and assign textContent.
          const el = (tag, cls, text) => {
            const n = document.createElement(tag);
            if (cls) n.className = cls;
            if (text !== undefined) n.textContent = text;
            return n;
          };
          list.replaceChildren();
          data.forEach(net => {
            const pct = Math.min(100, Math.max(0, 2 * (net.rssi + 100)));
            const item = el('div', 'wifi-item');
            const left = el('div');
            const meta = el('div', 'wifi-meta');
            meta.append(el('span', 'tag', 'CH ' + net.chan),
                        el('span', 'tag', net.secure ? 'WPA2' : 'OPEN'));
            if (net.saved) meta.append(el('span', 'tag saved', 'TERSIMPAN'));
            meta.append(el('span', 'wifi-rssi', net.rssi + ' dBm (' + pct + '%)'));
            left.append(el('div', 'wifi-ssid', net.ssid), meta);

            const actions = el('div', 'wifi-actions');
            if (net.saved) {
              const b = el('button', 'btn-forget', '[ LUPA ]');
              b.type = 'button';
              b.addEventListener('click', e => { e.stopPropagation(); forgetNet(net.ssid); });
              actions.append(b);
            }
            const arrow = el('div', 'wifi-arrow', '\u2192');
            actions.append(arrow);

            item.append(left, actions);
            item.addEventListener('click', () => pickNet(net.ssid, net.chan, item));
            list.append(item);
          });
        })
        .catch(err => {
          btn.innerText = '[ SCAN SPEKTRUM ]';
          list.innerHTML = '<div class="empty-state">Scan gagal. Silakan coba kembali.</div>';
        });
    }

    function pickNet(ssid, chan, elem) {
      document.querySelectorAll('.wifi-item').forEach(el => el.classList.remove('selected'));
      elem.classList.add('selected');
      document.getElementById('ssid').value = ssid;
      document.getElementById('chan').value = chan;
      document.getElementById('pass').focus();
    }

    function forgetNet(ssid) {
      const body = new URLSearchParams({ ssid: ssid });
      fetch('/forget', { method: 'POST', body: body })
        .then(r => r.json())
        .then(d => {
          if (d.removed) {
            scanWifi();
          } else {
            alert('Gagal lupa: ' + ssid);
          }
        })
        .catch(() => alert('Gagal lupa'));
    }

    updateStatus();
    setInterval(updateStatus, 3000);
  </script>
</body>
</html>
)rawliteral";

const char WebDashboardManager::LOGO_SVG[] PROGMEM = R"rawliteral(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="2"><path d="M2 12H9.5"/><rect x="9.5" y="9.5" width="5" height="5"/><path d="M14.5 12H19.6"/><path d="M14.5 12L18.4 7.5"/><path d="M14.5 12L18.4 16.5"/><circle cx="20.6" cy="12" r="1.4" fill="#fff"/><circle cx="19.5" cy="6.2" r="1.4" fill="#fff"/><circle cx="19.5" cy="17.8" r="1.4" fill="#fff"/></svg>)rawliteral";

