// Self-serve setup mode: WiFi AP + DNS hijack + captive portal HTML form.
//
// Brings up an open AP named "WhatsTheWeather-XXXX" (last 4 of MAC) and runs
// a tiny HTTP server. The form lets the user pick a WiFi network from a scan,
// enter its password, and a zip code. On submit, the firmware tries the
// connection, fetches a test PNG from the worker, and on success writes the
// values to NVS via saveConfig() and calls esp_restart().
//
// Returns on idle timeout (no activity for IDLE_TIMEOUT_MS) OR when the user
// presses the button (any press) to exit. Either way the caller then performs the
// Home transition (weather / splash). On a successful save the chip restarts and
// never returns from this function. The caller renders the setup screen before
// calling (renderSetupScreen, with the WiFi-join QR).

#pragma once

void enterSetupMode();
