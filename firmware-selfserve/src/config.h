// Persistent device configuration stored in NVS (ESP32 non-volatile storage).
//
// Survives deep sleep, full power cycle, and firmware reflashes. Wiped only by
// explicit clearConfig() call (or NVS partition wipe via esptool).
//
// Populated by setup mode (captive portal); consumed by the normal-operation
// boot path. If loadConfig() returns false, the firmware should drop into the
// splash-displaying idle state and wait for the user to trigger setup.

#pragma once

#include <Arduino.h>

struct DeviceConfig {
    String ssid;
    String password;  // empty for open networks
    String zip;
};

// Loads SSID, password, and zip from NVS. Returns true if both ssid and zip
// are non-empty (password may be empty for open networks). Returns false if
// NVS is empty or the values are incomplete — caller should treat this as
// "device not yet configured."
bool loadConfig(DeviceConfig& cfg);

void saveConfig(const DeviceConfig& cfg);

// Wipes all stored config keys. Used by the captive portal's "factory reset"
// button to scrub WiFi creds before gifting/handing off the device.
void clearConfig();
