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
Remaining:
- **Debug logs** — a persisted history of recent fetch attempts + live diagnostics
  (RSSI, fail counts, last-good fetch). Drawn on-device since it matters most when
  offline. Likely the point to extract the menu into its own `menu.cpp`.
- **Setup-screen text rework** (the splash doubles as the onboarding and setup-QR
  screen) and a broader **menu-flow / back-out UX** pass.

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

### OTA (over-the-air) firmware updates — ✅ Shipped
The device pulls and flashes new firmware from the worker over WiFi — validated
end-to-end (devices have self-updated v1→v2 and v2→v3 over the air). Discovery is
**free and every wake**: the worker advertises the latest available version on
every weather response via the `X-Firmware-Latest` header, and the device flashes a
newer build *after* rendering the weather (no separate request, no once/day
throttle). Release flow + scripts are in `docs/ota.md` and
`firmware/scripts/ota-{publish,promote,add-fast-device}.sh`.

Fast/slow release channels still exist server-side (resolved by chip ID on the
legacy `/firmware/check` endpoint, which pre-piggyback firmware uses to bootstrap
onto a piggyback build), but current firmware **skips channel resolution** — the
weather header always reports the *fast* version, so every updated device tracks
fast.

Follow-ups:
- **Migrate binary storage from KV to R2.** The binary currently lives in Cloudflare
  KV (`firmware:bin:{version}`, ~1.1 MB) as an interim measure — R2 wasn't enabled on
  the account. Move it to a dedicated R2 bucket (`weather-esp32-firmware`) once R2 is
  available; the device-facing `/firmware/{version}.bin` URL is unchanged, so only the
  worker's read and the `ota-publish.sh` upload change.
- **Per-device channels on the weather header.** Restore the fast/slow split for the
  piggyback path (e.g. send the chip ID on the weather fetch and resolve the channel
  server-side) so gifted devices can lag a canary again. `ota-promote.sh` no longer
  gates current devices until this lands.
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
