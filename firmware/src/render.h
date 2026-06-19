// Cross-cpp display render helpers.
//
// Implementation lives in main.cpp where the framebuffer + PNG decoder + EPD
// driver are set up. Other modules (e.g. setup_mode.cpp) include this header
// to render the splash screen.

#pragma once

// Renders the bundled splash PNG to the e-paper (onboarding / offline
// fallback). If bottomMsg is non-null, draws it centered in the reserved
// bottom strip (e.g. "WiFi network unavailable"). Never draws a QR — the
// device-setup screen (renderSetupScreen) owns the QR now.
void renderSplash(const char *bottomMsg = nullptr);

// Renders the dedicated device-setup PNG with the WiFi-join QR over the QR
// area. Shown while the captive-portal AP is active (enterSetupMode()).
void renderSetupScreen(const char *wifiJoinStr);
