# Roadmap

Planned and in-progress work for Weather Claude — a Cloudflare Worker that renders
weather as grayscale PNGs and a LilyGo T5 4.7" e-paper device that displays them.
Rough priority order within each section.

## Firmware (device)

### On-device button menu
Replace the current single-gesture model on the IO21 user button (tap = ignored,
long-press = setup) with a scrollable on-device menu: tap to cycle options,
long-press to select. The menu renders on the e-paper itself (no companion app or
web page) — its main value is visibility when WiFi or the server is unreachable.

Planned options:
- **Live test** — actively run WiFi connect → server connect → data fetch and
  report each step's result on screen.
- **Debug logs** — a short persisted history of recent fetch attempts (time, WiFi
  result, HTTP code, age) plus live diagnostics (RSSI, fail counts, last-good fetch).
- **Setup mode** — re-enter the existing QR-code captive-portal onboarding
  (currently button-driven; folding it into the menu needs new UX).
- **Factory reset.**

Open questions: navigation feel given slow/flashy e-paper refresh, menu timeout,
and whether to pre-render static menu chrome as PNGs vs draw it on-device.

### Status iconography below the weather
Replace the single staleness *time* badge ("33m") with a set of small *binary*
status icons — a symbol can't freeze at a wrong value the way a number can.
Candidate icons: stale data, WiFi-connect failed, server/HTTP failed, battery low,
alongside the existing battery gauge.

Constraint to resolve first: the failure icons and battery-low surface exactly when
there is no fresh PNG to draw, and the current display path only does a full-screen
refresh. Showing them mid-failure requires either a partial-region refresh, caching
the last good PNG in flash, or relegating failure detail to the debug menu — that
decision gates the rest of the redesign. Fixing the staleness badge so it reliably
clears once data is fresh again is folded into this work rather than patched
separately.

### OTA (over-the-air) firmware updates
Let the device pull and flash new firmware from the worker over WiFi, so a gifted
device can be updated without physical access.

- The flash is already on an OTA-capable dual-slot layout (`app0`/`app1`/`otadata`),
  with ~3.3 MB slots vs ~1.15 MB firmware — no repartitioning needed.
- High-level tasks:
  1. **Device OTA codepath** — add a `FIRMWARE_VERSION`, wire up Arduino
     `HTTPUpdate`, reboot on success. Must be flashed once over USB *before gifting*,
     since the current firmware has no OTA code.
  2. **Worker endpoints** — report the latest version and serve the binary (R2 or KV).
  3. **Throttled version-check** on wake (e.g. once/day); update if newer.
  4. **Release workflow** — build `.bin`, upload, bump the version.
  5. **Harden for unattended devices** — rollback protection, battery guard, TLS cert
     validation on the OTA path, optionally signed images.

### Self-serve splash on unconfigured / offline boot
The splash PNG is rendered and committed (`worker/renderer/splash.png`); still need
firmware to display it on no-network / unconfigured boot. Waiting on device
availability for debugging.

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
