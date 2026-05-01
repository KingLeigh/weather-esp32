# Weather Display for ESP32 E-Paper

An always-on weather display built on the LilyGo T5 4.7" S3 E-Paper board (ESP32-S3, 960x540 pixels, 4-bit grayscale). A Cloudflare Worker renders the weather UI as a grayscale PNG; the ESP32 fetches and displays it, waking from deep sleep every 5 minutes to check for updates.

## Architecture

```
  OpenWeatherMap API
        ↓
  Cloudflare Worker (every 3 min)
    └─ /weather.png   (960x540 grayscale PNG, server-rendered)
        ↓
  ESP32 (every 5 min wake)
    ├─ fetch PNG → decode → display
    ├─ battery overlay   ← drawn on-device
    └─ staleness overlay ← drawn on-device
        ↓
  4.7" E-Paper Display
```

All weather UI rendering happens on the server (JSX layout → satori → resvg → grayscale PNG). The ESP32 is a "dumb" display that fetches the PNG, composites a battery and staleness overlay in the bottom-right corner, and pushes it to the e-paper. If nothing has changed since the last wake, the display refresh is skipped to save power and avoid flicker.

## What the Display Shows

- Current temperature (large) with high/low
- Weather condition icon (Erik Flowers Weather Icons font)
- UV index with custom sun icon (current + daily high)
- 24-hour chart: temperature time series (dry) or precipitation bars (rain/snow)
- Right-aligned summary: "No umbrella needed!" or "Rain at 4pm · 0.8" total"
- Battery level indicator (device-side overlay)
- Data staleness warning when data is older than 30 minutes (device-side overlay)

## Project Structure

```
weather-claude/
├── firmware/            ESP32 firmware: captive-portal setup + weather display
├── firmware-probe/      Disposable: GPIO button identification sketch
├── firmware-png-test/   Disposable: embedded PNG decode smoke test
└── worker/
    ├── src/             Cloudflare Worker (routing, caching, providers, renderer)
    └── renderer/        Shared layout + local preview tooling
```

### `firmware/` — ESP32 Firmware

The production firmware. Fetches `/weather/{zip}.png` from the Worker for the
device's configured location, decodes with PNGdec, draws battery + staleness
overlay, pushes to e-paper, deep sleeps. Uses PNG hash in RTC memory for
change detection.

WiFi credentials and zip code live in NVS (the ESP32's non-volatile flash
partition), populated through a self-serve captive-portal flow on first boot
and any time the user long-presses the IO21 button. See
[`firmware/README.md`](firmware/README.md) (if present) or just read
`firmware/src/main.cpp` for the full state machine.

### `worker/` — Cloudflare Worker

Server-side weather API + PNG rendering pipeline. See
[`worker/README.md`](worker/README.md) for full documentation including
setup-from-scratch instructions.

### `firmware-probe/` & `firmware-png-test/` — Disposable

Minimal one-off sketches for hardware bring-up. `firmware-probe/` identifies
which GPIO each physical button is wired to; `firmware-png-test/` verifies the
PNGdec + e-paper pipeline with an embedded PNG. Safe to delete once no longer
useful.

## Getting Started

### Prerequisites

- [PlatformIO CLI](https://platformio.org/install/cli)
- LilyGo T5 4.7" S3 E-Paper board
- Cloudflare account with Workers Paid plan ($5/mo)
- [OpenWeatherMap](https://openweathermap.org/api/one-call-3) API key (One Call 3.0, free tier)

### Worker Setup

```bash
cd worker && npm install
cd renderer && npm install   # also downloads fonts
npx wrangler kv:namespace create WEATHER_KV   # paste ID into wrangler.toml
npx wrangler secret put WEATHER_API_KEY       # paste your OWM key
npm run deploy
```

### ESP32 Setup

1. Build and flash:
   ```bash
   cd firmware
   pio run -t upload
   ```

2. Monitor serial output:
   ```bash
   pio device monitor --baud 115200
   ```

3. Configure WiFi + location through the device:
   - On first boot the screen shows a "Setup required" splash with
     instructions and a placeholder for a QR code.
   - **Long-press IO21** for ~1.5 seconds. The device wakes into setup
     mode: a WiFi access point named `WhatsTheWeather-XXXX` (last 4 of
     the device's MAC) starts broadcasting, and a real WiFi-join QR
     overdraws the placeholder.
   - **Scan the QR with your phone's camera** to auto-join the AP.
     iOS/Android pop a captive portal automatically.
   - Pick your home WiFi, enter the password, hit **Connect**. The device
     verifies the WiFi works, then fetches the registered location list
     from the Worker.
   - Pick a location from the dropdown, hit **Save**. The device verifies
     it can fetch weather for that zip, writes to NVS, and reboots into
     normal operation.
   - Locations come from the Worker's `/locations` endpoint, populated
     by the admin page at `/admin`. Add a zip there before flashing
     (or any time after — the device's setup form will see new entries
     on next setup).

   To re-configure later (change WiFi or location), long-press IO21 at
   any time. To wipe all settings before gifting the device, hit the
   **Factory reset** link at the bottom of the captive-portal form.

### Local Layout Preview

```bash
cd worker/renderer
npm run preview          # renders preview.svg + preview.png from sample data
npm run preview:all      # side-by-side: dry, rain, rain+snow variants
```

Open `preview.svg` or `preview-all.html` in a browser for fast layout iteration.
