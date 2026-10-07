#pragma once

#include <Preferences.h>

#define MAX_NETWORKS 5

struct SavedNetwork {
    String   ssid;
    String   pass;
    uint64_t seq = 0;   // LRU stamp: higher = more recently used
    bool     used = false;
};

class StorageManager {
private:
    Preferences prefs;
    const char* NS = "routerd";
    uint64_t stamp = 0;

    String key(int i, const char* suffix) const {
        return "net" + String(i) + suffix;
    }

    void saveStamp() {
        prefs.putULong64("stamp", stamp);
    }

    void writeSlot(int i) {
        prefs.putString(key(i, "_ssid").c_str(), networks[i].ssid);
        prefs.putString(key(i, "_pass").c_str(), networks[i].pass);
        prefs.putULong64(key(i, "_seq").c_str(), networks[i].seq);
    }

    void eraseSlot(int i) {
        prefs.remove(key(i, "_ssid").c_str());
        prefs.remove(key(i, "_pass").c_str());
        prefs.remove(key(i, "_seq").c_str());
    }

    int indexOf(const String& ssid) const {
        for (int i = 0; i < MAX_NETWORKS; i++)
            if (networks[i].used && networks[i].ssid == ssid) return i;
        return -1;
    }

    int freeSlot() const {
        for (int i = 0; i < MAX_NETWORKS; i++)
            if (!networks[i].used) return i;
        return -1;
    }

    // LRU eviction victim: slot with the oldest (lowest) stamp.
    int oldestSlot() const {
        int best = 0;
        for (int i = 1; i < MAX_NETWORKS; i++)
            if (networks[i].seq < networks[best].seq) best = i;
        return best;
    }

public:
    SavedNetwork networks[MAX_NETWORKS];
    String apSsid;
    String apPass;

    void begin() {
        prefs.begin(NS, false);
        stamp = prefs.getULong64("stamp", 0);

        // Legacy single-slot migration (pre multi-network firmware).
        String legacy = prefs.getString("sta_ssid", "");
        if (legacy.length() > 0) {
            networks[0].ssid = legacy;
            networks[0].pass = prefs.getString("sta_pass", "");
            networks[0].seq = ++stamp;
            networks[0].used = true;
            writeSlot(0);
            prefs.remove("sta_ssid");
            prefs.remove("sta_pass");
            prefs.remove("sta_chan");
            prefs.remove("configured");
        }

        for (int i = 0; i < MAX_NETWORKS; i++) {
            networks[i].ssid = prefs.getString(key(i, "_ssid").c_str(), "");
            networks[i].pass = prefs.getString(key(i, "_pass").c_str(), "");
            networks[i].seq = prefs.getULong64(key(i, "_seq").c_str(), 0);
            networks[i].used = networks[i].ssid.length() > 0;
        }
        saveStamp();

        apSsid = prefs.getString("ap_ssid", "routerd");
        apPass = prefs.getString("ap_pass", "routerd123");

        Serial.println("[NVS] Configuration initialized:");
        Serial.printf("      AP SSID     : '%s'\n", apSsid.c_str());
        Serial.printf("      Saved nets  : %d\n", count());
        for (int i = 0; i < MAX_NETWORKS; i++)
            if (networks[i].used)
                Serial.printf("      net%d        : '%s' (seq %llu)\n",
                    i, networks[i].ssid.c_str(), (unsigned long long)networks[i].seq);
    }

    int count() const {
        int c = 0;
        for (int i = 0; i < MAX_NETWORKS; i++)
            if (networks[i].used) c++;
        return c;
    }

    bool isSaved(const String& ssid) const {
        return indexOf(ssid) >= 0;
    }

    // Insert or update a network. Returns the slot index used.
    int upsert(const String& ssid, const String& pass) {
        int idx = indexOf(ssid);
        if (idx < 0) idx = freeSlot();
        if (idx < 0) idx = oldestSlot();
        networks[idx].ssid = ssid;
        networks[idx].pass = pass;
        networks[idx].seq = ++stamp;
        networks[idx].used = true;
        writeSlot(idx);
        saveStamp();
        Serial.printf("[NVS] upsert '%s' at slot %d (seq %llu)\n",
            ssid.c_str(), idx, (unsigned long long)networks[idx].seq);
        return idx;
    }

    // Touch LRU stamp. Call after a successful association.
    // Flash sectors have a finite erase budget, and an unstable uplink can
    // reassociate hundreds of times a day. If this network already holds
    // the newest stamp, the ordering would not change, so skip the write.
    bool remember(const String& ssid) {
        int idx = indexOf(ssid);
        if (idx < 0) return false;
        if (networks[idx].seq == stamp) return true;
        networks[idx].seq = ++stamp;
        prefs.putULong64(key(idx, "_seq").c_str(), networks[idx].seq);
        saveStamp();
        Serial.printf("[NVS] remember '%s' (seq %llu)\n",
            ssid.c_str(), (unsigned long long)networks[idx].seq);
        return true;
    }

    int find(const String& ssid) const {
        return indexOf(ssid);
    }

    bool forget(const String& ssid) {
        int idx = indexOf(ssid);
        if (idx < 0) return false;
        eraseSlot(idx);
        networks[idx] = SavedNetwork();
        Serial.printf("[NVS] forget '%s'\n", ssid.c_str());
        return true;
    }

    void clearAll() {
        for (int i = 0; i < MAX_NETWORKS; i++) {
            eraseSlot(i);
            networks[i] = SavedNetwork();
        }
        stamp = 0;
        saveStamp();
        Serial.println("[NVS] All uplink networks wiped");
    }

    void saveAP(const String& ssid, const String& pass) {
        apSsid = ssid;
        apPass = pass;
        prefs.putString("ap_ssid", ssid);
        prefs.putString("ap_pass", pass);
        Serial.printf("[NVS] AP credentials saved: '%s'\n", ssid.c_str());
    }
};

extern StorageManager storage;