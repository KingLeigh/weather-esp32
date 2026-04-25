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
    prefs.begin(NS, /*readOnly=*/false);
    prefs.clear();
    prefs.end();
}
