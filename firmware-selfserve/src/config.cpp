#include "config.h"

#include <Preferences.h>

static const char *NS = "wxconfig";

bool loadConfig(DeviceConfig &cfg) {
    Preferences prefs;
    if (!prefs.begin(NS, /*readOnly=*/true)) {
        // Namespace doesn't exist yet (first boot, fresh NVS).
        return false;
    }
    cfg.ssid     = prefs.getString("ssid", "");
    cfg.password = prefs.getString("password", "");
    cfg.zip      = prefs.getString("zip", "");
    prefs.end();

    return cfg.ssid.length() > 0 && cfg.zip.length() > 0;
}

void saveConfig(const DeviceConfig &cfg) {
    Preferences prefs;
    prefs.begin(NS, /*readOnly=*/false);
    prefs.putString("ssid", cfg.ssid);
    prefs.putString("password", cfg.password);
    prefs.putString("zip", cfg.zip);
    prefs.end();
}

void clearConfig() {
    Preferences prefs;
    if (!prefs.begin(NS, /*readOnly=*/false)) {
        Serial.println("clearConfig: NVS namespace open failed (already empty?)");
        return;
    }
    bool clearedAll = prefs.clear();
    // Belt-and-suspenders: explicit removes in case clear() is a no-op for
    // some NVS edge case. Both ignore "key doesn't exist" silently.
    prefs.remove("ssid");
    prefs.remove("password");
    prefs.remove("zip");
    prefs.end();
    Serial.printf("clearConfig: clear()=%s + explicit removes done\n",
                  clearedAll ? "ok" : "fail");
}
