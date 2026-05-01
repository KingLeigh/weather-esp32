#include "setup_mode.h"
#include "config.h"
#include "render.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <esp_mac.h>
#include <esp_system.h>

// ─── tunables ────────────────────────────────────────────────────────────────

#define IDLE_TIMEOUT_MS  (3UL * 60UL * 1000UL)  // 3 min of no HTTP activity
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
// Two-step single-page app:
//   Step 1: pick SSID, enter password → POST /connect → tries WiFi + fetches
//           the registered location list from the worker.
//   Step 2: pick location from dropdown → POST /save → verifies + writes NVS
//           + restarts the chip.
// Kept as a raw string literal so we don't need a build step to bundle it.

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
  .step-done { color: #15803d; font-size: 14px; margin-bottom: 16px; }
  .reset-row { margin-top: 24px; text-align: center; }
  .link-btn { background: none; color: #888; padding: 6px 12px;
              font-size: 13px; text-decoration: underline;
              border: none; cursor: pointer; width: auto; margin: 0;
              font-weight: 400; }
  .link-btn:hover { color: #b91c1c; background: none; }
</style>
</head>
<body>
<h1>Weather Display Setup</h1>

<form id="step1">
  <p class="lede">First, connect the device to your WiFi.</p>
  <label for="ssid">WiFi Network</label>
  <select id="ssid" name="ssid" required>
    <option value="">Scanning&hellip;</option>
  </select>

  <label for="password">WiFi Password</label>
  <input type="password" id="password" name="password" autocomplete="current-password">
  <div class="hint">Leave blank for open networks.</div>

  <button type="submit" id="connect-btn">Connect</button>

  <div class="reset-row">
    <button type="button" id="reset-btn" class="link-btn">Factory reset</button>
  </div>
</form>

<form id="step2" style="display:none;">
  <div class="step-done" id="connected-msg">&#10003; Connected to WiFi.</div>
  <p class="lede">Now pick the location to display.</p>
  <label for="zip">Location</label>
  <select id="zip" name="zip" required>
    <option value="">Loading&hellip;</option>
  </select>

  <button type="submit" id="save-btn">Save</button>
</form>

<div id="status"></div>

<script>
  let creds = null;       // remember WiFi creds across the two steps
  let stored = { ssid: '', password: '' };  // prefill source — same SSID only

  // Refill the password field if the selected SSID matches the stored one,
  // else clear it. Lets the owner reconnect to their own network without
  // retyping the password, while gift recipients on a different network
  // never see the stored password.
  function maybePrefillPassword() {
    const selected = document.getElementById('ssid').value;
    const pw = document.getElementById('password');
    if (selected && selected === stored.ssid && stored.password) {
      pw.value = stored.password;
    } else {
      pw.value = '';
    }
  }

  // ── Populate SSID dropdown from /scan on page load ──
  const scanP = fetch('/scan').then(r => r.json()).then(networks => {
    const sel = document.getElementById('ssid');
    sel.innerHTML = '';
    if (!networks.length) {
      sel.innerHTML = '<option value="">No networks found — refresh page</option>';
      return;
    }
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

  // ── Fetch stored creds + run initial prefill check after both load ──
  const currentP = fetch('/current').then(r => r.json()).then(c => {
    stored = c;
  }).catch(() => { /* leave stored empty — no prefill */ });

  Promise.all([scanP, currentP]).then(maybePrefillPassword);
  document.getElementById('ssid').addEventListener('change', maybePrefillPassword);

  // ── Factory reset: two-click inline confirmation ──
  // First click arms the button (text changes, 5s timeout). Second click
  // within the window actually fires the reset. Avoids confirm() because the
  // iOS Captive Network Assistant browser silently blocks system dialogs.
  let resetArmed = false;
  let resetTimer = null;
  const resetBtn = document.getElementById('reset-btn');

  resetBtn.addEventListener('click', async () => {
    if (!resetArmed) {
      resetArmed = true;
      resetBtn.textContent = 'Click again to confirm — erases WiFi & location';
      resetBtn.style.color = '#b91c1c';
      resetBtn.style.fontWeight = '600';
      resetTimer = setTimeout(() => {
        resetArmed = false;
        resetBtn.textContent = 'Factory reset';
        resetBtn.style.color = '';
        resetBtn.style.fontWeight = '';
      }, 5000);
      return;
    }

    clearTimeout(resetTimer);
    resetBtn.textContent = 'Resetting…';
    resetBtn.disabled = true;
    try {
      await fetch('/reset', { method: 'POST' });
    } catch (e) { /* chip restarts mid-response — expected */ }
    document.body.innerHTML =
      '<h1>Reset complete</h1>' +
      '<p>The device has restarted with no saved settings. ' +
      'Press the device button to start setup again, then re-scan the QR code.</p>';
  });

  function showError(msg) {
    const status = document.getElementById('status');
    status.className = 'err';
    status.textContent = msg;
  }
  function clearStatus() {
    const status = document.getElementById('status');
    status.className = '';
    status.textContent = '';
  }

  // ── Step 1: connect to WiFi, fetch location list ──
  document.getElementById('step1').addEventListener('submit', async e => {
    e.preventDefault();
    const btn = document.getElementById('connect-btn');
    btn.disabled = true;
    btn.textContent = 'Connecting…';
    clearStatus();

    const formData = new FormData(e.target);
    try {
      const res = await fetch('/connect', { method: 'POST', body: formData });
      const json = await res.json();
      if (!json.ok) {
        showError(json.error || 'Connection failed.');
        btn.disabled = false;
        btn.textContent = 'Try Again';
        return;
      }
      // Remember creds for the /save step.
      creds = { ssid: formData.get('ssid'), password: formData.get('password') };

      // Populate location dropdown.
      const zipSel = document.getElementById('zip');
      zipSel.innerHTML = '';
      if (!json.locations || !json.locations.length) {
        zipSel.innerHTML = '<option value="">No locations registered on server</option>';
      } else {
        json.locations.forEach(loc => {
          const opt = document.createElement('option');
          opt.value = loc.zip;
          opt.textContent = loc.label + ' — ' + loc.zip;
          zipSel.appendChild(opt);
        });
      }

      document.getElementById('step1').style.display = 'none';
      document.getElementById('step2').style.display = '';
    } catch (err) {
      showError('Network error — try again.');
      btn.disabled = false;
      btn.textContent = 'Try Again';
    }
  });

  // ── Step 2: verify zip, write NVS, restart ──
  document.getElementById('step2').addEventListener('submit', async e => {
    e.preventDefault();
    const btn = document.getElementById('save-btn');
    btn.disabled = true;
    btn.textContent = 'Saving…';
    clearStatus();

    const fd = new FormData(e.target);
    fd.set('ssid', creds.ssid);
    fd.set('password', creds.password);

    try {
      const res = await fetch('/save', { method: 'POST', body: fd });
      const txt = await res.text();
      if (res.ok) {
        const status = document.getElementById('status');
        status.className = 'ok';
        status.textContent = txt;
      } else {
        showError(txt);
        btn.disabled = false;
        btn.textContent = 'Try Again';
      }
    } catch (err) {
      showError('Network error — try again.');
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

// Avoid reconnecting if we're already on the requested SSID (the /save call
// usually happens on top of an already-connected /connect).
static bool tryConnectIfNeeded(const char *ssid, const char *password) {
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == String(ssid)) {
        Serial.println("Setup: STA already connected to target SSID");
        return true;
    }
    return tryConnect(ssid, password);
}

// Fetch JSON from a URL into the given String. Returns the HTTP status code,
// or -1 on transport error.
static int httpsGetString(const String &url, String &out) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient h;
    h.begin(client, url);
    h.setTimeout(10000);
    h.setConnectTimeout(10000);
    int code = h.GET();
    if (code == 200) {
        out = h.getString();
    }
    h.end();
    return code;
}

// GET the per-zip PNG to verify the zip is registered + reachable.
static bool tryFetchTest(const String &zip) {
    String url = String(SERVER_BASE_URL) + "/weather/" + zip + ".png";
    String body;
    int code = httpsGetString(url, body);
    Serial.printf("Setup: GET %s -> %d\n", url.c_str(), code);
    return code == 200;
}

// Returns the currently-stored WiFi creds so the form can pre-fill the
// password field IF the user re-selects the same network. Password is sent
// in cleartext over the AP — acceptable trade-off for the small-trust use
// case (gifting to friends/family). Recipient on a different WiFi never
// triggers the prefill since their dropdown won't contain the stored SSID.
static void handleCurrent() {
    touchActivity();
    DeviceConfig cfg;
    bool has = loadConfig(cfg);

    String resp = "{";
    resp += "\"ssid\":";
    appendJsonString(resp, has ? cfg.ssid : String(""));
    resp += ",\"password\":";
    appendJsonString(resp, has ? cfg.password : String(""));
    resp += "}";

    http.send(200, "application/json", resp);
}

// Wipes all NVS config and restarts the chip. Used by the factory-reset
// button before gifting the device.
static void handleReset() {
    touchActivity();
    Serial.println("Setup: factory reset — clearing NVS and restarting");
    clearConfig();
    http.send(200, "text/plain", "Factory reset complete. Restarting...");
    delay(1000);  // let the response flush before the chip goes
    esp_restart();
}

// Step 1 endpoint. Connects STA to the user's WiFi, then fetches the slim
// location list from the worker so the form can populate its zip dropdown.
// Errors distinguish between bad WiFi creds and an unreachable/empty server
// so the user can diagnose what's actually wrong.
static void handleConnect() {
    touchActivity();
    String ssid     = http.arg("ssid");
    String password = http.arg("password");

    if (ssid.length() == 0) {
        http.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"Pick a WiFi network.\"}");
        return;
    }

    if (!tryConnectIfNeeded(ssid.c_str(), password.c_str())) {
        WiFi.disconnect(false);  // keep AP up
        http.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"Couldn't connect to that WiFi. "
                  "Check the network name and password.\"}");
        return;
    }

    String url = String(SERVER_BASE_URL) + "/locations";
    String body;
    int code = httpsGetString(url, body);
    Serial.printf("Setup: GET %s -> %d\n", url.c_str(), code);

    if (code != 200) {
        // WiFi connected but worker is unreachable / down / wrong URL. Leave
        // STA connected so the user can retry without redoing WiFi.
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "{\"ok\":false,\"error\":\"Connected to WiFi, but couldn't reach "
                 "the weather server (HTTP %d). The server may be down — try again "
                 "in a minute.\"}", code);
        http.send(502, "application/json", msg);
        return;
    }

    if (body == "[]") {
        http.send(502, "application/json",
                  "{\"ok\":false,\"error\":\"Reached the weather server, but it has "
                  "no locations registered. Ask the device's owner to add one on "
                  "the admin page.\"}");
        return;
    }

    // Forward the location list verbatim — already JSON.
    String resp = "{\"ok\":true,\"locations\":" + body + "}";
    http.send(200, "application/json", resp);
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
        http.send(400, "text/plain", "Pick a location.");
        return;
    }

    if (!tryConnectIfNeeded(ssid.c_str(), password.c_str())) {
        WiFi.disconnect(false);
        http.send(400, "text/plain",
                  "Couldn't connect to that WiFi. Check SSID & password.");
        return;
    }

    if (!tryFetchTest(zip)) {
        WiFi.disconnect(false);
        http.send(400, "text/plain",
                  "Connected to WiFi, but couldn't fetch weather for that zip. "
                  "The server may be down or the location was just removed.");
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

    // Render splash with WiFi-join QR over the placeholder. iOS/Android scan
    // this string format as a "join WiFi" intent — one tap and the user's on
    // the AP, captive-portal popup follows.
    String wifiJoin = "WIFI:T:nopass;S:" + apSsid + ";;";
    renderSplash(wifiJoin.c_str());

    // DNS hijack: every name resolves to AP IP. Forces phones to load the
    // captive-portal probe from us, which triggers the auto-popup behavior.
    dns.start(DNS_PORT, "*", apIp);

    http.on("/", handleRoot);
    http.on("/scan", handleScan);
    http.on("/current", handleCurrent);
    http.on("/connect", HTTP_POST, handleConnect);
    http.on("/save", HTTP_POST, handleSave);
    http.on("/reset", HTTP_POST, handleReset);
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

    // Repaint plain splash (no QR) so the screen no longer suggests an active
    // AP. If the caller has NVS config, this'll get overwritten by weather
    // shortly; if not, plain splash is the correct end state.
    renderSplash();
}
