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

// Worker base URL — the per-zip PNG lives at SERVER_BASE_URL/weather/{zip}.png.
// Same worker for every device, so we bake it in. Used by both the normal
// fetch path (main.cpp) and the captive-portal verify path (setup_mode.cpp).
inline constexpr const char *SERVER_BASE_URL =
    "https://weather-esp32.leigh-herbert.workers.dev";

// Compiled-in firmware version. Integer, monotonically increasing. Sent to the
// worker's /firmware/check endpoint as the `current` query param so the worker
// can decide whether a newer build is available for this device's channel.
// Displayed on the splash/setup screen as "vN". Bump on every OTA release.
inline constexpr int FIRMWARE_VERSION = 7;

// User button (IO21): wakes the chip from deep sleep via ext0; a long-press
// (≥ BUTTON_HOLD_MS) enters the menu / setup while a brief tap is ignored.
// Shared by the wake path (main.cpp) and setup mode (setup_mode.cpp polls it to
// offer long-press → menu while the captive-portal AP is up).
#define BUTTON_GPIO     GPIO_NUM_21
#define BUTTON_HOLD_MS  1500

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
