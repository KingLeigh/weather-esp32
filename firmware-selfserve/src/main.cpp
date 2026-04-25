// Self-serve PNG-fetching firmware for the LilyGo T5 4.7" S3 E-Paper.
//
// Reads WiFi credentials and a zip code from NVS (populated by setup mode —
// see future setup_mode.cpp), fetches a pre-rendered 960×540 grayscale PNG
// from the Cloudflare Worker for that zip, decodes it with PNGdec, composites
// a battery + staleness overlay, and pushes to the e-paper. Then deep sleeps.
//
// If NVS has no config yet, displays the bundled splash and deep sleeps —
// future setup-mode firmware will trigger the captive portal from this state.
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
#include <PNGdec.h>

#include "epd_driver.h"
#include "firasans.h"

#include "config.h"
#include "splash_png.h"

// ─── constants ───────────────────────────────────────────────────────────────

#define SLEEP_MINUTES        5
#define SLEEP_US             ((uint64_t)SLEEP_MINUTES * 60 * 1000000ULL)

// Worker base URL — the per-zip PNG lives at SERVER_BASE_URL/weather/{zip}.png.
// Same worker for every device, so we bake it in.
static const char *SERVER_BASE_URL = "https://weather-esp32.leigh-herbert.workers.dev";

// WiFi connect timeout — give up if STA association doesn't complete in this
// window, count the wake as a failure, and deep sleep.
#define WIFI_TIMEOUT_MS      20000

// User button — IO21 wakes the chip from deep sleep via ext0. Long-press
// (≥ this duration) is required to enter setup mode, so a stray tap doesn't
// pull the user out of normal operation.
#define BUTTON_GPIO          GPIO_NUM_21
#define BUTTON_HOLD_MS       1500

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

RTC_DATA_ATTR static uint32_t prev_png_hash       = 0;
RTC_DATA_ATTR static int      prev_battery        = -1;
RTC_DATA_ATTR static bool     prev_was_stale      = false;
RTC_DATA_ATTR static uint32_t boot_count          = 0;
// Set after we render the splash for a no-config / offline state, so we don't
// re-flash the same splash on every wake (visible refresh + power cost).
// Cleared whenever we successfully render weather.
RTC_DATA_ATTR static bool     splash_already_drawn = false;

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

// ─── splash render (bundled PNG, no network) ─────────────────────────────────

static void renderSplash() {
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
    pushDisplay();
}

// ─── deep sleep ──────────────────────────────────────────────────────────────

static void enterDeepSleep() {
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

    Serial.printf("Sleeping for %d minutes (or until button press on IO%d)...\n",
                  SLEEP_MINUTES, (int)BUTTON_GPIO);
    Serial.flush();

    // Configure the button GPIO as an RTC input with internal pull-up so ext0
    // reliably detects the LOW transition when the user presses the button.
    rtc_gpio_pullup_en(BUTTON_GPIO);
    rtc_gpio_pulldown_dis(BUTTON_GPIO);

    esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 0);  // wake when button pulled LOW
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

    Serial.printf("\n=== firmware-selfserve  boot #%u  wakeup=%d ===\n",
                  boot_count, (int)wakeup);

    // ── Button wake handling ─────────────────────────────────────────────
    // If the button woke us, require a long hold before doing anything — a
    // brief tap is treated as accidental and we go straight back to sleep
    // (no EPD spin-up, no WiFi). A confirmed long press will (in phase 5)
    // enter setup mode; for now we just log and continue normal flow, with
    // the splash forced to redraw as visual feedback.
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
        Serial.println("Long press confirmed (>1.5s) — would enter SETUP mode (phase 5).");
        // Force splash redraw so the user gets visible feedback their press worked.
        splash_already_drawn = false;
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
    if (!loadConfig(cfg)) {
        Serial.println("No NVS config — device not yet set up.");
        if (!splash_already_drawn) {
            renderSplash();
            splash_already_drawn = true;
        } else {
            Serial.println("Splash already drawn — skipping refresh.");
        }
        enterDeepSleep();
        return;
    }
    Serial.printf("Config: SSID='%s' zip='%s'\n", cfg.ssid.c_str(), cfg.zip.c_str());
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
