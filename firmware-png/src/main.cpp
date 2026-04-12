// "Dumb" PNG-fetching firmware for the LilyGo T5 4.7" S3 E-Paper.
//
// Fetches a pre-rendered 960×540 grayscale PNG from the Cloudflare Worker,
// decodes it with PNGdec, composites a battery + staleness overlay in the
// bottom-right corner, and pushes the result to the e-paper display. Then
// enters deep sleep for 5 minutes and repeats.
//
// Change detection: a simple hash of the PNG bytes is persisted in RTC memory
// across deep sleep cycles. If the hash, battery level, and staleness display
// are all unchanged, the e-paper refresh is skipped (saves power and avoids
// the visible flash of a full refresh).
//
// TODO (future):
//   - Shared library refactor: extract WiFi, battery, PNG display helpers
//     into a common lib/ so firmware-png and src/ don't duplicate code

#include <Arduino.h>
#include <esp_sleep.h>
#include <esp_adc_cal.h>
#include <time.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PNGdec.h>

#include "epd_driver.h"
#include "firasans.h"
#include "wifi_config.h"

// ─── constants ───────────────────────────────────────────────────────────────

#define SLEEP_MINUTES        5
#define SLEEP_US             ((uint64_t)SLEEP_MINUTES * 60 * 1000000ULL)

// Staleness threshold: show "Xm ago" / "Xh Ym ago" if data is older than this.
#define STALE_THRESHOLD_MIN  30

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

RTC_DATA_ATTR static uint32_t prev_png_hash  = 0;
RTC_DATA_ATTR static int      prev_battery   = -1;
RTC_DATA_ATTR static bool     prev_was_stale = false;
RTC_DATA_ATTR static uint32_t boot_count     = 0;

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

static bool connectWiFi() {
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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

    // NTP sync — needed for staleness calculation.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();

    return true;
}

static void disconnectWiFi() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
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

static bool fetchPng() {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, WEATHER_PNG_URL);
    http.setTimeout(15000);
    http.setConnectTimeout(10000);

    const char *headerKeys[] = {"X-Updated"};
    http.collectHeaders(headerKeys, 1);

    Serial.printf("GET %s\n", WEATHER_PNG_URL);
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

// ─── deep sleep ──────────────────────────────────────────────────────────────

static void enterDeepSleep() {
    Serial.printf("Sleeping for %d minutes...\n", SLEEP_MINUTES);
    Serial.flush();
    esp_sleep_enable_timer_wakeup(SLEEP_US);
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

    Serial.printf("\n=== firmware-png  boot #%u  wakeup=%d ===\n",
                  boot_count, (int)wakeup);

    // Init display + framebuffer.
    epd_init();
    framebuffer = (uint8_t *)ps_calloc(1, EPD_WIDTH * EPD_HEIGHT / 2);
    if (!framebuffer) {
        Serial.println("FATAL: framebuffer alloc failed");
        enterDeepSleep();
        return;
    }
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    // Calibrate ADC (for battery reading).
    calibrateADC();

    // ── Fetch PNG ────────────────────────────────────────────────────────
    bool fetchOk = false;
    if (connectWiFi()) {
        fetchOk = fetchPng();
        // Keep WiFi on briefly for NTP sync (needed for staleness calc).
        // NTP was initiated in connectWiFi(); give it a moment to resolve.
        if (fetchOk) delay(500);
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

    enterDeepSleep();
}

void loop() {
    // Never reached — deep sleep restarts from setup().
}
