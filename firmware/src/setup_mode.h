// Self-serve setup mode: WiFi AP + DNS hijack + captive portal HTML form.
//
// Brings up an open AP named "WhatsTheWeather-XXXX" (last 4 of MAC) and runs
// a tiny HTTP server. The form lets the user pick a WiFi network from a scan,
// enter its password, and a zip code. On submit, the firmware tries the
// connection, fetches a test PNG from the worker, and on success writes the
// values to NVS via saveConfig() and calls esp_restart().
//
// Returns on idle timeout (no activity for IDLE_TIMEOUT_MS) OR when the user
// long-presses the button to open the on-device menu. On a successful save the
// chip restarts and never returns from this function. The caller renders the
// setup screen before calling (renderSetupScreen, with the WiFi-join QR).

#pragma once

// Why enterSetupMode() returned (it never returns on a successful save — the
// chip restarts instead).
enum SetupResult {
    SETUP_TIMEOUT,  // idle timeout / user gave up — caller goes home
    SETUP_MENU,     // user long-pressed → open the on-device menu
};

SetupResult enterSetupMode();
