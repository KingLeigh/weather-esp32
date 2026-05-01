// Self-serve setup mode: WiFi AP + DNS hijack + captive portal HTML form.
//
// Brings up an open AP named "WhatsTheWeather-XXXX" (last 4 of MAC) and runs
// a tiny HTTP server. The form lets the user pick a WiFi network from a scan,
// enter its password, and a zip code. On submit, the firmware tries the
// connection, fetches a test PNG from the worker, and on success writes the
// values to NVS via saveConfig() and calls esp_restart().
//
// Returns ONLY on idle timeout (no HTTP activity for IDLE_TIMEOUT_MS). On
// successful save the chip restarts and never returns from this function.
// On entry, caller is expected to have rendered the splash already (so the
// device shows the "Press and hold middle button" instructions / future QR
// while the AP is broadcasting).

#pragma once

void enterSetupMode();
