#include "setup_mode.h"
#include "config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <esp_mac.h>
#include <esp_system.h>

// ─── tunables ────────────────────────────────────────────────────────────────

#define IDLE_TIMEOUT_MS  (5UL * 60UL * 1000UL)  // 5 min of no HTTP activity
#define WIFI_CONNECT_MS  20000                  // STA connect timeout in /save
#define DNS_PORT         53
#define HTTP_PORT        80

// ─── module state ────────────────────────────────────────────────────────────

static DNSServer  dns;
static WebServer  http(HTTP_PORT);
static IPAddress  apIp;
static String     apSsid;
static unsigned long lastActivityMs = 0;

// ─── inlined HTML form ───────────────────────────────────────────────────────
// Single-page app: scans WiFi on load, posts to /save on submit. Kept as a raw
// string literal so we don't need a build step to bundle it.

static const char FORM_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Weather Display Setup</title>
<style>
  * { box-sizing: border-box; }
  body { font-family: -apple-system, BlinkMacSystemFont, sans-serif;
         max-width: 420px; margin: 32px auto; padding: 0 20px; color: #222; }
  h1 { margin: 0 0 8px; font-size: 24px; }
  p.lede { color: #666; margin: 0 0 24px; }
  label { display: block; margin: 18px 0 6px; font-weight: 600; font-size: 14px; }
  select, input { width: 100%; padding: 12px; font-size: 16px;
                  border: 1px solid #ccc; border-radius: 6px; }
  button { width: 100%; padding: 14px; margin-top: 24px;
           background: #2563eb; color: white; font-size: 16px; font-weight: 600;
           border: none; border-radius: 6px; cursor: pointer; }
  button:hover { background: #1d4ed8; }
  button:disabled { background: #999; cursor: wait; }
  #status { margin-top: 16px; padding: 12px; border-radius: 6px; display: none; }
  #status.ok { display: block; background: #f0fdf4; color: #15803d; border: 1px solid #86efac; }
  #status.err { display: block; background: #fef2f2; color: #b91c1c; border: 1px solid #fca5a5; }
  .hint { font-size: 13px; color: #888; margin-top: 4px; }
</style>
</head>
<body>
<h1>Weather Display Setup</h1>
<p class="lede">Pick your WiFi network and enter the zip code you want forecasts for.</p>

<form id="f">
  <label for="ssid">WiFi Network</label>
  <select id="ssid" name="ssid" required>
    <option value="">Scanning&hellip;</option>
  </select>

  <label for="password">WiFi Password</label>
  <input type="password" id="password" name="password" autocomplete="current-password">
  <div class="hint">Leave blank for open networks.</div>

  <label for="zip">Zip Code</label>
  <input type="text" id="zip" name="zip" required pattern="[0-9]{5}" maxlength="5"
         inputmode="numeric" placeholder="10010">
  <div class="hint">Must be a zip your device's owner has registered on the server.</div>

  <button type="submit" id="btn">Save &amp; Connect</button>
  <div id="status"></div>
</form>

<script>
  // Populate SSID dropdown from /scan.
  fetch('/scan').then(r => r.json()).then(networks => {
    const sel = document.getElementById('ssid');
    sel.innerHTML = '';
    if (!networks.length) {
      sel.innerHTML = '<option value="">No networks found — refresh page</option>';
      return;
    }
    // Dedup by SSID, keep strongest signal.
    const seen = new Map();
    networks.forEach(n => {
      const prev = seen.get(n.ssid);
      if (!prev || n.rssi > prev.rssi) seen.set(n.ssid, n);
    });
    Array.from(seen.values()).sort((a,b) => b.rssi - a.rssi).forEach(n => {
      const opt = document.createElement('option');
      opt.value = n.ssid;
      opt.textContent = n.ssid + (n.locked ? ' \u{1F512}' : '');
      sel.appendChild(opt);
    });
  }).catch(() => {
    document.getElementById('ssid').innerHTML =
      '<option value="">Scan failed — refresh page</option>';
  });

  document.getElementById('f').addEventListener('submit', async e => {
    e.preventDefault();
    const btn = document.getElementById('btn');
    const status = document.getElementById('status');
    btn.disabled = true;
    btn.textContent = 'Connecting…';
    status.className = '';
    status.textContent = '';

    try {
      const res = await fetch('/save', { method: 'POST', body: new FormData(e.target) });
      const txt = await res.text();
      if (res.ok) {
        status.className = 'ok';
        status.textContent = txt;
      } else {
        status.className = 'err';
        status.textContent = txt;
        btn.disabled = false;
        btn.textContent = 'Try Again';
      }
    } catch (err) {
      status.className = 'err';
      status.textContent = 'Network error — try again.';
      btn.disabled = false;
      btn.textContent = 'Try Again';
    }
  });
</script>
</body>
</html>
)HTML";

// ─── helpers ─────────────────────────────────────────────────────────────────

static String makeApSsid() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char buf[32];
    snprintf(buf, sizeof(buf), "WhatsTheWeather-%02X%02X", mac[4], mac[5]);
    return String(buf);
}

// JSON-escape a string into the destination String (handles ", \, control chars).
static void appendJsonString(String &out, const String &in) {
    out += '"';
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n')        { out += "\\n"; }
        else if (c == '\r')        { out += "\\r"; }
        else if (c == '\t')        { out += "\\t"; }
        else if ((uint8_t)c < 0x20) {
            char hex[8]; snprintf(hex, sizeof(hex), "\\u%04x", (uint8_t)c);
            out += hex;
        } else {
            out += c;
        }
    }
    out += '"';
}

// ─── HTTP handlers ───────────────────────────────────────────────────────────

static void touchActivity() { lastActivityMs = millis(); }

static void handleRoot() {
    touchActivity();
    http.send_P(200, "text/html", FORM_HTML);
}

static void handleScan() {
    touchActivity();
    Serial.println("Setup: scanning WiFi");
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":";
        appendJsonString(json, WiFi.SSID(i));
        json += ",\"rssi\":" + String(WiFi.RSSI(i));
        json += ",\"locked\":";
        json += (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? "true" : "false";
        json += "}";
    }
    json += "]";
    Serial.printf("Setup: scan found %d networks\n", n);
    WiFi.scanDelete();
    http.send(200, "application/json", json);
}

// Try connecting STA to the given network. AP stays up (we're in AP_STA mode).
static bool tryConnect(const char *ssid, const char *password) {
    Serial.printf("Setup: STA connect to '%s'\n", ssid);
    WiFi.begin(ssid, password);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_MS) {
        delay(200);
    }
    bool ok = WiFi.status() == WL_CONNECTED;
    Serial.printf("Setup: STA connect %s (status=%d)\n", ok ? "OK" : "FAILED",
                  WiFi.status());
    return ok;
}

// HEAD/GET the per-zip PNG to verify the zip is registered + reachable.
static bool tryFetchTest(const String &zip) {
    String url = String(SERVER_BASE_URL) + "/weather/" + zip + ".png";
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient h;
    h.begin(client, url);
    h.setTimeout(10000);
    h.setConnectTimeout(10000);
    int code = h.GET();
    h.end();
    Serial.printf("Setup: GET %s -> %d\n", url.c_str(), code);
    return code == 200;
}

static void handleSave() {
    touchActivity();
    String ssid     = http.arg("ssid");
    String password = http.arg("password");
    String zip      = http.arg("zip");

    if (ssid.length() == 0) {
        http.send(400, "text/plain", "Pick a WiFi network.");
        return;
    }
    if (zip.length() != 5) {
        http.send(400, "text/plain", "Zip must be 5 digits.");
        return;
    }

    if (!tryConnect(ssid.c_str(), password.c_str())) {
        WiFi.disconnect(false);  // keep AP up
        http.send(400, "text/plain",
                  "Couldn't connect to that WiFi. Check SSID & password.");
        return;
    }

    if (!tryFetchTest(zip)) {
        WiFi.disconnect(false);
        http.send(400, "text/plain",
                  "Connected to WiFi, but couldn't fetch weather for that zip. "
                  "Check that the zip is registered on the server.");
        return;
    }

    DeviceConfig cfg;
    cfg.ssid     = ssid;
    cfg.password = password;
    cfg.zip      = zip;
    saveConfig(cfg);

    http.send(200, "text/plain",
              "Saved! The device will restart and start showing weather.");
    Serial.println("Setup: config saved, restarting in 1s");
    delay(1000);  // give the response time to flush over WiFi
    esp_restart();
}

// Captive-portal fallback: redirect anything else to the form. iOS/Android
// detect this redirect on their captive-portal probe URLs and pop the form.
static void handleNotFound() {
    touchActivity();
    String url = "http://" + apIp.toString() + "/";
    http.sendHeader("Location", url, true);
    http.send(302, "text/plain", "");
}

// ─── entry point ─────────────────────────────────────────────────────────────

void enterSetupMode() {
    Serial.println("=== Entering setup mode ===");

    // AP_STA so we can run an AP for the captive portal AND attempt STA
    // connections to the user's WiFi during the /save flow.
    WiFi.mode(WIFI_AP_STA);
    apSsid = makeApSsid();
    bool apOk = WiFi.softAP(apSsid.c_str());  // open network, no password
    apIp = WiFi.softAPIP();
    Serial.printf("Setup: AP %s SSID='%s' IP=%s\n",
                  apOk ? "up" : "FAILED", apSsid.c_str(), apIp.toString().c_str());

    // DNS hijack: every name resolves to AP IP. Forces phones to load the
    // captive-portal probe from us, which triggers the auto-popup behavior.
    dns.start(DNS_PORT, "*", apIp);

    http.on("/", handleRoot);
    http.on("/scan", handleScan);
    http.on("/save", HTTP_POST, handleSave);
    http.onNotFound(handleNotFound);
    http.begin();

    lastActivityMs = millis();
    Serial.printf("Setup: idle timeout = %lu ms\n", IDLE_TIMEOUT_MS);

    // Run until idle timeout. handleSave() calls esp_restart() on success and
    // never returns, so reaching the end of this loop means the user gave up.
    while (millis() - lastActivityMs < IDLE_TIMEOUT_MS) {
        dns.processNextRequest();
        http.handleClient();
        delay(10);
    }

    Serial.println("Setup: idle timeout — tearing down AP and returning.");
    http.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}
