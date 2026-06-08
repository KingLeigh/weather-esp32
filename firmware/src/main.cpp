// Self-serve PNG-fetching firmware for the LilyGo T5 4.7" S3 E-Paper.
//
// Three boot paths, picked from wakeup cause + NVS state:
//   - Long-press IO21 → render splash + enterSetupMode() (AP + captive portal)
//   - No NVS config → render splash, deep sleep until button (no timer wake)
//   - Has NVS config → fetch the per-zip PNG from the worker, display weather
//
// Setup-mode entry from the captive portal (see setup_mode.cpp) writes ssid,
// password, and zip into NVS via saveConfig() and esp_restart()s; the next
// boot lands in the weather flow.
//
// Change detection: a simple hash of the PNG bytes is persisted in RTC memory
// across deep sleep cycles. If the hash, battery level, and staleness display
// are all unchanged, the e-paper refresh is skipped (saves power and avoids
// the visible flash of a full refresh).

#include <Arduino.h>
#include <esp_sleep.h>
#include <esp_adc_cal.h>
#include <driver/rtc_io.h>
#include <time.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <PNGdec.h>
#include <qrcode.h>

#include "epd_driver.h"
#include "firasans.h"

#include "config.h"
#include "splash_png.h"
#include "menu_png.h"
#include "render.h"
#include "setup_mode.h"

// ─── constants ───────────────────────────────────────────────────────────────

#define SLEEP_MINUTES        10
// Shorter wake interval after a failed fetch — recover faster from transient
// outages without burning extra battery in the steady-state success case.
#define RETRY_SLEEP_MINUTES  5

// SERVER_BASE_URL lives in config.h (used by both main + setup_mode).

// WiFi connect timeout — give up if STA association doesn't complete in this
// window, count the wake as a failure, and deep sleep.
#define WIFI_TIMEOUT_MS      20000

// User button — IO21 wakes the chip from deep sleep via ext0. Long-press
// (≥ this duration) is required to enter setup mode, so a stray tap doesn't
// pull the user out of normal operation.
#define BUTTON_GPIO          GPIO_NUM_21
#define BUTTON_HOLD_MS       1500

// ── On-device menu ───────────────────────────────────────────────────────────
// Opened by a long-press wake; navigated by short presses (cycle the cursor),
// items chosen by another long-press. Rendered from the bundled menu PNG
// (menu_png.h); the cursor arrow is drawn on-device in the reserved left column.
// ⚠️ Row geometry MUST match worker/renderer/src/menu.jsx (ROW_Y0/ROW_H/items).
#define MENU_ITEM_COUNT      4
#define MENU_ROW_Y0          195    // y-centre (px) of the first menu row
#define MENU_ROW_DY          90     // row pitch (px)
#define MENU_CURSOR_X        50     // left x (px) of the cursor arrow
#define MENU_CURSOR_W        34     // arrow width (px)
#define MENU_CURSOR_H        40     // arrow height (px)
#define MENU_IDLE_TIMEOUT_MS 30000  // auto-exit the menu after this much inactivity
#define MENU_CONFIRM_TIMEOUT_MS 15000  // factory-reset confirm auto-cancels after this

// Menu item indices — order MUST match the rows in menu.jsx.
enum MenuItem {
    MENU_DEVICE_SETUP  = 0,
    MENU_DEBUG         = 1,
    MENU_FACTORY_RESET = 2,
    MENU_EXIT          = 3,
};

// QR overlay placement on the splash. Coordinates match the placeholder box
// in splash.jsx: the bottom-right column of the layout, vertically centered
// in the 330-px-tall bottom section (210px below the title + icons).
#define QR_AREA_X      660
#define QR_AREA_Y      255
#define QR_AREA_W      240
#define QR_AREA_H      240
// Version 4 (33×33 modules) at ECC_M holds 46 bytes — fits a WiFi-join string
// like "WIFI:T:nopass;S:WhatsTheWeather-XXXX;;" (~38 bytes) with headroom.
#define QR_VERSION     4
#define QR_ECC         ECC_MEDIUM
#define QR_MODULE_PX   6   // 6 × 33 = 198 px QR data, leaves ~21 px white margin

// Staleness threshold: show "Xm ago" / "Xh Ym ago" if data is older than this.
#define STALE_THRESHOLD_MIN  30

// OTA update check cadence. We only poll the worker's /firmware/check endpoint
// once per this interval (not every wake) to keep the battery hit negligible.
// 86400s = once per day. A failed check/update waits a full interval to retry
// (prev_ota_check is stamped before the attempt), avoiding per-wake retry storms.
//
// OTA_TEST_MODE (build flag — env firmware-ota-test) drops the interval to 0 so
// the device checks on EVERY wake, for fast OTA iteration. Don't ship it.
#ifdef OTA_TEST_MODE
#define OTA_CHECK_INTERVAL_S  0
#else
#define OTA_CHECK_INTERVAL_S  86400
#endif

// Battery: skip display refresh if battery changed less than this.
#define BATTERY_TOLERANCE    10

// Overlay region: bottom-right corner, reserved by the server layout.
// The server leaves this area empty; we draw battery + staleness here.
#define OVERLAY_COLOR_FG     0x00  // black
#define OVERLAY_COLOR_MUTED  0x50  // medium grey
#define OVERLAY_COLOR_OUTLINE 0xA0 // light grey (battery outline)

// ─── RTC memory — survives deep sleep ────────────────────────────────────────
// RTC_DATA_ATTR places these in RTC slow memory, which is NOT cleared on
// deep-sleep wake. Regular RAM is wiped on every wake.

RTC_DATA_ATTR static uint32_t prev_png_hash       = 0;
RTC_DATA_ATTR static int      prev_battery        = -1;
RTC_DATA_ATTR static bool     prev_was_stale      = false;
RTC_DATA_ATTR static uint32_t boot_count          = 0;
// Set after we render the splash for a no-config / offline state, so we don't
// re-flash the same splash on every wake (visible refresh + power cost).
// Cleared whenever we successfully render weather.
RTC_DATA_ATTR static bool     splash_already_drawn = false;
// Wall-clock time (epoch seconds) of the last OTA version check. 0 = never
// checked. Throttles the check to OTA_CHECK_INTERVAL_S; see checkForOtaUpdate().
RTC_DATA_ATTR static time_t   prev_ota_check       = 0;

// ─── globals (re-initialized every wake) ─────────────────────────────────────

static uint8_t *framebuffer = nullptr;
static PNG png;
static uint16_t line_rgb565[EPD_WIDTH + 16];

// Filled by fetchPng():
static uint8_t  *pngBuf     = nullptr;
static int32_t   pngLen     = 0;
static char      updatedStr[32] = {0};  // X-Updated header value

// ─── simple hash (djb2) ─────────────────────────────────────────────────────

static uint32_t hashBytes(const uint8_t *data, int32_t len) {
    uint32_t h = 5381;
    for (int32_t i = 0; i < len; i++) {
        h = ((h << 5) + h) ^ data[i];
    }
    return h;
}

// ─── PNG decode callback ─────────────────────────────────────────────────────

static int png_draw_callback(PNGDRAW *pDraw) {
    png.getLineAsRGB565(pDraw, line_rgb565, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);

    const int y = pDraw->y;
    const int w = pDraw->iWidth;

    for (int x = 0; x < w && x < EPD_WIDTH; x++) {
        uint16_t rgb = line_rgb565[x];
        uint8_t r = ((rgb >> 11) & 0x1F) << 3;
        uint8_t g = ((rgb >> 5)  & 0x3F) << 2;
        uint8_t b = ( rgb        & 0x1F) << 3;
        uint8_t luma = (77u * r + 150u * g + 29u * b) >> 8;
        epd_draw_pixel(x, y, luma, framebuffer);
    }
    return 1;
}

// ─── WiFi ────────────────────────────────────────────────────────────────────

static bool connectWiFi(const char *ssid, const char *password) {
    Serial.printf("Connecting to WiFi: %s\n", ssid);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    unsigned long start = millis();
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
        attempts++;
        if (attempts % 10 == 0) {
            Serial.printf("\n  WiFi status: %d\n", WiFi.status());
        }
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("WiFi failed! Status: %d\n", WiFi.status());
        return false;
    }

    Serial.printf("Connected! IP: %s, RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());

    IPAddress ip = WiFi.localIP();
    IPAddress gw = WiFi.gatewayIP();
    IPAddress sn = WiFi.subnetMask();
    WiFi.config(ip, gw, sn, IPAddress(8,8,8,8), IPAddress(1,1,1,1));

    // NTP sync — needed for staleness calculation. configTime() is async;
    // we must wait for it to resolve before disconnecting WiFi, otherwise
    // time() returns a stale value from the last boot and the staleness
    // calculation drifts further behind on each deep sleep cycle.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    // TODO: This timezone must match the Worker's TIMEZONE var in wrangler.toml.
    // If the Worker is reconfigured for a different timezone, update this POSIX
    // TZ string to match, otherwise the staleness calculation will be wrong.
    // See: https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();

    Serial.print("Waiting for NTP sync");
    unsigned long ntpStart = millis();
    struct tm ti;
    while (!getLocalTime(&ti, 0) && millis() - ntpStart < 5000) {
        delay(100);
        Serial.print(".");
    }
    if (getLocalTime(&ti, 0)) {
        Serial.printf(" OK (%lu ms)  %04d-%02d-%02dT%02d:%02d:%02d\n",
                      millis() - ntpStart,
                      ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                      ti.tm_hour, ti.tm_min, ti.tm_sec);
    } else {
        Serial.printf(" TIMEOUT after %lu ms — staleness may be inaccurate\n",
                      millis() - ntpStart);
    }

    return true;
}

static void disconnectWiFi() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

// ─── device identity ─────────────────────────────────────────────────────────

// Stable per-chip ID: the eFuse MAC formatted as lowercase 12-char hex. Used as
// the X-Device-Id header so the worker can route this device to a release
// channel (fast/slow). Written into `buf` (must be >= 13 bytes).
static void deviceId(char *buf, size_t bufSize) {
    snprintf(buf, bufSize, "%012llx", ESP.getEfuseMac());
}

// ─── OTA firmware update ─────────────────────────────────────────────────────
// Asks the worker whether a newer firmware build is available for this device's
// channel, and if so streams it straight into the inactive OTA slot and reboots.
//
// Must be called with WiFi up and NTP synced. Throttled to OTA_CHECK_INTERVAL_S
// via prev_ota_check (stamped before the attempt so a failure waits a full
// interval). On a successful update the chip reboots into the new slot and this
// never returns; otherwise it logs and returns so the normal weather flow runs.
static void checkForOtaUpdate() {
    time_t now;
    if (time(&now) == (time_t)-1 || now <= 0) {
        // No valid wall clock (NTP didn't sync) — can't safely throttle; skip.
        Serial.println("OTA: no valid time, skipping check.");
        return;
    }
    if (prev_ota_check != 0 && (now - prev_ota_check) < OTA_CHECK_INTERVAL_S) {
        Serial.printf("OTA: last check %lds ago (< %ds) — skipping.\n",
                      (long)(now - prev_ota_check), OTA_CHECK_INTERVAL_S);
        return;
    }
    // Stamp before attempting so a failed check/update waits a full interval
    // rather than retrying on every wake.
    prev_ota_check = now;

    char id[13];
    deviceId(id, sizeof(id));

    String checkUrl = String(SERVER_BASE_URL) + "/firmware/check?current="
                      + FIRMWARE_VERSION;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, checkUrl);
    http.setTimeout(15000);
    http.setConnectTimeout(10000);
    http.addHeader("X-Device-Id", id);

    const char *headerKeys[] = {"X-Firmware-Url"};
    http.collectHeaders(headerKeys, 1);

    Serial.printf("OTA: GET %s (X-Device-Id: %s)\n", checkUrl.c_str(), id);
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("OTA: check HTTP error: %d\n", httpCode);
        http.end();
        return;
    }

    String fwUrl = http.header("X-Firmware-Url");
    http.end();

    if (fwUrl.length() == 0) {
        Serial.println("OTA: up to date (no X-Firmware-Url).");
        return;
    }

    String url = String(SERVER_BASE_URL) + fwUrl;
    Serial.printf("OTA: update available, downloading %s\n", url.c_str());

    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    t_httpUpdate_return ret =
        httpUpdate.update(secureClient, url, String(FIRMWARE_VERSION));

    switch (ret) {
        case HTTP_UPDATE_OK:
            // The image is flashed into the inactive slot and otadata flipped;
            // the chip normally reboots itself. Force it if it didn't.
            Serial.println("OTA: update OK — rebooting into new firmware.");
            Serial.flush();
            ESP.restart();
            break;
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("OTA: no updates (server reported none).");
            break;
        case HTTP_UPDATE_FAILED:
        default:
            Serial.printf("OTA: update FAILED (%d): %s\n",
                          httpUpdate.getLastError(),
                          httpUpdate.getLastErrorString().c_str());
            break;
    }
}

// ─── battery ─────────────────────────────────────────────────────────────────

static uint32_t vref = 1100;

static void calibrateADC() {
    esp_adc_cal_characteristics_t adc_chars;
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
        ADC_UNIT_2, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        vref = adc_chars.vref;
    }
}

static int readBatteryPercent() {
    // EPD must be powered on for BATT_PIN ADC to read correctly.
    epd_poweron();
    delay(10);
    uint16_t v = analogRead(BATT_PIN);
    epd_poweroff();

    float voltage = ((float)v / 4095.0f) * 2.0f * 3.3f * (vref / 1000.0f);
    if (voltage > 4.2f) voltage = 4.2f;

    int pct = (int)((voltage - 3.0f) / 1.2f * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// ─── staleness ───────────────────────────────────────────────────────────────

static time_t parseTimestamp(const char *ts) {
    if (!ts || !ts[0]) return 0;
    int Y, M, D, h, m, s;
    if (sscanf(ts, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return 0;
    struct tm t = {};
    t.tm_year = Y - 1900;
    t.tm_mon  = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = s;
    t.tm_isdst = -1;
    return mktime(&t);
}

// Returns age in minutes, or -1 if we can't determine it.
static int getAgeMinutes(const char *ts) {
    time_t now;
    time(&now);
    time_t dt = parseTimestamp(ts);
    if (dt == 0 || now == 0) return -1;
    int secs = (int)difftime(now, dt);
    return secs < 0 ? 0 : secs / 60;
}

// Writes e.g. "35m" or "1h 23m" into buf. Empty string if fresh (<=threshold).
static void formatAge(int age_min, char *buf, size_t bufSize) {
    buf[0] = '\0';
    if (age_min < 0 || age_min <= STALE_THRESHOLD_MIN) return;
    int h = age_min / 60;
    int m = age_min % 60;
    if (h > 0) snprintf(buf, bufSize, "%dh %dm", h, m);
    else       snprintf(buf, bufSize, "%dm", age_min);
}

// ─── overlay: battery + staleness drawn into framebuffer ─────────────────────
// Drawn after PNG decode, in the bottom-right 200×44 px reserved region.

static void drawBatteryIcon(int32_t x, int32_t y, int pct) {
    int32_t w = 40, h = 20, tip_w = 4;
    epd_draw_rect(x, y, w, h, OVERLAY_COLOR_OUTLINE, framebuffer);
    epd_fill_rect(x + w, y + 6, tip_w, h - 12, OVERLAY_COLOR_OUTLINE, framebuffer);
    int32_t fill_w = (w - 2) * pct / 100;
    if (fill_w > 0) {
        epd_fill_rect(x + 1, y + 1, fill_w, h - 2, OVERLAY_COLOR_MUTED, framebuffer);
    }
}

static void drawOverlay(int battery_pct, const char *age_str) {
    // Battery icon — bottom-right corner.
    int32_t batt_x = EPD_WIDTH - 55;
    int32_t batt_y = EPD_HEIGHT - 35;
    drawBatteryIcon(batt_x, batt_y, battery_pct);

    // Staleness text — to the left of battery, only if stale.
    if (age_str[0] != '\0') {
        int32_t tx = batt_x - 80;
        int32_t ty = EPD_HEIGHT - 15;
        writeln((GFXfont *)&FiraSans, age_str, &tx, &ty, framebuffer);
    }
}

// ─── HTTP fetch ──────────────────────────────────────────────────────────────
// Downloads the PNG into PSRAM and captures the X-Updated header.
// Returns true on success; pngBuf / pngLen / updatedStr are populated.

static bool fetchPng(const char *url) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(15000);
    http.setConnectTimeout(10000);

    const char *headerKeys[] = {"X-Updated"};
    http.collectHeaders(headerKeys, 1);

    Serial.printf("GET %s\n", url);
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("HTTP error: %d\n", httpCode);
        http.end();
        return false;
    }

    // Capture X-Updated header.
    String hdr = http.header("X-Updated");
    strncpy(updatedStr, hdr.c_str(), sizeof(updatedStr) - 1);
    updatedStr[sizeof(updatedStr) - 1] = '\0';
    Serial.printf("X-Updated: %s\n", updatedStr);

    // Read body into PSRAM.
    int32_t contentLen = http.getSize();
    Serial.printf("Content-Length: %d bytes\n", contentLen);
    if (contentLen <= 0) {
        Serial.println("No content or chunked (unsupported)");
        http.end();
        return false;
    }

    pngBuf = (uint8_t *)ps_malloc(contentLen);
    if (!pngBuf) {
        Serial.println("PSRAM alloc failed");
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    int32_t bytesRead = 0;
    unsigned long t0 = millis();
    while (bytesRead < contentLen) {
        int avail = stream->available();
        if (avail > 0) {
            int got = stream->readBytes(pngBuf + bytesRead,
                                        min(avail, (int)(contentLen - bytesRead)));
            bytesRead += got;
        } else if (!stream->connected()) {
            break;
        } else {
            delay(1);
        }
    }
    http.end();
    pngLen = bytesRead;
    Serial.printf("Fetched %d bytes in %lu ms\n", pngLen, millis() - t0);

    if (pngLen != contentLen) {
        Serial.printf("Short read: %d of %d\n", pngLen, contentLen);
        free(pngBuf); pngBuf = nullptr; pngLen = 0;
        return false;
    }
    return true;
}

// ─── decode + display ────────────────────────────────────────────────────────

static bool decodePng() {
    unsigned long t0 = millis();
    int rc = png.openRAM(pngBuf, pngLen, png_draw_callback);
    if (rc != PNG_SUCCESS) {
        Serial.printf("PNG openRAM failed: %d\n", rc);
        return false;
    }
    Serial.printf("PNG: %dx%d, bpp=%d, type=%d\n",
                  png.getWidth(), png.getHeight(),
                  png.getBpp(), png.getPixelType());

    rc = png.decode(nullptr, 0);
    png.close();
    if (rc != PNG_SUCCESS) {
        Serial.printf("PNG decode failed: %d\n", rc);
        return false;
    }
    Serial.printf("Decoded in %lu ms\n", millis() - t0);
    return true;
}

static void pushDisplay() {
    unsigned long t0 = millis();
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();
    Serial.printf("Display pushed in %lu ms\n", millis() - t0);
}

// ─── splash render (bundled PNG, optional QR overlay) ───────────────────────

// Draws a WiFi-join QR code over the splash's QR placeholder area. Erases
// the dashed-border placeholder (white-fills the area), then draws the QR
// modules in black, centered.
static void drawQrOverPlaceholder(const char *text) {
    QRCode qr;
    uint8_t buf[qrcode_getBufferSize(QR_VERSION)];
    qrcode_initText(&qr, buf, QR_VERSION, QR_ECC, text);

    // Wipe the placeholder (including the dashed outline + "QR" text).
    epd_fill_rect(QR_AREA_X, QR_AREA_Y, QR_AREA_W, QR_AREA_H, 0xFF, framebuffer);

    // Center the QR data within the placeholder. Surrounding white space
    // serves as the scanner-required quiet zone.
    int qrPx   = qr.size * QR_MODULE_PX;
    int origX  = QR_AREA_X + (QR_AREA_W - qrPx) / 2;
    int origY  = QR_AREA_Y + (QR_AREA_H - qrPx) / 2;

    for (int y = 0; y < qr.size; y++) {
        for (int x = 0; x < qr.size; x++) {
            if (qrcode_getModule(&qr, x, y)) {
                epd_fill_rect(origX + x * QR_MODULE_PX,
                              origY + y * QR_MODULE_PX,
                              QR_MODULE_PX, QR_MODULE_PX,
                              0x00, framebuffer);
            }
        }
    }
}

void renderSplash(const char *wifiJoinStr) {
    int rc = png.openRAM((uint8_t *)splash_png, splash_png_len, png_draw_callback);
    if (rc != PNG_SUCCESS) {
        Serial.printf("Splash openRAM failed: %d\n", rc);
        return;
    }
    Serial.printf("Splash: %dx%d, bpp=%d\n",
                  png.getWidth(), png.getHeight(), png.getBpp());
    rc = png.decode(nullptr, 0);
    png.close();
    if (rc != PNG_SUCCESS) {
        Serial.printf("Splash decode failed: %d\n", rc);
        return;
    }

    if (wifiJoinStr) {
        Serial.printf("Splash: drawing QR for '%s'\n", wifiJoinStr);
        drawQrOverPlaceholder(wifiJoinStr);
    }

    // Firmware version + chip ID, bottom-left. Lets the owner read the running
    // version and the device's channel ID off-screen. Bottom-left is clear of
    // the QR area (QR_AREA_X=660), so this shows on both the plain splash and
    // the QR setup screen.
    char id[13];
    deviceId(id, sizeof(id));
    char verText[40];
    snprintf(verText, sizeof(verText), "v%d  %s", FIRMWARE_VERSION, id);
    int32_t vx = 10;
    int32_t vy = EPD_HEIGHT - 12;
    writeln((GFXfont *)&FiraSans, verText, &vx, &vy, framebuffer);

    pushDisplay();
}

// ─── on-device menu ─────────────────────────────────────────────────────────

// Renders the bundled menu PNG with the cursor arrow at the selected row.
// Full-screen refresh (increment 1; a partial-refresh cursor is a later step).
static void renderMenu(int selectedIndex) {
    int rc = png.openRAM((uint8_t *)menu_png, menu_png_len, png_draw_callback);
    if (rc != PNG_SUCCESS) {
        Serial.printf("Menu openRAM failed: %d\n", rc);
        return;
    }
    rc = png.decode(nullptr, 0);
    png.close();
    if (rc != PNG_SUCCESS) {
        Serial.printf("Menu decode failed: %d\n", rc);
        return;
    }

    // Cursor arrow (right-pointing triangle) at the selected row's centre, in
    // the white column left of the item text. Row centre matches menu.jsx.
    int32_t cy = MENU_ROW_Y0 + selectedIndex * MENU_ROW_DY;
    epd_fill_triangle(MENU_CURSOR_X,                 cy - MENU_CURSOR_H / 2,
                      MENU_CURSOR_X,                 cy + MENU_CURSOR_H / 2,
                      MENU_CURSOR_X + MENU_CURSOR_W, cy,
                      0x00, framebuffer);

    pushDisplay();
}

// Brief placeholder shown when an unimplemented menu item is selected
// (currently only Debug mode).
static void showComingSoon() {
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    int32_t x = 60;
    int32_t y = EPD_HEIGHT / 2;
    writeln((GFXfont *)&FiraSans, "Coming soon...", &x, &y, framebuffer);
    pushDisplay();
    delay(1500);
}

enum MenuButton { BTN_NONE, BTN_SHORT, BTN_LONG };

// Polls the button once. Returns BTN_NONE if not pressed; otherwise blocks for
// the press duration and classifies SHORT (< BUTTON_HOLD_MS) vs LONG. For a
// LONG press it waits for release so the hold isn't re-read as another event.
static MenuButton readButtonEvent() {
    if (digitalRead(BUTTON_GPIO) == HIGH) return BTN_NONE;  // not pressed
    delay(20);                                              // debounce
    if (digitalRead(BUTTON_GPIO) == HIGH) return BTN_NONE;  // bounce / noise

    unsigned long pressStart = millis();
    while (digitalRead(BUTTON_GPIO) == LOW) {
        if (millis() - pressStart >= BUTTON_HOLD_MS) {
            while (digitalRead(BUTTON_GPIO) == LOW) delay(10);  // wait for release
            return BTN_LONG;
        }
        delay(10);
    }
    return BTN_SHORT;  // released before the long threshold
}

enum ConfirmResult { CONFIRM_YES, CONFIRM_NO, CONFIRM_TIMEOUT };

// Confirmation screen for the destructive factory reset. CONFIRM_YES on a
// long-press, CONFIRM_NO on a short-press (active cancel → back to the menu),
// CONFIRM_TIMEOUT if the user walks away (idle → caller goes home to weather).
static ConfirmResult confirmFactoryReset() {
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    int32_t x = 60, y = 150;
    writeln((GFXfont *)&FiraSans, "Factory reset?", &x, &y, framebuffer);
    x = 60; y = 240;
    writeln((GFXfont *)&FiraSans, "Erases saved WiFi + location.", &x, &y, framebuffer);
    x = 60; y = 360;
    writeln((GFXfont *)&FiraSans, "Long-press = confirm", &x, &y, framebuffer);
    x = 60; y = 420;
    writeln((GFXfont *)&FiraSans, "Short-press = cancel", &x, &y, framebuffer);
    pushDisplay();

    unsigned long start = millis();
    while (millis() - start < MENU_CONFIRM_TIMEOUT_MS) {
        MenuButton ev = readButtonEvent();
        if (ev == BTN_LONG)  return CONFIRM_YES;
        if (ev == BTN_SHORT) return CONFIRM_NO;
        delay(20);
    }
    return CONFIRM_TIMEOUT;  // user walked away
}

// On-device menu. Short press cycles the cursor, long press selects. Returns on
// "Exit menu" or after MENU_IDLE_TIMEOUT_MS of inactivity. Device setup and
// Factory reset are wired; Debug mode is still a placeholder. Assumes the
// framebuffer is already allocated.
static void enterMenuMode() {
    Serial.println("=== Entering menu mode ===");
    pinMode(BUTTON_GPIO, INPUT_PULLUP);

    // The wake long-press may still be held — wait for release so it isn't
    // immediately read as a selection.
    while (digitalRead(BUTTON_GPIO) == LOW) delay(10);
    delay(50);

    int selected = 0;
    renderMenu(selected);
    unsigned long lastActivity = millis();

    while (millis() - lastActivity < MENU_IDLE_TIMEOUT_MS) {
        MenuButton ev = readButtonEvent();
        if (ev == BTN_NONE) {
            delay(20);
            continue;
        }
        lastActivity = millis();
        if (ev == BTN_SHORT) {
            selected = (selected + 1) % MENU_ITEM_COUNT;
            Serial.printf("Menu: cursor -> item %d\n", selected);
            renderMenu(selected);
        } else {  // BTN_LONG — select
            Serial.printf("Menu: select item %d\n", selected);
            switch (selected) {
                case MENU_EXIT:
                    Serial.println("Menu: exit.");
                    return;
                case MENU_DEVICE_SETUP:
                    // Bring up the AP + captive portal. On a successful save this
                    // esp_restart()s (never returns); it returns only on its own
                    // idle timeout — treat that as "go home": exit the menu so the
                    // caller drops back to weather (or the onboarding splash).
                    Serial.println("Menu: entering device setup.");
                    enterSetupMode();
                    Serial.println("Setup idle timeout — exiting menu.");
                    return;
                case MENU_FACTORY_RESET: {
                    ConfirmResult cr = confirmFactoryReset();
                    if (cr == CONFIRM_YES) {
                        Serial.println("Menu: factory reset confirmed — clearing config.");
                        clearConfig();
                        splash_already_drawn = false;  // show onboarding splash after reboot
                        delay(200);
                        esp_restart();  // reboot into a clean, unconfigured state
                    } else if (cr == CONFIRM_TIMEOUT) {
                        Serial.println("Factory-reset confirm timed out — exiting menu.");
                        return;  // idle → go home (weather / splash)
                    }
                    Serial.println("Menu: factory reset canceled.");
                    renderMenu(selected);
                    break;
                }
                default:  // MENU_DEBUG (and any future not-yet-wired item)
                    showComingSoon();
                    renderMenu(selected);
                    break;
            }
            lastActivity = millis();
        }
    }
    Serial.println("Menu: idle timeout — exiting.");
}

// ─── deep sleep ──────────────────────────────────────────────────────────────

// armTimer=false: only the button wakes the chip (used in the no-config state
// where there's nothing useful to retry on a periodic timer). armTimer=true:
// both timer + button (normal weather operation). The caller picks the timer
// interval — typically SLEEP_MINUTES on success, RETRY_SLEEP_MINUTES on a
// failed fetch so we recover from transient WiFi/server blips faster.
static void enterDeepSleep(bool armTimer = true, uint32_t timerMinutes = SLEEP_MINUTES) {
#ifdef KEEP_AWAKE
    // Debug build: skip real deep sleep so USB CDC stays alive across "wakes".
    // Soft-restart after a short delay to simulate the wake cycle quickly.
    // Note: button wake doesn't work in this mode (chip never actually sleeps).
    Serial.println("KEEP_AWAKE: 5s delay, then esp_restart() (USB stays alive).");
    Serial.flush();
    delay(5000);
    esp_restart();
    return;
#endif

    if (armTimer) {
        Serial.printf("Sleeping for %u min (or button on IO%d)...\n",
                      (unsigned)timerMinutes, (int)BUTTON_GPIO);
    } else {
        Serial.printf("Sleeping until button on IO%d (no timer wake)...\n",
                      (int)BUTTON_GPIO);
    }
    Serial.flush();

    // Configure the button GPIO as an RTC input with internal pull-up so ext0
    // reliably detects the LOW transition when the user presses the button.
    rtc_gpio_pullup_en(BUTTON_GPIO);
    rtc_gpio_pulldown_dis(BUTTON_GPIO);

    esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 0);  // wake when button pulled LOW
    if (armTimer) {
        uint64_t us = (uint64_t)timerMinutes * 60ULL * 1000000ULL;
        esp_sleep_enable_timer_wakeup(us);
    }
    esp_deep_sleep_start();
    // Never reaches here.
}

// ─── main ────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(200);

    boot_count++;
    bool firstBoot = (boot_count == 1);
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();

    Serial.printf("\n=== firmware  boot #%u  wakeup=%d ===\n",
                  boot_count, (int)wakeup);

    // Print firmware version + device ID once so the owner can read them over
    // serial (the device ID is also the OTA channel key sent to the worker).
    {
        char id[13];
        deviceId(id, sizeof(id));
        Serial.printf("Firmware v%d  device-id: %s\n", FIRMWARE_VERSION, id);
    }

    // ── Button wake handling ─────────────────────────────────────────────
    // If the button woke us, require a long hold before doing anything — a
    // brief tap is treated as accidental and we go straight back to sleep
    // (no EPD spin-up, no WiFi). A confirmed long press flips wantMenu so the
    // post-config branch opens the on-device menu instead of fetching weather.
    bool wantMenu = false;
    if (wakeup == ESP_SLEEP_WAKEUP_EXT0) {
        Serial.println("Wakeup: button on IO21");
        pinMode(BUTTON_GPIO, INPUT_PULLUP);
        unsigned long pressStart = millis();
        while (millis() - pressStart < BUTTON_HOLD_MS) {
            if (digitalRead(BUTTON_GPIO) == HIGH) {
                Serial.printf("Button released after %lu ms — too brief, ignoring.\n",
                              millis() - pressStart);
                enterDeepSleep();
                return;
            }
            delay(20);
        }
        Serial.println("Long press confirmed (>1.5s) — opening menu.");
        wantMenu = true;
    }

    // Init display + framebuffer.
    epd_init();
    framebuffer = (uint8_t *)ps_calloc(1, EPD_WIDTH * EPD_HEIGHT / 2);
    if (!framebuffer) {
        Serial.println("FATAL: framebuffer alloc failed");
        enterDeepSleep();
        return;
    }
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

#ifdef SPLASH_TEST_MODE
    Serial.println("SPLASH_TEST_MODE: rendering bundled splash, no network.");
    renderSplash();
    enterDeepSleep();
    return;
#endif

    // ── Load config from NVS ─────────────────────────────────────────────
    DeviceConfig cfg;
    bool hasConfig = loadConfig(cfg);
    if (hasConfig) {
        Serial.printf("Config: SSID='%s' zip='%s'\n",
                      cfg.ssid.c_str(), cfg.zip.c_str());
    } else {
        Serial.println("No NVS config — device not yet set up.");
    }

    // ── Menu entry (long-press) ──────────────────────────────────────────
    if (wantMenu) {
        Serial.println("Long-press: opening on-device menu.");

        // Navigable menu: short press cycles the cursor, long press selects.
        // Returns on "Exit menu" or after an idle timeout. Increment 1 wires
        // only Exit; the other items show a "coming soon" placeholder.
        enterMenuMode();

        if (hasConfig) {
            // Return to weather. The menu is currently on screen, so force a
            // repaint on the next fetch (change detection would otherwise skip
            // it when the fetched PNG matches the previously-displayed weather).
            Serial.println("Menu exited — returning to weather.");
            prev_png_hash = 0;
            // fall through to the weather flow ↓
        } else {
            // No config yet — return to the onboarding splash, button-only sleep.
            Serial.println("Menu exited with no config — showing splash.");
            renderSplash();
            splash_already_drawn = true;
            enterDeepSleep(/*armTimer=*/false);
            return;
        }
    } else if (!hasConfig) {
        // No setup requested + no NVS config: show splash, wait for button.
        if (!splash_already_drawn) {
            renderSplash();
            splash_already_drawn = true;
        } else {
            Serial.println("Splash already drawn — skipping refresh.");
        }
        enterDeepSleep(/*armTimer=*/false);
        return;
    }

    // ── Normal weather flow ──────────────────────────────────────────────
    // Reset the splash-drawn flag so a future drop back to no-config (e.g.
    // after factory reset in setup mode) triggers a fresh splash render.
    splash_already_drawn = false;

    // Calibrate ADC (for battery reading).
    calibrateADC();

    // ── Fetch PNG ────────────────────────────────────────────────────────
    String pngUrl = String(SERVER_BASE_URL) + "/weather/" + cfg.zip + ".png";

    bool fetchOk = false;
    if (connectWiFi(cfg.ssid.c_str(), cfg.password.c_str())) {
        fetchOk = fetchPng(pngUrl.c_str());
        // OTA check while WiFi is up + NTP is synced. Throttled internally to
        // once per OTA_CHECK_INTERVAL_S. On a successful update this reboots
        // into the new slot and never returns; otherwise it falls through to
        // the normal weather display + deep sleep.
        checkForOtaUpdate();
        disconnectWiFi();
    } else {
        Serial.println("WiFi failed");
    }

    // ── Read battery + compute staleness ─────────────────────────────────
    int battery = readBatteryPercent();
    int ageMin  = getAgeMinutes(updatedStr);
    bool isStale = (ageMin > STALE_THRESHOLD_MIN);
    char ageStr[16];
    formatAge(ageMin, ageStr, sizeof(ageStr));

    Serial.printf("Battery: %d%%  Age: %d min  Stale: %s\n",
                  battery, ageMin, isStale ? "yes" : "no");

    // ── Change detection ─────────────────────────────────────────────────
    uint32_t newHash = fetchOk ? hashBytes(pngBuf, pngLen) : prev_png_hash;

    bool pngChanged     = (newHash != prev_png_hash);
    bool battChanged    = (abs(battery - prev_battery) > BATTERY_TOLERANCE);
    bool staleChanged   = (isStale != prev_was_stale);
    bool shouldRefresh  = firstBoot || pngChanged || battChanged || staleChanged;

    Serial.printf("Hash: 0x%08X (prev 0x%08X)  changed=%d  batt_changed=%d  stale_changed=%d\n",
                  newHash, prev_png_hash, pngChanged, battChanged, staleChanged);

    if (!shouldRefresh) {
        Serial.println("No changes — skipping display refresh.");
    } else if (!fetchOk) {
        Serial.println("Fetch failed on first boot — screen left blank.");
    } else {
        // Decode PNG into framebuffer.
        if (decodePng()) {
            // Draw battery + staleness overlay on top of the decoded image.
            drawOverlay(battery, ageStr);
            pushDisplay();
        } else {
            Serial.println("PNG decode failed — screen left blank.");
        }
    }

    // ── Persist state for next wake ──────────────────────────────────────
    if (fetchOk) prev_png_hash = newHash;
    prev_battery   = battery;
    prev_was_stale = isStale;

    // Free PNG buffer before sleep.
    if (pngBuf) { free(pngBuf); pngBuf = nullptr; pngLen = 0; }

    // Shorter retry interval if we couldn't fetch — recover from transient
    // WiFi/server outages quickly, without burning extra battery on the
    // steady-state success case.
    enterDeepSleep(/*armTimer=*/true,
                   fetchOk ? SLEEP_MINUTES : RETRY_SLEEP_MINUTES);
}

void loop() {
    // Never reached — deep sleep restarts from setup().
}
