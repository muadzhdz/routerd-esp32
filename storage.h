#pragma once

#include <Preferences.h>

struct RouterConfig {
    String staSsid;
    String staPass;
    int    staChannel;
    String apSsid;
    String apPass;
    bool   configured;
};

class StorageManager {
private:
    Preferences prefs;
    const char* NS = "routerd";

public:
    RouterConfig config;

    void begin() {
        prefs.begin(NS, false); // Read/Write mode

        // Clean out legacy "Can Ngopi" if present from earlier tests
        String savedSsid = prefs.getString("sta_ssid", "");
        if (savedSsid.startsWith("Can Ngopi")) {
            prefs.remove("sta_ssid");
            prefs.remove("sta_pass");
            prefs.remove("sta_chan");
            prefs.putBool("configured", false);
            savedSsid = "";
        }

        config.staSsid    = savedSsid;
        config.staPass    = prefs.getString("sta_pass", "");
        config.staChannel = prefs.getInt("sta_chan", 0);
        config.apSsid     = prefs.getString("ap_ssid", "routerd");
        config.apPass     = prefs.getString("ap_pass", "routerd123");
        config.configured = prefs.getBool("configured", false);

        Serial.println("[NVS] Configuration initialized:");
        if (config.configured && config.staSsid.length() > 0) {
            Serial.printf("      Uplink SSID : '%s' (Channel %d)\n", config.staSsid.c_str(), config.staChannel);
        } else {
            Serial.println("      Uplink SSID : [NONE] (Ready for scan/setup)");
        }
        Serial.printf("      AP SSID     : '%s'\n", config.apSsid.c_str());
    }

    void saveUplink(const String& ssid, const String& pass, int channel = 0) {
        config.staSsid = ssid;
        config.staPass = pass;
        config.staChannel = channel;
        config.configured = true;

        prefs.putString("sta_ssid", ssid);
        prefs.putString("sta_pass", pass);
        prefs.putInt("sta_chan", channel);
        prefs.putBool("configured", true);

        Serial.printf("[NVS] New uplink saved to flash: '%s' (Channel %d)\n", ssid.c_str(), channel);
    }

    void saveAP(const String& ssid, const String& pass) {
        config.apSsid = ssid;
        config.apPass = pass;

        prefs.putString("ap_ssid", ssid);
        prefs.putString("ap_pass", pass);

        Serial.printf("[NVS] AP credentials saved to flash: '%s'\n", ssid.c_str());
    }

    void clearUplink() {
        prefs.remove("sta_ssid");
        prefs.remove("sta_pass");
        prefs.remove("sta_chan");
        prefs.putBool("configured", false);
        config.staSsid = "";
        config.staPass = "";
        config.staChannel = 0;
        config.configured = false;
        Serial.println("[NVS] Uplink wiped. Reverted to standalone discovery mode");
    }

    void reset() {
        prefs.clear();
        Serial.println("[NVS] Storage reset to factory defaults");
    }
};

extern StorageManager storage;
