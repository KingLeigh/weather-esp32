# Weather Display for ESP32 E-Paper

An always-on weather display built on the LilyGo T5 4.7" S3 E-Paper board (ESP32-S3, 960x540 pixels, 4-bit grayscale). A Cloudflare Worker renders the weather UI as a grayscale PNG; the ESP32 fetches and displays it, waking from deep sleep every 5 minutes to check for updates.

## Architecture

```
  OpenWeatherMap API
        ↓
  Cloudflare Worker (every 15 min)
    ├─ /weather.json  (raw JSON, for legacy firmware)
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
- 24-hour precipitation probability chart with real time axis labels
- Sunrise and sunset times
- Battery level indicator (device-side overlay)
- Data staleness warning when data is older than 30 minutes (device-side overlay)

## Project Structure

```
weather-claude/
├── firmware-png/        ESP32 firmware: fetch PNG → decode → display (active)
├── src/                 ESP32 firmware: fetch JSON → render on-device (legacy)
├── firmware-png-test/   Disposable: embedded PNG decode smoke test
├── worker/
│   ├── src/             Cloudflare Worker API (routing, caching, providers)
│   └── renderer/        Shared layout + local preview tooling
├── tools/               Icon conversion utilities
└── platformio.ini       PlatformIO config for legacy firmware
```

### `firmware-png/` — Active Firmware

The production firmware. Fetches `/weather.png` from the Worker, decodes with PNGdec, draws battery + staleness overlay, pushes to e-paper, deep sleeps for 5 minutes. Uses PNG hash in RTC memory for change detection.

### `src/` — Legacy Firmware

The original "smart" firmware that fetches `/weather.json` and renders the entire UI on-device. Kept for reference; may be retired once the PNG pipeline is fully proven.

### `worker/` — Cloudflare Worker Backend

Server-side weather API + PNG rendering pipeline. See [`worker/README.md`](worker/README.md) for full documentation including setup-from-scratch instructions.

### `firmware-png-test/` — Smoke Test (Disposable)

Minimal firmware that decodes an embedded PNG (no WiFi). Used to verify PNGdec + e-paper pipeline before building the network side. Can be deleted once no longer needed.

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

1. Create WiFi config:
   ```bash
   # Edit firmware-png/include/wifi_config.h with your WiFi credentials
   # (see template comments in the file)
   ```

2. Build and flash:
   ```bash
   cd firmware-png
   pio run -t upload
   ```

3. Monitor serial output:
   ```bash
   pio device monitor --baud 115200
   ```

### Local Layout Preview

```bash
cd worker/renderer
npm run preview          # renders preview.svg + preview.png from sample data
```

Open `preview.svg` in a browser for fast layout iteration.
