// Cross-cpp display render helpers.
//
// Implementation lives in main.cpp where the framebuffer + PNG decoder + EPD
// driver are set up. Other modules (e.g. setup_mode.cpp) include this header
// to render the splash screen.

#pragma once

// Renders the bundled splash PNG to the e-paper (onboarding / offline
// fallback). If wifiJoinStr is non-null, also draws a WiFi-join QR over the
// QR area.
//
// Format expected for wifiJoinStr: "WIFI:T:nopass;S:<ssid>;;" for open
// networks. iOS/Android scan this as a "join WiFi" intent.
void renderSplash(const char *wifiJoinStr = nullptr);

// Renders the dedicated device-setup PNG with the WiFi-join QR over the QR
// area. Shown while the captive-portal AP is active (enterSetupMode()).
void renderSetupScreen(const char *wifiJoinStr);
