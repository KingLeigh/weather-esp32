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
// across deep sleep cycles. If the hash and the active status code are both
// unchanged, the e-paper refresh is skipped (saves power and avoids the visible
// flash of a full refresh).

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
#include "setup_png.h"
#include "menu_png.h"
#include "render.h"
#include "setup_mode.h"

// ─── constants ───────────────────────────────────────────────────────────────

#define SLEEP_MINUTES        10
// Shorter wake interval after a failed fetch — recover faster from transient
// outages without burning extra battery in the steady-state success case.
#define RETRY_SLEEP_MINUTES  5

// After this many consecutive failed WiFi connects (~3h at RETRY_SLEEP_MINUTES),
// a configured device gives up its now-stale weather and falls back to the
// no-WiFi splash. The streak resets on ANY successful connect, so intermittent
// WiFi never trips it — only a sustained outage does.
#define WIFI_FAIL_SPLASH_THRESHOLD  36
// While on the no-WiFi splash, recheck for WiFi at this slower cadence (vs the
// 5-min retry) — we've given up for now, so just poll occasionally to save power.
#define RECOVERY_SLEEP_MINUTES  30
// Message drawn in the splash's reserved bottom strip when a configured device
// has lost WiFi past the threshold (splash.jsx leaves this strip clear).
#define SPLASH_MSG_NO_WIFI  "WiFi network unavailable"
#define SPLASH_MSG_Y        (EPD_HEIGHT - 40)   // text baseline in the bottom strip

// SERVER_BASE_URL lives in config.h (used by both main + setup_mode).

// WiFi connect timeout — give up if STA association doesn't complete in this
// window, count the wake as a failure, and deep sleep.
#define WIFI_TIMEOUT_MS      20000

// BUTTON_GPIO / BUTTON_HOLD_MS now live in config.h (shared with setup_mode.cpp,
// which polls the button to offer long-press → menu while the AP is up).

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
// Partial-refresh cursor box: on a cursor move the arrow is erased/redrawn
// inside this byte-aligned box instead of repainting the whole 960×540 screen.
// x and width MUST be even so the packed 4bpp sub-buffer (2 px/byte) extracts on
// clean byte boundaries. Sized to contain the arrow + a small margin.
#define MENU_CURSOR_BOX_X    (MENU_CURSOR_X - 4)   // 46 (even)
#define MENU_CURSOR_BOX_W    (MENU_CURSOR_W + 8)   // 42 (even)
#define MENU_CURSOR_BOX_H    (MENU_CURSOR_H + 8)   // 48
// Erase flash for the old cursor box: number of black↔white clear cycles. The
// library default is 4 — the main per-move flicker source. Fewer = less flicker
// but more ghost residue left behind (1 = minimum that still erases). Tunable:
// raise toward 2–4 if residue is too heavy.
#define MENU_CURSOR_CLEAR_CYCLES  1
// Shared idle timeout for every awake on-device screen (menu, factory-reset
// confirm, debug, recent errors). After this much inactivity the screen exits to
// Home. Device Setup is the exception — it keeps its own longer 3-min timeout
// (IDLE_TIMEOUT_MS in setup_mode.cpp) since the user may be on their phone.
#define SCREEN_IDLE_MS  30000

// Menu item indices — order MUST match the rows in menu.jsx.
enum MenuItem {
    MENU_DEVICE_SETUP  = 0,
    MENU_DEBUG         = 1,
    MENU_FACTORY_RESET = 2,
    MENU_EXIT          = 3,
};

// QR overlay placement on the device-setup screen. setup.jsx has no placeholder
// box — the firmware white-fills this region and centers the QR in it — so this
// just needs to sit on the right and be vertically centered in the area below
// the title underline (which is at ~y127 in setup.jsx).
#define QR_AREA_X      660
#define QR_AREA_Y      214   // centers the 240px box in the space below the underline
#define QR_AREA_W      240
#define QR_AREA_H      240
// Version 4 (33×33 modules) at ECC_M holds 46 bytes — fits a WiFi-join string
// like "WIFI:T:nopass;S:WhatsTheWeather-XXXX;;" (~38 bytes) with headroom.
#define QR_VERSION     4
#define QR_ECC         ECC_MEDIUM
#define QR_MODULE_PX   6   // 6 × 33 = 198 px QR data, leaves ~21 px white margin

// OLD threshold: show the OLD code when the server's data (X-Updated) is older
// than this. OLD now means a real, ongoing *server-side* staleness issue (the
// fetch succeeded, so connectivity is fine) — NET/SRV cover the connectivity
// failures separately — so the fuse is longer than when this also stood in for
// bad WiFi.
#define STALE_THRESHOLD_MIN  60

// OTA updates are discovered for free: the worker advertises the latest
// available firmware version on every weather response (X-Firmware-Latest),
// which fetchPng() captures into latestFirmwareAvail. No separate version-check
// request and no once-a-day throttle — see applyOtaUpdate().
//
// If a flash attempt fails, that version is skipped for this many wakes before
// being retried — a transient error must not permanently block updates, so the
// skip is a cooldown, not a permanent ban. At SLEEP_MINUTES=10 this is ~6 hours
// (a newer published version bypasses the cooldown and retries immediately).
#define OTA_FAIL_COOLDOWN_WAKES  36

// Status region: bottom-right corner, reserved by the server layout. The server
// leaves this area empty; we stamp at most one 3-letter status code here.
#define OVERLAY_COLOR_MUTED  0x50  // medium grey (debug-screen divider)

// ─── status codes ────────────────────────────────────────────────────────────
// The device surfaces problems as a single 3-letter code in the status region:
// computeStatus() returns the highest-priority active code and drawStatus()
// stamps it. Nothing is drawn when everything is healthy. This replaces the old
// always-on battery icon + "Xm ago" staleness text.
//
// To add a code: insert an enum value at the right priority (lower value = more
// severe / drawn first), add its 3-letter string to STATUS_CODES at the same
// index, and add a predicate to computeStatus() in the matching slot.
enum StatusCode {
    ST_NONE = -1,
    ST_NET  = 0,   // can't connect to WiFi / network
    ST_SRV,        // WiFi up but couldn't fetch weather (server / HTTP error)
    ST_OLD,        // weather data is stale
    ST_BAT,        // battery low — time to charge (lowest priority: it lingers
                   //   for days, so it must not starve out time-sensitive errors)
    ST_COUNT
};

static const char *STATUS_CODES[ST_COUNT] = {
    "NET",  // ST_NET
    "SRV",  // ST_SRV
    "OLD",  // ST_OLD
    "BAT",  // ST_BAT
};

// Status box: the corner rectangle a code occupies. Used for partial refresh,
// where there's no fresh image to do a full redraw with (e.g. NET — WiFi down),
// so we repaint only this box and leave the retained weather on the panel. x and
// width MUST be even so each row packs to a clean byte boundary in the 4bpp
// framebuffer. The box sits inside the server's reserved (empty) region, so
// clearing it to white always matches the weather image beneath it.
#define STATUS_BOX_X   880   // even — left edge
#define STATUS_BOX_W   80    // even — 880..960 (panel right edge)
#define STATUS_BOX_Y   496
#define STATUS_BOX_H   44    // 496..540 (panel bottom edge)
#define STATUS_TEXT_X  890   // writeln cursor, inset from the box
#define STATUS_TEXT_Y  525   // text baseline
// Erase cycles for the status box partial refresh. Partials chain (so ghosting
// accrues) only across NET<->SRV oscillation with no successful fetch between — a
// plain WiFi flap self-cleans, since any wake that fetches is a full refresh, and
// a stably-offline device holds one code and never repaints. A clearing full
// refresh eventually comes on the next successful fetch, or (future) a
// prolonged-offline splash. So ghosting barely accrues; 2 is a slightly cleaner
// erase than the cursor's 1. Bump it if the corner smears under sustained
// NET<->SRV oscillation; drop to 1 if the flash is too eager.
#define STATUS_CLEAR_CYCLES  2

// BAT trips when the pack voltage falls to BATTERY_LOW_MV and only clears once
// it rises back to BATTERY_OK_MV — i.e. only after an actual recharge. The wide
// gap is hysteresis: it stops BAT flapping on/off as the reading jitters near
// the threshold (the battery only discharges between charges, so once tripped it
// stays tripped until you plug it in).
//
// We work in raw millivolts rather than a percentage: a single-cell LiPo's
// voltage→charge curve is very non-linear (a long flat plateau, then a cliff
// below ~3.5 V), so a linear "%" is meaningless exactly where it matters. The
// reading is taken with WiFi up, so the radio's current draw sags the rail —
// this trip point is deliberately below a resting "charge me" target. These are
// first-pass values; tune them once we have real discharge data from this cell.
#define BATTERY_LOW_MV   3500   // ~3.5 V (loaded) — show BAT at/below this
#define BATTERY_OK_MV    3700   // ~3.7 V — clears the latch after a recharge

// ─── RTC memory — survives deep sleep ────────────────────────────────────────
// RTC_DATA_ATTR places these in RTC slow memory, which is NOT cleared on
// deep-sleep wake. Regular RAM is wiped on every wake.

RTC_DATA_ATTR static uint32_t prev_png_hash       = 0;
RTC_DATA_ATTR static int      prev_status         = ST_NONE;
RTC_DATA_ATTR static bool     battery_low_latched = false;
// No-WiFi splash fallback: consecutive failed connects, and whether the current
// home screen is the splash (vs weather). See WIFI_FAIL_SPLASH_THRESHOLD.
RTC_DATA_ATTR static uint32_t wifi_fail_streak    = 0;
RTC_DATA_ATTR static bool     home_is_splash      = false;
// Recent-errors ring (shown on the debug "Recent Errors" screen), newest first.
// Consecutive identical failures (same kind+detail, no clean wake between)
// coalesce into one entry with a count + start time, so a sustained outage is a
// single tallied line rather than a wall of repeats; distinct failures and new
// incidents get their own chronological entries. The error KINDS are exhaustive
// (see ErrKind) so this screen surfaces more than the on-screen status bar does.
// Survives deep sleep; cleared on a true power-off.
enum ErrKind : uint8_t {
    EK_NET = 0,    // WiFi connect failed
    EK_HTTP,       // server returned non-200 (detail = HTTP status)
    EK_TRANSPORT,  // HTTPClient negative code: DNS / connect / TLS / timeout (detail = code)
    EK_EMPTY,      // no Content-Length / chunked response
    EK_OOM,        // PSRAM alloc for the PNG failed
    EK_TRUNCATED,  // short read — connection dropped mid-download
    EK_DECODE,     // PNG decode failed (detail = PNGdec rc)
    EK_NTP,        // NTP sync timed out (clock / staleness unreliable)
    EK_OTA,        // OTA flash failed (detail = httpUpdate error)
};
struct ErrEntry {
    uint32_t firstEpoch;  // first occurrence of this run (unix time; 0 = clock not synced)
    uint16_t count;       // consecutive occurrences coalesced into this entry
    uint8_t  code;        // ErrKind
    int16_t  detail;      // see ErrKind comments
};
#define ERR_RING_SIZE 10
RTC_DATA_ATTR static ErrEntry err_ring[ERR_RING_SIZE];
RTC_DATA_ATTR static uint8_t  err_head         = 0;  // next write slot (ring head)
RTC_DATA_ATTR static uint8_t  err_count        = 0;  // valid entries (<= SIZE)
RTC_DATA_ATTR static bool     last_wake_failed = false;  // did the previous wake log an error
RTC_DATA_ATTR static uint32_t boot_count          = 0;
// Set after we render the splash for a no-config / offline state, so we don't
// re-flash the same splash on every wake (visible refresh + power cost).
// Cleared whenever we successfully render weather.
RTC_DATA_ATTR static bool     splash_already_drawn = false;
// OTA failure cooldown. After a failed httpUpdate we record the version and the
// boot_count at which it may be retried (set to boot_count + OTA_FAIL_COOLDOWN_
// WAKES). This stops a broken build re-downloading every wake, but auto-retries
// once the window elapses, so a transient error can never permanently block
// updates. A newer advertised version bypasses it; a power-on reset clears it.
RTC_DATA_ATTR static int      ota_failed_version   = 0;
RTC_DATA_ATTR static uint32_t ota_retry_after_boot = 0;

// ─── globals (re-initialized every wake) ─────────────────────────────────────

static uint8_t *framebuffer = nullptr;
static PNG png;
static uint16_t line_rgb565[EPD_WIDTH + 16];

// Filled by fetchPng():
static uint8_t  *pngBuf     = nullptr;
static int32_t   pngLen     = 0;
static char      updatedStr[32] = {0};  // X-Updated header value
static int       lastHttpCode   = 0;    // HTTP status from the last fetchPng()
static int       latestFirmwareAvail = 0;  // X-Firmware-Latest from the weather fetch

// Failure detail captured by the wake flow, consumed by logError() (see ErrKind).
static uint8_t   g_fetchFail   = EK_HTTP; // why the last fetch failed (ErrKind)
static int       g_fetchDetail = 0;       // HTTP / transport code for that failure
static bool      g_ntpSynced   = true;    // did NTP sync this wake (set by connectWiFi)
static int       g_decodeRc    = 0;       // PNGdec return code on a decode failure
static int       g_otaError    = 0;       // httpUpdate error on an OTA failure

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
        g_ntpSynced = true;
        Serial.printf(" OK (%lu ms)  %04d-%02d-%02dT%02d:%02d:%02d\n",
                      millis() - ntpStart,
                      ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                      ti.tm_hour, ti.tm_min, ti.tm_sec);
    } else {
        g_ntpSynced = false;
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

// Stable per-chip ID: the eFuse MAC formatted as lowercase 12-char hex. Shown
// on the debug screen and logged over serial. Written into `buf` (must be >= 13
// bytes).
static void deviceId(char *buf, size_t bufSize) {
    snprintf(buf, bufSize, "%012llx", ESP.getEfuseMac());
}

// ─── OTA firmware update ─────────────────────────────────────────────────────
// Downloads firmware version `latestVersion` from the worker and flashes it into
// the inactive OTA slot. Must be called with WiFi up. On success the chip reboots
// into the new slot and this never returns; returns false on failure so the
// caller can record the bad version and avoid re-downloading it every wake.
//
// Discovery is free: the worker advertises the latest available version on every
// weather response (X-Firmware-Latest, captured by fetchPng), so there's no
// separate version-check request or once-a-day throttle.
static bool applyOtaUpdate(int latestVersion) {
    String url = String(SERVER_BASE_URL) + "/firmware/" + latestVersion + ".bin";
    Serial.printf("OTA: v%d > v%d — downloading %s\n",
                  latestVersion, FIRMWARE_VERSION, url.c_str());

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
            return true;  // not reached
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("OTA: server reported no update.");
            g_otaError = 0;
            return false;
        case HTTP_UPDATE_FAILED:
        default:
            Serial.printf("OTA: update FAILED (%d): %s\n",
                          httpUpdate.getLastError(),
                          httpUpdate.getLastErrorString().c_str());
            g_otaError = httpUpdate.getLastError();
            return false;
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

// Returns the pack voltage in millivolts. Averages several ADC samples because
// a single read is noisy — especially here, where WiFi current spikes bounce the
// rail by tens of mV (≈1% of a cell's usable range per 12 mV).
static int readBatteryMillivolts() {
    // EPD must be powered on for BATT_PIN ADC to read correctly.
    epd_poweron();
    delay(10);
    const int N = 16;
    uint32_t sum = 0;
    for (int i = 0; i < N; i++) sum += analogRead(BATT_PIN);
    epd_poweroff();

    float adc = (float)sum / N;
    // BATT_PIN sits behind a 2:1 divider; convert raw ADC → pack volts → mV.
    float voltage = (adc / 4095.0f) * 2.0f * 3.3f * (vref / 1000.0f);
    return (int)(voltage * 1000.0f);
}

// Hysteretic low-battery state: trips at BATTERY_LOW_MV, clears at BATTERY_OK_MV.
// Latched in RTC so it survives deep sleep and only flips on a genuine
// charge/discharge crossing, not on per-wake measurement jitter.
static bool batteryIsLow(int mv) {
    if (mv <= BATTERY_LOW_MV)     battery_low_latched = true;
    else if (mv >= BATTERY_OK_MV) battery_low_latched = false;
    return battery_low_latched;
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

// ─── status: highest-priority code drawn into framebuffer ────────────────────
// Drawn after PNG decode, in the bottom-right ~200×44 px reserved region.

// Returns the highest-priority active status code, or ST_NONE if healthy.
// Predicates are evaluated most-severe-first; the first match wins, so only the
// single worst problem is ever surfaced.
static int computeStatus(bool net_failed, bool srv_failed, int age_min, bool battery_low) {
    if (net_failed)                    return ST_NET;
    if (srv_failed)                    return ST_SRV;
    if (age_min > STALE_THRESHOLD_MIN) return ST_OLD;
    if (battery_low)                   return ST_BAT;
    return ST_NONE;
}

// Record a wake's outcome into the recent-errors ring. Consecutive identical
// failures coalesce into the most-recent entry (count++, keep first-seen time);
// a different failure — or a success — starts a fresh incident. Called once per
// normal-weather wake (not from the debug screen's own test fetch). Only the
// connectivity failures are logged (NET / SRV); OLD/BAT are states, not errors.
// Set by logError() during a wake; persisted into last_wake_failed before sleep
// so the next wake knows whether the previous one failed (for coalescing). RAM,
// so it resets to false on every wake.
static bool wakeHadError = false;

// Append a failure to the recent-errors ring (newest at the head). Consecutive
// identical failures — same kind+detail, with no clean wake in between — coalesce
// into the head entry (count++, keep the start time) so a sustained outage is one
// tallied line; a different failure or one after a clean wake starts a new entry.
static void logError(uint8_t kind, int16_t detail) {
    time_t now;
    time(&now);
    uint32_t epoch = (now > 1700000000) ? (uint32_t)now : 0;  // 0 if the clock isn't synced

    if (last_wake_failed && err_count > 0) {
        ErrEntry &head = err_ring[(err_head + ERR_RING_SIZE - 1) % ERR_RING_SIZE];
        if (head.code == kind && head.detail == detail) {
            if (head.count < 0xFFFF) head.count++;
            wakeHadError = true;
            return;
        }
    }
    ErrEntry &e = err_ring[err_head];
    e.firstEpoch = epoch;
    e.count      = 1;
    e.code       = kind;
    e.detail     = detail;
    err_head = (err_head + 1) % ERR_RING_SIZE;
    if (err_count < ERR_RING_SIZE) err_count++;
    wakeHadError = true;
}

// Stamps the status code in the reserved corner, or nothing if ST_NONE. The
// server keeps this region empty, so on a full refresh it is already blank.
static void drawStatus(int status) {
    if (status == ST_NONE) return;
    int32_t x = STATUS_TEXT_X;
    int32_t y = STATUS_TEXT_Y;
    writeln((GFXfont *)&FiraSans, (char *)STATUS_CODES[status], &x, &y, framebuffer);
}

// Repaints ONLY the status box, leaving the rest of the panel physically intact.
// Used when there's no fresh image (failed fetch) but the status code changed:
// the last good weather is still held on the e-paper, so a full push would wipe
// it. The framebuffer is white at this point (cleared at wake), so the box ends
// up white + the code — or white alone when the code clears (ST_NONE).
static uint8_t statusSubBuf[(STATUS_BOX_W / 2) * STATUS_BOX_H];

static void partialRefreshStatus(int status) {
    Rect_t box = { STATUS_BOX_X, STATUS_BOX_Y, STATUS_BOX_W, STATUS_BOX_H };

    // Build the box in the framebuffer, then pack it tightly (2 px/byte, stride =
    // width/2) — STATUS_BOX_X/W are even, so each row is a clean memcpy.
    epd_fill_rect(box.x, box.y, box.width, box.height, 0xFF, framebuffer);
    drawStatus(status);  // no-op if ST_NONE → box stays white (code cleared)

    const int32_t stride = box.width / 2;
    for (int32_t row = 0; row < box.height; row++) {
        const uint8_t *src =
            framebuffer + (box.y + row) * (EPD_WIDTH / 2) + box.x / 2;
        memcpy(statusSubBuf + row * stride, src, stride);
    }

    epd_poweron();
    epd_clear_area_cycles(box, STATUS_CLEAR_CYCLES, 50);
    epd_draw_grayscale_image(box, statusSubBuf);
    epd_poweroff();
    Serial.printf("Status corner repainted (partial): %s\n",
                  status == ST_NONE ? "(cleared)" : STATUS_CODES[status]);
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

    const char *headerKeys[] = {"X-Updated", "X-Firmware-Latest"};
    http.collectHeaders(headerKeys, 2);

    Serial.printf("GET %s\n", url);
    int httpCode = http.GET();
    lastHttpCode = httpCode;

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("HTTP error: %d\n", httpCode);
        g_fetchFail = (httpCode > 0) ? EK_HTTP : EK_TRANSPORT;  // real HTTP vs transport error
        g_fetchDetail = httpCode;
        http.end();
        return false;
    }

    // Capture X-Updated header.
    String hdr = http.header("X-Updated");
    strncpy(updatedStr, hdr.c_str(), sizeof(updatedStr) - 1);
    updatedStr[sizeof(updatedStr) - 1] = '\0';
    Serial.printf("X-Updated: %s\n", updatedStr);

    // Capture X-Firmware-Latest — the newest firmware version available to this
    // device. Drives free OTA discovery (see the OTA step in setup()).
    latestFirmwareAvail = http.header("X-Firmware-Latest").toInt();
    Serial.printf("X-Firmware-Latest: %d (running v%d)\n",
                  latestFirmwareAvail, FIRMWARE_VERSION);

    // Read body into PSRAM.
    int32_t contentLen = http.getSize();
    Serial.printf("Content-Length: %d bytes\n", contentLen);
    if (contentLen <= 0) {
        Serial.println("No content or chunked (unsupported)");
        g_fetchFail = EK_EMPTY; g_fetchDetail = 0;
        http.end();
        return false;
    }

    pngBuf = (uint8_t *)ps_malloc(contentLen);
    if (!pngBuf) {
        Serial.println("PSRAM alloc failed");
        g_fetchFail = EK_OOM; g_fetchDetail = 0;
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
        g_fetchFail = EK_TRUNCATED; g_fetchDetail = 0;
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
        g_decodeRc = rc;
        return false;
    }
    Serial.printf("PNG: %dx%d, bpp=%d, type=%d\n",
                  png.getWidth(), png.getHeight(),
                  png.getBpp(), png.getPixelType());

    rc = png.decode(nullptr, 0);
    png.close();
    if (rc != PNG_SUCCESS) {
        Serial.printf("PNG decode failed: %d\n", rc);
        g_decodeRc = rc;
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

// Decode a baked full-screen PNG into the framebuffer, optionally draw the
// WiFi-join QR over the QR area, and push to the panel. Shared by
// renderSplash() (onboarding / offline fallback) and renderSetupScreen()
// (shown while the AP is active).
static void renderBakedScreen(const uint8_t *pngData, uint32_t pngDataLen,
                              const char *label, const char *wifiJoinStr,
                              const char *bottomMsg) {
    int rc = png.openRAM((uint8_t *)pngData, pngDataLen, png_draw_callback);
    if (rc != PNG_SUCCESS) {
        Serial.printf("%s openRAM failed: %d\n", label, rc);
        return;
    }
    Serial.printf("%s: %dx%d, bpp=%d\n",
                  label, png.getWidth(), png.getHeight(), png.getBpp());
    rc = png.decode(nullptr, 0);
    png.close();
    if (rc != PNG_SUCCESS) {
        Serial.printf("%s decode failed: %d\n", label, rc);
        return;
    }

    if (wifiJoinStr) {
        Serial.printf("%s: drawing QR for '%s'\n", label, wifiJoinStr);
        drawQrOverPlaceholder(wifiJoinStr);
    }

    if (bottomMsg && bottomMsg[0]) {
        // Centered in the reserved bottom strip (splash.jsx keeps it clear).
        int32_t bx = 0, by = 0, x1, y1, w, h;
        get_text_bounds((GFXfont *)&FiraSans, bottomMsg, &bx, &by, &x1, &y1, &w, &h, NULL);
        int32_t mx = (EPD_WIDTH - w) / 2;
        int32_t my = SPLASH_MSG_Y;
        writeln((GFXfont *)&FiraSans, (char *)bottomMsg, &mx, &my, framebuffer);
        Serial.printf("%s: bottom message '%s'\n", label, bottomMsg);
    }

    pushDisplay();
}

// Onboarding / offline-fallback splash (no AP active). Optionally draws a
// bottom-strip message (e.g. the no-WiFi reason); never draws a QR.
void renderSplash(const char *bottomMsg) {
    renderBakedScreen(splash_png, splash_png_len, "Splash", nullptr, bottomMsg);
}

// Device-setup screen, shown while the captive-portal AP is up. Draws the
// WiFi-join QR over the reserved QR area (the PNG has no placeholder box).
void renderSetupScreen(const char *wifiJoinStr) {
    renderBakedScreen(setup_png, setup_png_len, "Setup", wifiJoinStr, nullptr);
}

// ─── on-device menu ─────────────────────────────────────────────────────────

// Draws the cursor arrow (right-pointing triangle) for menu row `i` into the
// framebuffer, in the white column left of the item text. No display update.
// Row centre matches menu.jsx.
static void drawCursorIntoFb(int i) {
    int32_t cy = MENU_ROW_Y0 + i * MENU_ROW_DY;
    epd_fill_triangle(MENU_CURSOR_X,                 cy - MENU_CURSOR_H / 2,
                      MENU_CURSOR_X,                 cy + MENU_CURSOR_H / 2,
                      MENU_CURSOR_X + MENU_CURSOR_W, cy,
                      0x00, framebuffer);
}

// Byte-aligned box around the cursor arrow for row `i` (used for partial refresh).
static Rect_t cursorBox(int i) {
    int32_t cy = MENU_ROW_Y0 + i * MENU_ROW_DY;
    Rect_t r = { MENU_CURSOR_BOX_X, cy - MENU_CURSOR_BOX_H / 2,
                 MENU_CURSOR_BOX_W, MENU_CURSOR_BOX_H };
    return r;
}

// Renders the bundled menu PNG with the cursor arrow at `selectedIndex` via a
// full-screen refresh. Used on menu entry and as the periodic ghosting-clearing
// refresh; cursor *moves* use moveCursorPartial() instead.
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

    drawCursorIntoFb(selectedIndex);
    pushDisplay();
}

// Moves the cursor from oldIndex to newIndex with a PARTIAL refresh: erase the
// old arrow's box (a localized white flash) and blit the new arrow's box, rather
// than repainting all 960×540. The framebuffer is kept authoritative so the
// periodic full renderMenu() (which clears accrued ghosting) stays correct.
//
// epd_draw_grayscale_image() wants a tightly-packed 4bpp buffer (row stride =
// width/2, no framebuffer stride), so we extract the new box from the full
// framebuffer first; MENU_CURSOR_BOX_X/W are even, so each row is a clean
// byte-aligned memcpy. The new box is white background before drawing (the only
// dark pixels there were the old arrow, which lives in a different row), so the
// blit needs no clear of its own.
static uint8_t cursorSubBuf[(MENU_CURSOR_BOX_W / 2) * MENU_CURSOR_BOX_H];

static void moveCursorPartial(int oldIndex, int newIndex) {
    Rect_t oldBox = cursorBox(oldIndex);
    Rect_t newBox = cursorBox(newIndex);

    // Keep the framebuffer in sync: clear the old arrow, draw the new one.
    epd_fill_rect(oldBox.x, oldBox.y, oldBox.width, oldBox.height, 0xFF, framebuffer);
    drawCursorIntoFb(newIndex);

    // Extract the new box as a packed sub-buffer (two pixels per byte).
    const int32_t stride = newBox.width / 2;
    for (int32_t row = 0; row < newBox.height; row++) {
        const uint8_t *src =
            framebuffer + (newBox.y + row) * (EPD_WIDTH / 2) + newBox.x / 2;
        memcpy(cursorSubBuf + row * stride, src, stride);
    }

    epd_poweron();
    // Low-cycle erase of the old arrow — fewer black↔white flashes than the
    // default epd_clear_area (4 cycles); the leftover residue is wiped by the
    // periodic full refresh.
    epd_clear_area_cycles(oldBox, MENU_CURSOR_CLEAR_CYCLES, 50);
    epd_draw_grayscale_image(newBox, cursorSubBuf);  // blit new arrow
    epd_poweroff();
}

enum MenuButton { BTN_NONE, BTN_SHORT, BTN_LONG };

// Polls the button once. Returns BTN_NONE if not pressed; otherwise blocks for
// the press duration and classifies SHORT (< BUTTON_HOLD_MS) vs LONG. For a
// LONG press it waits for release so the hold isn't re-read as another event.
//
// TODO (see ROADMAP.md "Long-press fires on threshold"): a long press should
// take effect the instant the hold crosses BUTTON_HOLD_MS — not on release —
// so there's feedback while holding. That means returning BTN_LONG immediately
// (drop the wait-for-release here) and relying on the shared waitForButtonRelease()
// gate to swallow the still-held button so it isn't re-read AND can't bleed into
// the next screen (e.g. auto-confirm the factory reset).
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

// ─── screen framework ────────────────────────────────────────────────────────
// Every awake, interactive screen is described by a Screen: how to paint it and
// what each button gesture does. One driver (runScreen) owns the poll loop —
// debounce, idle timeout, dispatch — so screens never hand-roll their own loop.
// A handler returns a Nav telling the navigator (runUi) where to go next.
//
// To add a screen: write a render() (+ optional onShort/onLong returning a Nav),
// declare a `static const Screen`, and route to it with navGoto() (e.g. from a
// menu item). Idle handling, debounce, and the Home transition come for free.

enum NavKind { NAV_STAY, NAV_HOME, NAV_REBOOT, NAV_GOTO };
struct Screen;  // fully defined below; a Nav can point at one
struct Nav { NavKind kind; const Screen *target; };

static constexpr Nav navStay()   { return { NAV_STAY,   nullptr }; }  // keep waiting
static constexpr Nav navHome()   { return { NAV_HOME,   nullptr }; }  // exit to weather/splash
static constexpr Nav navReboot() { return { NAV_REBOOT, nullptr }; }  // esp_restart()
static constexpr Nav navGoto(const Screen *s) { return { NAV_GOTO, s }; }

struct Screen {
    const char *name;       // for serial logs
    Nav  (*run)();          // optional: self-contained screen that owns its own
                            //   loop (Device Setup — concurrent web + button). If
                            //   set, render/onShort/onLong/onIdle are ignored.
    void (*render)();       // else: paint the screen once on entry
    Nav  (*onShort)();      // short press   (nullptr ⇒ navStay — keep waiting)
    Nav  (*onLong)();       // long press    (nullptr ⇒ navHome)
    Nav  onIdle;            // where to go after idleMs of no input
    uint32_t idleMs;
};

// Config + has-config flag the screens need (the debug live test reads them).
// Set once before runUi(); single-threaded, so plain module statics are fine and
// match the rest of this file. Valid only for the duration of a runUi() call.
static const DeviceConfig *g_uiCfg       = nullptr;
static bool                g_uiHasConfig = false;

// The wake long-press (confirmed at the hold threshold, not on release) may still
// be held when we open the first screen — wait it out so it isn't re-read as an
// in-screen press, then debounce the release.
static void waitForButtonRelease() {
    pinMode(BUTTON_GPIO, INPUT_PULLUP);
    while (digitalRead(BUTTON_GPIO) == LOW) delay(10);
    delay(50);
}

// Runs one screen: render once, then poll until a handler returns a non-STAY Nav
// or the idle timeout fires. Handlers own any repaint they need (e.g. the menu's
// partial cursor move), so STAY just keeps looping without a re-render.
static Nav runScreen(const Screen *s) {
    if (s->run) return s->run();          // self-contained (Device Setup)

    s->render();
    unsigned long start = millis();
    for (;;) {
        MenuButton ev = readButtonEvent();
        if (ev == BTN_SHORT) {
            start = millis();
            Nav n = s->onShort ? s->onShort() : navStay();
            if (n.kind != NAV_STAY) return n;
        } else if (ev == BTN_LONG) {
            start = millis();
            Nav n = s->onLong ? s->onLong() : navHome();
            if (n.kind != NAV_STAY) return n;
        } else if (millis() - start >= s->idleMs) {
            return s->onIdle;
        } else {
            delay(20);
        }
    }
}

// Walks the screen graph from `start` until a screen returns HOME (the caller
// then does the Home transition — weather flow / splash) or REBOOT.
static void runUi(const Screen *start) {
    const Screen *cur = start;
    while (cur) {
        Nav n = runScreen(cur);
        if (n.kind == NAV_GOTO)   { cur = n.target; continue; }
        if (n.kind == NAV_REBOOT) { delay(200); esp_restart(); }  // never returns
        return;  // NAV_HOME
    }
}

// Confirmation screen for the destructive factory reset (render only; the
// SCREEN_CONFIRM_RESET handlers interpret the buttons: long = confirm,
// short = cancel, idle = cancel — both cancels go Home).
static void drawConfirmResetScreen() {
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    int32_t x, y;

    // Title + divider (matches the Device info screen chrome).
    x = 60; y = 64;
    writeln((GFXfont *)&FiraSans, "Factory reset?", &x, &y, framebuffer);
    epd_fill_rect(60, 92, EPD_WIDTH - 120, 3, OVERLAY_COLOR_MUTED, framebuffer);

    x = 60; y = 170;
    writeln((GFXfont *)&FiraSans, "Erases saved WiFi + location.", &x, &y, framebuffer);

    x = 60; y = 320;
    writeln((GFXfont *)&FiraSans, "Long-press = confirm", &x, &y, framebuffer);
    x = 60; y = 386;
    writeln((GFXfont *)&FiraSans, "Short-press = cancel", &x, &y, framebuffer);

    pushDisplay();
}

// ─── debug live-test screen ─────────────────────────────────────────────────
// A single on-device diagnostic screen: actively connects WiFi and fetches the
// weather PNG, reporting each step's result. Drawn entirely with FiraSans (no
// server PNG) so it works precisely when WiFi/server is unreachable. Reuses the
// normal wake's connectWiFi()/fetchPng() so it exercises the real code path.

enum WifiState   { WS_PENDING, WS_OK, WS_FAIL, WS_NA };
enum ServerState { SS_PENDING, SS_OK, SS_HTTPFAIL, SS_NA };

struct DebugInfo {
    char        idStr[13];
    char        timeStr[24];
    const char *ssid;
    WifiState   wifi;
    const char *zip;
    ServerState server;
    int         httpCode;
    int         latestFw;    // X-Firmware-Latest this pass (valid iff server==SS_OK)
    char        dataTime[24];
    char        ageStr[16];
};

// Draws the full debug screen from the current DebugInfo (full refresh). Called
// repeatedly as the live test progresses; not-yet-known fields show placeholders.
static void drawDebugScreen(const DebugInfo &d) {
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    char line[80];
    int32_t x, y;

    // Title + divider (mirrors the menu chrome).
    x = 60; y = 64;
    writeln((GFXfont *)&FiraSans, "Device info", &x, &y, framebuffer);
    epd_fill_rect(60, 92, EPD_WIDTH - 120, 3, OVERLAY_COLOR_MUTED, framebuffer);

    // Device identity.
    x = 60; y = 150;
    snprintf(line, sizeof(line), "Device ID: %s", d.idStr);
    writeln((GFXfont *)&FiraSans, line, &x, &y, framebuffer);

    x = 60; y = 196;
    // Append the OTA status once the server has responded (a successful fetch
    // carries X-Firmware-Latest). Lets you see at a glance whether the device
    // thinks it's current — a persistent "(update available)" across debug runs
    // means it's discovering updates but not applying them.
    // TODO: OTA diagnostics + manual control (see ROADMAP.md). Surface *why* an
    // update failed — persist + show the httpUpdate error (getLastError /
    // getLastErrorString) and ota_failed_version — and add a way to force-retry
    // past the cooldown. Likely wants a second debug page or a dedicated
    // "Software update" menu item rather than crowding this screen.
    if (d.server == SS_OK) {
        snprintf(line, sizeof(line), "Software version: v%d (%s)", FIRMWARE_VERSION,
                 d.latestFw > FIRMWARE_VERSION ? "update available" : "up to date");
    } else {
        snprintf(line, sizeof(line), "Software version: v%d", FIRMWARE_VERSION);
    }
    writeln((GFXfont *)&FiraSans, line, &x, &y, framebuffer);

    x = 60; y = 242;
    snprintf(line, sizeof(line), "Device time: %s", d.timeStr);
    writeln((GFXfont *)&FiraSans, line, &x, &y, framebuffer);

    // WiFi.
    x = 60; y = 308;
    if (d.wifi == WS_NA) {
        snprintf(line, sizeof(line), "WiFi: (not configured)");
    } else {
        const char *ws;
        switch (d.wifi) {
            case WS_OK:   ws = "connected"; break;
            case WS_FAIL: ws = "failed to connect"; break;
            default:      ws = "connecting..."; break;
        }
        snprintf(line, sizeof(line), "WiFi: %s  (%s)", d.ssid, ws);
    }
    writeln((GFXfont *)&FiraSans, line, &x, &y, framebuffer);

    // Weather.
    x = 60; y = 374;
    snprintf(line, sizeof(line), "Weather location: %s",
             (d.zip && d.zip[0]) ? d.zip : "(none)");
    writeln((GFXfont *)&FiraSans, line, &x, &y, framebuffer);

    x = 60; y = 420;
    char sbuf[32];
    const char *ss;
    switch (d.server) {
        case SS_OK:       ss = "Yes"; break;
        case SS_HTTPFAIL: snprintf(sbuf, sizeof(sbuf), "No (HTTP %d)", d.httpCode);
                          ss = sbuf; break;
        case SS_NA:       ss = "- (no WiFi)"; break;
        default:          ss = "checking..."; break;
    }
    snprintf(line, sizeof(line), "Weather server accessible: %s", ss);
    writeln((GFXfont *)&FiraSans, line, &x, &y, framebuffer);

    x = 60; y = 466;
    if (d.dataTime[0])
        snprintf(line, sizeof(line), "Weather data: %s  (%s)", d.dataTime, d.ageStr);
    else
        snprintf(line, sizeof(line), "Weather data: -");
    writeln((GFXfont *)&FiraSans, line, &x, &y, framebuffer);

    pushDisplay();
}

// Captures the device's current local time into buf ("(no time)" if unsynced).
static void currentTimeStr(char *buf, size_t bufSize) {
    struct tm ti;
    if (getLocalTime(&ti, 0))
        strftime(buf, bufSize, "%Y-%m-%d %H:%M:%S", &ti);
    else
        snprintf(buf, bufSize, "(no time)");
}

// Runs one live-test pass: draw the static screen immediately (WiFi/server show
// "connecting..."/"checking..." so the user sees the test started), then run
// the WiFi connect AND the weather fetch, and redraw ONCE when both results are
// ready. Leaves WiFi disconnected. The fetched PNG is freed here (not
// displayed) — the post-menu fall-through re-fetches + repaints fresh weather
// on menu exit.
static void runDebugPass(const DeviceConfig &cfg, bool hasConfig) {
    DebugInfo d;
    memset(&d, 0, sizeof(d));
    deviceId(d.idStr, sizeof(d.idStr));
    d.ssid = cfg.ssid.c_str();
    d.zip  = cfg.zip.c_str();
    snprintf(d.timeStr, sizeof(d.timeStr), "...");

    if (!hasConfig) {
        // Nothing to test — show identity + "not configured" and bail.
        d.wifi   = WS_NA;
        d.server = SS_NA;
        currentTimeStr(d.timeStr, sizeof(d.timeStr));
        drawDebugScreen(d);
        return;
    }

    // First draw (immediate): static fields + "connecting..."/"checking..."
    // placeholders, so the user gets instant feedback that the test started.
    d.wifi   = WS_PENDING;
    d.server = SS_PENDING;
    drawDebugScreen(d);

    // Run both tests — WiFi connect, then the weather fetch — before redrawing.
    // The screen updates once, when both results are ready.
    bool wifiOk = connectWiFi(cfg.ssid.c_str(), cfg.password.c_str());
    currentTimeStr(d.timeStr, sizeof(d.timeStr));
    if (wifiOk) {
        d.wifi = WS_OK;

        String url = String(SERVER_BASE_URL) + "/weather/" + cfg.zip + ".png";
        bool fetchOk = fetchPng(url.c_str());
        if (fetchOk) {
            d.server = SS_OK;
            d.latestFw = latestFirmwareAvail;
            strncpy(d.dataTime, updatedStr, sizeof(d.dataTime) - 1);
            int ageMin = getAgeMinutes(updatedStr);
            if (ageMin < 0)        snprintf(d.ageStr, sizeof(d.ageStr), "age unknown");
            else if (ageMin < 60)  snprintf(d.ageStr, sizeof(d.ageStr), "%dm ago", ageMin);
            else                   snprintf(d.ageStr, sizeof(d.ageStr), "%dh %dm ago",
                                            ageMin / 60, ageMin % 60);
            // Diagnostic only: free the buffer rather than display it. The
            // post-menu fall-through re-fetches + repaints fresh weather on exit.
            if (pngBuf) { free(pngBuf); pngBuf = nullptr; pngLen = 0; }
        } else {
            d.server   = SS_HTTPFAIL;
            d.httpCode = lastHttpCode;
        }
        disconnectWiFi();
    } else {
        d.wifi   = WS_FAIL;
        d.server = SS_NA;
    }

    // Second draw: both results together.
    drawDebugScreen(d);
}

// ─── Recent Errors screen ────────────────────────────────────────────────────

// Concise human text for a logged error code + detail.
static void errorText(uint8_t code, int16_t detail, char *buf, size_t n) {
    switch (code) {
        case EK_NET:       snprintf(buf, n, "WiFi connect failed"); break;
        case EK_HTTP:      snprintf(buf, n, "Server HTTP %d", detail); break;
        case EK_TRANSPORT:
            if (detail == -11)     snprintf(buf, n, "Server timeout");        // READ_TIMEOUT
            else if (detail == -1) snprintf(buf, n, "Server no connection");  // CONNECTION_REFUSED
            else                   snprintf(buf, n, "Server net error (%d)", detail);
            break;
        case EK_EMPTY:     snprintf(buf, n, "Empty/chunked response"); break;
        case EK_OOM:       snprintf(buf, n, "Out of memory (PNG)"); break;
        case EK_TRUNCATED: snprintf(buf, n, "Download truncated"); break;
        case EK_DECODE:    snprintf(buf, n, "Image decode failed (%d)", detail); break;
        case EK_NTP:       snprintf(buf, n, "Clock not synced (NTP)"); break;
        case EK_OTA:       snprintf(buf, n, "Update failed (E%d)", detail); break;
        default:           snprintf(buf, n, "Error %u", code); break;
    }
}

// Relative age of a logged timestamp: "5m ago" / "2h ago" / "3d ago"; "--" if
// the clock wasn't synced (then or now).
static void relTime(uint32_t epoch, char *buf, size_t n) {
    time_t now;
    time(&now);
    if (epoch == 0 || now < 1700000000 || (uint32_t)now < epoch) {
        snprintf(buf, n, "--");
        return;
    }
    uint32_t s = (uint32_t)now - epoch;
    if (s < 3600UL)         snprintf(buf, n, "%lum ago", (unsigned long)(s / 60));
    else if (s < 360000UL)  snprintf(buf, n, "%luh ago", (unsigned long)(s / 3600));  // up to 99h
    else                    snprintf(buf, n, "%lud ago", (unsigned long)(s / 86400));
}

static void drawRecentErrors() {
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    int32_t x = 60, y = 64;
    writeln((GFXfont *)&FiraSans, "Recent Errors", &x, &y, framebuffer);
    epd_fill_rect(60, 92, EPD_WIDTH - 120, 3, OVERLAY_COLOR_MUTED, framebuffer);

    if (err_count == 0) {
        x = 60; y = 150;
        writeln((GFXfont *)&FiraSans, "No recent errors.", &x, &y, framebuffer);
        pushDisplay();
        return;
    }

    char line[80], when[16], what[48];
    const int32_t STEP = 46, BOTTOM = 524;  // ~9 lines fit before the panel edge
    y = 150;
    for (int i = 0; i < err_count && y <= BOTTOM; i++) {  // newest first
        int idx = (err_head + ERR_RING_SIZE - 1 - i) % ERR_RING_SIZE;
        ErrEntry &e = err_ring[idx];
        relTime(e.firstEpoch, when, sizeof(when));
        errorText(e.code, e.detail, what, sizeof(what));
        if (e.count > 1) snprintf(line, sizeof(line), "%s   %s  x%u", when, what, e.count);
        else             snprintf(line, sizeof(line), "%s   %s", when, what);
        x = 60;
        writeln((GFXfont *)&FiraSans, line, &x, &y, framebuffer);
        y += STEP;
    }
    pushDisplay();
}

// ─── screen instances ─────────────────────────────────────────────────────────
// The navigation graph, declared once. Defined in reverse-dependency order so a
// navGoto() target always exists already: Recent Errors → Debug → Confirm →
// Setup → Menu. The Menu is the single hub; every sub-screen exits Home (never
// back to the Menu), so there's no back-stack to reason about.

// — Recent Errors — any press → Home, idle → Home. (drawRecentErrors is above.)
static Nav errorsDismiss() { return navHome(); }
static const Screen SCREEN_RECENT_ERRORS = {
    "recent-errors", nullptr, drawRecentErrors,
    errorsDismiss, errorsDismiss, navHome(), SCREEN_IDLE_MS
};

// — Debug live test — runs the WiFi/server test on entry; any press advances to
// Recent Errors; idle → Home. (runDebugPass is above.)
static void debugRender()  { runDebugPass(*g_uiCfg, g_uiHasConfig); }
static Nav  debugAdvance() { return navGoto(&SCREEN_RECENT_ERRORS); }
static const Screen SCREEN_DEBUG = {
    "debug", nullptr, debugRender,
    debugAdvance, debugAdvance, navHome(), SCREEN_IDLE_MS
};

// — Factory-reset confirm — long = wipe + reboot; short/idle = cancel → Home.
static Nav confirmReset() {
    Serial.println("Factory reset confirmed — clearing config.");
    clearConfig();
    splash_already_drawn = false;  // show the onboarding splash after reboot
    return navReboot();
}
static Nav confirmCancel() {
    Serial.println("Factory reset dismissed.");
    return navHome();
}
static const Screen SCREEN_CONFIRM_RESET = {
    "confirm-reset", nullptr, drawConfirmResetScreen,
    confirmCancel, confirmReset, navHome(), SCREEN_IDLE_MS
};

// — Device Setup — self-contained: enterSetupMode() owns the AP + concurrent
// web/button loop and its own (longer) 3-min idle timeout, then exits Home.
static Nav setupRun() {
    Serial.println("Entering device setup.");
    enterSetupMode();
    Serial.println("Setup exited — returning home.");
    return navHome();
}
static const Screen SCREEN_SETUP = {
    "setup", setupRun, nullptr, nullptr, nullptr, navHome(), 0
};

// — Menu (hub) — short cycles the cursor (partial refresh; full on wrap-to-top),
// long selects, idle → Home.
static int  g_menuCursor = 0;
static void menuRender() { renderMenu(g_menuCursor); }
static Nav  menuOnShort() {
    int prev = g_menuCursor;
    g_menuCursor = (g_menuCursor + 1) % MENU_ITEM_COUNT;
    Serial.printf("Menu: cursor -> item %d\n", g_menuCursor);
    // Full refresh on wrap back to the top (once per cycle for a 4-item menu — a
    // natural moment for the flash); partial refresh for the snappy moves between.
    if (g_menuCursor == 0) renderMenu(g_menuCursor);
    else                   moveCursorPartial(prev, g_menuCursor);
    return navStay();
}
static Nav menuOnLong() {
    Serial.printf("Menu: select item %d\n", g_menuCursor);
    switch (g_menuCursor) {
        case MENU_DEVICE_SETUP:  return navGoto(&SCREEN_SETUP);
        case MENU_DEBUG:         return navGoto(&SCREEN_DEBUG);
        case MENU_FACTORY_RESET: return navGoto(&SCREEN_CONFIRM_RESET);
        case MENU_EXIT:          return navHome();
        default:                 return navStay();  // unimplemented (none today)
    }
}
static const Screen SCREEN_MENU = {
    "menu", nullptr, menuRender,
    menuOnShort, menuOnLong, navHome(), SCREEN_IDLE_MS
};

// Opens the on-device menu (the single UI hub) and runs the screen graph until it
// — or a sub-screen — returns Home; the caller then performs the Home transition.
// cfg/hasConfig are exposed to the screens (the debug live test needs them). The
// wake long-press may still be held, so wait it out first. A confirmed factory
// reset reboots inside runUi() and never returns.
static void enterMenu(const DeviceConfig &cfg, bool hasConfig) {
    Serial.println("=== Entering menu ===");
    g_uiCfg       = &cfg;
    g_uiHasConfig = hasConfig;
    g_menuCursor  = 0;
    waitForButtonRelease();
    runUi(&SCREEN_MENU);
    Serial.println("Menu closed — returning home.");
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
    // serial.
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

    // ── Long-press entry: open the on-device menu ────────────────────────
    // A long press from ANY home screen (weather, onboarding splash, or no-WiFi
    // splash) opens the menu — the single UI hub. Device setup is reached from
    // there (Menu → Device setup), and every sub-screen exits back Home. The menu
    // runs entirely on-device, so it works with no WiFi.
    if (wantMenu) {
        enterMenu(cfg, hasConfig);  // reboots & never returns on factory reset / save
        // Leave the menu (or last screen) on the panel while we fetch — the fresh
        // weather replaces it when ready. Just force a repaint (the fetched PNG may
        // match the hash of the weather shown before the menu).
        prev_png_hash  = 0;
        home_is_splash = false;     // re-decided by the weather flow / splash branch
    }

    // ── No config: onboarding splash, button-only wake ───────────────────
    // Either a normal wake of an un-set-up device, or we just closed the menu on
    // one. Repaint the splash if the menu left other content on screen (wantMenu)
    // or it hasn't been drawn yet; otherwise skip the refresh to save power.
    if (!hasConfig) {
        if (wantMenu || !splash_already_drawn) {
            renderSplash();
        } else {
            Serial.println("Splash already drawn — skipping refresh.");
        }
        splash_already_drawn = true;
        home_is_splash = true;
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

    bool wifiOk  = connectWiFi(cfg.ssid.c_str(), cfg.password.c_str());
    bool fetchOk = false;
    if (wifiOk) {
        fetchOk = fetchPng(pngUrl.c_str());
        // Leave WiFi up: the OTA step runs after the weather is on screen
        // (further down) so the device shows fresh weather before any firmware
        // download/reboot.
    } else {
        Serial.println("WiFi failed");
    }

    // ── Read battery + compute status ────────────────────────────────────
    int  battMv   = readBatteryMillivolts();
    bool battLow  = batteryIsLow(battMv);
    int  ageMin   = getAgeMinutes(updatedStr);
    int  status   = computeStatus(!wifiOk, wifiOk && !fetchOk, ageMin, battLow);

    Serial.printf("Battery: %d mV (low=%d)  Age: %d min  WiFi: %s  Status: %s\n",
                  battMv, battLow, ageMin, wifiOk ? "ok" : "FAIL",
                  status == ST_NONE ? "OK" : STATUS_CODES[status]);

    // ── WiFi-failure streak (drives the no-WiFi splash fallback) ─────────
    // Count consecutive failed connects; reset on any successful connect, so
    // intermittent WiFi never trips the fallback — only a sustained outage.
    wifi_fail_streak = wifiOk ? 0 : (wifi_fail_streak + 1);

    // Record this wake's failure (if any) for the "Recent Errors" debug screen.
    // These are mutually exclusive per wake: no WiFi → NET; WiFi but the fetch
    // failed → the specific fetch reason; otherwise a synced-clock miss → NTP.
    // (Decode and OTA failures are logged at their own points further down.)
    if (!wifiOk) {
        logError(EK_NET, 0);
    } else if (!fetchOk) {
        logError(g_fetchFail, (int16_t)g_fetchDetail);
    } else if (!g_ntpSynced) {
        logError(EK_NTP, 0);
    }

    // ── Change detection ─────────────────────────────────────────────────
    uint32_t newHash = fetchOk ? hashBytes(pngBuf, pngLen) : prev_png_hash;
    bool pngChanged    = (newHash != prev_png_hash);
    bool statusChanged = (status != prev_status);

    Serial.printf("Hash: 0x%08X (prev 0x%08X)  png_changed=%d  status_changed=%d  "
                  "wifi_fail_streak=%u  home=%s\n",
                  newHash, prev_png_hash, pngChanged, statusChanged,
                  (unsigned)wifi_fail_streak, home_is_splash ? "splash" : "weather");

    bool decoded = fetchOk && decodePng();
    if (fetchOk && !decoded) {
        // Got PNG bytes but couldn't render them (corrupt image) — log IMG and
        // fall through to the no-fresh-weather handling below.
        logError(EK_DECODE, (int16_t)g_decodeRc);
    }
    if (decoded) {
        // Fresh weather. Repaint if anything changed, on first boot, or when
        // coming back from the splash (which is currently the home screen).
        if (firstBoot || pngChanged || statusChanged || home_is_splash) {
            drawStatus(status);
            pushDisplay();
        } else {
            Serial.println("No changes — skipping display refresh.");
        }
        prev_status    = status;
        prev_png_hash  = newHash;
        home_is_splash = false;
    } else {
        // No fresh weather this wake (WiFi down, fetch failed, or decode failed).
        bool giveUpWeather = (wifi_fail_streak >= WIFI_FAIL_SPLASH_THRESHOLD);
        if (home_is_splash) {
            // Already on the no-WiFi splash — recheck mode, leave it as-is.
            Serial.println("Still offline — staying on the no-WiFi splash.");
        } else if (firstBoot || giveUpWeather) {
            // Nothing worth preserving (fresh boot, or a sustained outage) and the
            // fetch failed — show the informative no-WiFi splash.
            Serial.printf("No weather to show (firstBoot=%d, fail_streak=%u) — splash.\n",
                          firstBoot, (unsigned)wifi_fail_streak);
            renderSplash(SPLASH_MSG_NO_WIFI);
            home_is_splash = true;
            prev_status    = ST_NONE;
        } else if (statusChanged) {
            // Still have recent weather on screen — keep it, stamp the status code
            // (NET/SRV) in the corner via partial refresh. A failed fetch is
            // always NET or SRV, so partials only chain (and ghosting accrues) on
            // NET<->SRV oscillation; the eventual splash/weather full refresh
            // wipes any residue.
            partialRefreshStatus(status);
            prev_status = status;
        } else {
            Serial.println("Offline; status unchanged — keeping weather as-is.");
        }
    }

    // Free PNG buffer before sleep.
    if (pngBuf) { free(pngBuf); pngBuf = nullptr; pngLen = 0; }

    // ── OTA update (piggybacked on the weather fetch) ────────────────────
    // The worker advertises the latest firmware version on every weather
    // response (X-Firmware-Latest → latestFirmwareAvail). If it's newer than
    // what we're running, flash it now — after the weather is on screen, while
    // WiFi is still up. applyOtaUpdate() reboots into the new slot on success
    // (never returns).
    //
    // A version that just failed to flash is skipped only until its cooldown
    // elapses (boot_count reaches ota_retry_after_boot) — a transient error
    // won't re-download the same build every wake, but it auto-retries after
    // ~OTA_FAIL_COOLDOWN_WAKES wakes so the device can never get stuck. A newer
    // published version (latestFirmwareAvail != ota_failed_version) bypasses the
    // cooldown immediately.
    bool inFailCooldown = (latestFirmwareAvail == ota_failed_version
                           && boot_count < ota_retry_after_boot);
    if (fetchOk && latestFirmwareAvail > FIRMWARE_VERSION && !inFailCooldown) {
        if (!applyOtaUpdate(latestFirmwareAvail)) {
            ota_failed_version   = latestFirmwareAvail;
            ota_retry_after_boot = boot_count + OTA_FAIL_COOLDOWN_WAKES;
            logError(EK_OTA, (int16_t)g_otaError);
            Serial.printf("OTA: v%d failed — cooling down %d wakes (retry at boot #%u)\n",
                          latestFirmwareAvail, OTA_FAIL_COOLDOWN_WAKES,
                          (unsigned)ota_retry_after_boot);
        }
    }
    disconnectWiFi();

    // Sleep cadence: normal on success; a faster retry on a transient failure;
    // but once we've fallen back to the no-WiFi splash, recheck slowly to save
    // battery (we've given up for now — no point retrying every 5 min).
    uint32_t sleepMin = home_is_splash ? RECOVERY_SLEEP_MINUTES
                      : (fetchOk ? SLEEP_MINUTES : RETRY_SLEEP_MINUTES);
    // Remember whether this wake logged any error, so the next wake's logError()
    // can coalesce a continuing failure (vs starting a new incident after a clean wake).
    last_wake_failed = wakeHadError;
    enterDeepSleep(/*armTimer=*/true, sleepMin);
}

void loop() {
    // Never reached — deep sleep restarts from setup().
}
