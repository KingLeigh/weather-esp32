# Roadmap

Planned and in-progress work for Weather Claude — a Cloudflare Worker that renders
weather as grayscale PNGs and a LilyGo T5 4.7" e-paper device that displays them.
Rough priority order within each section.

## Firmware (device)

### On-device button menu — ✅ Core shipped (PR #4)
Replaces the single-gesture IO21 model (tap = ignored, long-press = setup) with a
button-operated menu on the e-paper: long-press opens it, short press cycles a
cursor, long press selects, and idle returns home (weather if configured, else the
onboarding splash). Rendered on-device (no companion app) — its main value is
visibility when WiFi or the server is unreachable.

Shipped: **Exit**, **Device setup** (the existing captive-portal onboarding),
**Factory reset** (with a confirmation step), **Debug mode → Live test**
(actively runs WiFi → server → fetch and reports each result on-screen, including
the firmware version with "up to date" / "update available"), and a
**partial-refresh cursor** (a move repaints only the cursor column instead of the
whole screen, with a full refresh on wrap-to-top to clear e-paper ghosting).
Since then: a **dedicated setup screen** split from the splash (v6), and
**long-press from the setup screen → menu** (v7) so Debug / Factory reset are
reachable even from a bad-WiFi state.
Remaining:
- **Debug logs / failure ring buffer** — ✅ **shipped (v8)**: the on-device
  "Recent Errors" screen (Debug live test → press → Recent Errors). A persisted
  (RTC) chronological log of recent failures, consecutive repeats coalesced with
  a count + start time, with exhaustive kinds beyond the status bar's NET/SRV
  (HTTP status, transport code, empty/chunked, OOM, truncated, decode/IMG, NTP,
  OTA). Still deferred: extracting the menu/debug code into its own `menu.cpp`.
- **Long-press fires on threshold, not release** — today the long-press action
  (and the wake→menu paint) waits for you to *release* the button, so there's no
  feedback while you hold and you can't tell how long is long enough. Make it act
  the instant the hold crosses `BUTTON_HOLD_MS`, then swallow the still-held
  button via a shared "waiting-for-release" gate so it isn't re-read — and can't
  bleed into the factory-reset confirm and auto-confirm it. Two spots:
  `readButtonEvent()` (returns `BTN_LONG` only after release) and the shared
  `waitForButtonRelease()` gate run before the first screen opens.
  Consider also dropping `BUTTON_HOLD_MS` (1500 ms) once there's instant feedback.

### Screen state-machine refactor — ✅ Shipped (v9)
The on-device UI is now a small, declarative state machine. Each interactive
screen is a `Screen` (a render fn + `onShort`/`onLong`/`onIdle` handlers that
return a navigation result), driven by one shared poll loop (`runScreen`) and one
navigator (`runUi`) — replacing five hand-rolled button loops. Adding a screen is
now "fill in a struct and route to it," not "copy a loop."

UX is unified to match:
- **Long-press from any home screen opens the Menu** (the single hub) — weather,
  onboarding splash, and no-WiFi splash all behave the same. Device setup is
  reached via **Menu → Device setup** (the old splash → setup-direct path is gone).
- **Every sub-screen exits Home, never back to the Menu** — Device setup, Factory
  reset, Debug, Recent Errors. Re-entering the menu is a deliberate long-press.
- **Short and long press both act on every interactive screen** (Device setup's
  short press now exits, matching long press); one 30 s idle timeout for all awake
  screens (Device setup keeps its longer 3-min timeout).
- Returning to weather **keeps the previous screen on the panel** until the fresh
  weather is ready (a brief "blank while loading" interstitial was tried and
  removed — it read as a dead device).

Screen polish along the way: the Debug live test is titled **Device info** (the
WiFi line drops the dBm reading), the Factory reset screen gained a title +
divider to match, and the onboarding splash text was rewritten for the
Menu → Device setup flow.

### Status codes below the weather — ✅ Shipped (v5)
Replaced the single staleness *time* badge ("33m") and the always-on battery icon
with one priority-ranked 3-letter code in the reserved corner, shown only on a
problem (blank when healthy): `NET` (WiFi failed), `SRV` (WiFi up but fetch
failed), `OLD` (server data >60 min stale), `BAT` (battery low — lowest priority
since it lingers for days). Diverged from the original "icons" idea: text codes
carry more meaning per glyph and, like icons, never freeze at a wrong value.

The blocker (no fresh PNG to draw on a failed fetch) is solved by
**partial-refreshing just the corner box** over the weather still held on the
e-paper, so `NET`/`SRV` appear mid-outage without wiping the screen. Battery moved
to a **pure-voltage model** (16-sample average, 3.5 V trip, RTC hysteresis latch);
the old linear "%" was meaningless on a LiPo's flat discharge curve.

Follow-ups:
- **Calibrate the BAT trip voltage.** 3.5 V is an educated guess (~20–30% real
  charge left, ±~10 pts), read under WiFi load, not measured from this cell. Proper
  fix: log raw voltage per wake (e.g. a fetch header the worker records) to capture
  the real discharge curve, then set the threshold from data.
- **`IMG` and `CLK` codes.** `IMG` for a fetch that succeeds but the PNG won't
  decode (a silent gap today); `CLK` for "NTP never synced" (which makes `OLD`
  untrustworthy). Both slot into the existing priority list.
- **`OLD` while offline** — needs the last-good `X-Updated` persisted in RTC (today
  `OLD` only shows on a successful fetch).
- **Offline → splash fallback** — ✅ **shipped (v7)**: after ~3h offline a
  configured device drops to the splash with a "WiFi network unavailable" message,
  rechecks every 30 min, and restores weather on reconnect. Also clears any
  partial-refresh ghosting.

### OTA (over-the-air) firmware updates — ✅ Shipped
The device pulls and flashes new firmware from the worker over WiFi — validated
end-to-end. Discovery is **free and every wake**: the worker advertises the latest
available version on every weather response via the `X-Firmware-Latest` header
(read from the `firmware:latest` KV key), and the device flashes a newer build
*after* rendering the weather (no separate request, no throttle). There is a
**single release stream** — `ota-publish.sh` bumps `firmware:latest` and every
device picks it up on its next wake. (The earlier fast/slow channel scheme and the
`/firmware/check` endpoint were removed 2026-06-24.) Release flow + the one script
are in `docs/ota.md` and `firmware/scripts/ota-publish.sh`.

Follow-ups:
- **Migrate binary storage from KV to R2.** The binary currently lives in Cloudflare
  KV (`firmware:bin:{version}`, ~1.2 MB) as an interim measure — R2 wasn't enabled on
  the account. Move it to a dedicated R2 bucket (`weather-esp32-firmware`) once R2 is
  available; the device-facing `/firmware/{version}.bin` URL is unchanged, so only the
  worker's read and the `ota-publish.sh` upload change.
- **OTA diagnostics + manual control on-device.**
  - **Force-retry an update** — a way to bypass the failure cooldown and re-attempt
    on demand (e.g. from the menu), instead of waiting out `OTA_FAIL_COOLDOWN_WAKES`.
  - **Surface *why* an OTA failed** — persist + show the `httpUpdate` error
    (`getLastError()` / `getLastErrorString()`) and the failed version on-screen, so a
    stuck/looping update is diagnosable without serial.
  - This likely needs a **second page of debug info**, or possibly its own
    **"Software update" menu item**, rather than crowding the Live test screen.

### Smaller items
- **Battery life** — consider raising the device poll interval (`SLEEP_MINUTES`,
  currently 10 min) toward 15.
- **Grainy grey fills** — chart bars (~`#ccc`) may render grainy on the panel;
  investigate RGB565 round-trip / resvg dithering / EPD waveform, or quantize to
  4-bpp-aligned values before PNG encoding.

## Server (Cloudflare Worker)

### Severe weather alerts
The OpenWeatherMap `alerts` field is currently excluded. Explore surfacing alerts
with severity filtering; the main concern is over-alerting on minor advisories.

### De-duplicate the PNG encoder
The encoder is shared between `worker/renderer/src/preview.js` and
`preview-splash.jsx` via `png-encode.js`, but the Worker's `worker/src/render.jsx`
still keeps its own copy. Needs a pluggable-compression variant usable in Workers.
