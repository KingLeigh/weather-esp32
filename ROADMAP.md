# Roadmap

Planned and in-progress work for Weather Claude — a Cloudflare Worker that renders
weather as grayscale PNGs and a LilyGo T5 4.7" e-paper device that displays them.
Rough priority order within each section.

## Firmware (device)

### On-device button menu — 🚧 In progress (PR #4)
Replaces the single-gesture IO21 model (tap = ignored, long-press = setup) with a
button-operated menu on the e-paper: long-press opens it, short press cycles a
cursor, long press selects, and idle returns home (weather if configured, else the
onboarding splash). Rendered on-device (no companion app) — its main value is
visibility when WiFi or the server is unreachable.

Wired so far: **Exit**, **Device setup** (the existing captive-portal onboarding),
and **Factory reset** (with a confirmation step). Remaining:
- **Debug mode** — *Live test* (WiFi → server → fetch, reporting each step on screen)
  and *Debug logs* (a persisted history of recent fetch attempts + live diagnostics
  like RSSI, fail counts, last-good fetch). Drawn on-device since it matters most when
  offline. Likely the point to extract the menu into its own `menu.cpp`.
- **Partial-refresh cursor** — the cursor currently does a full-screen refresh per move.
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
end-to-end (a device self-updated v1→v2 over the air). Fast/slow release channels
are resolved server-side by chip ID, so each release can be canaried on your own
device before it reaches gifted ("slow") devices. Release flow + scripts are in
`docs/ota.md` and `firmware/scripts/ota-{publish,promote,add-fast-device}.sh`.

Follow-ups:
- **Migrate binary storage from KV to R2.** The binary currently lives in Cloudflare
  KV (`firmware:bin:{version}`, ~1.1 MB) as an interim measure — R2 wasn't enabled on
  the account. Move it to a dedicated R2 bucket (`weather-esp32-firmware`) once R2 is
  available; the device-facing `/firmware/{version}.bin` URL is unchanged, so only the
  worker's read and the `ota-publish.sh` upload change.
- **Piggyback the version check on the weather fetch.** The device already fetches the
  weather PNG every wake; adding the channel-resolved latest version as a header on that
  response would let it discover new builds for free — dropping the separate
  `/firmware/check` request (and probably the once/day throttle).

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
