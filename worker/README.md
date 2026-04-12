# Cloudflare Worker Backend (`worker/`)

A serverless renderer that fetches weather data, renders a 960x540 grayscale PNG for an e-paper display, caches it in Cloudflare KV, and serves it to the ESP32.

## Architecture

The Worker operates in two modes:

1. **Cron trigger** (every 3 minutes) — Fetches weather data from the configured provider. Re-renders the PNG only when the weather data has actually changed (hash comparison); otherwise just refreshes the timestamp so the device sees fresh data.

2. **HTTP handler** (`GET /weather.png`) — Serves the cached PNG from KV. If the cache is cold, fetches and renders on demand.

### Render pipeline

```
WeatherData (from provider)
  → layout.jsx (JSX components, shared with local preview)
  → satori/standalone (JSX → SVG, using pre-compiled yoga WASM)
  → @resvg/resvg-wasm (SVG → RGBA pixels, pre-compiled WASM)
  → grayscale PNG encoder (RGBA → luma → zlib via CompressionStream)
  → ~11-14 KB grayscale PNG
```

Cloudflare Workers block runtime `WebAssembly.compile()`, so both yoga and resvg WASM modules are imported as pre-compiled `WebAssembly.Module` objects via wrangler's `CompiledWasm` module rule.

### Caching

```
Cron (every 3 min)
  ├─ fetchWeatherData()    (always — keeps timestamp fresh)
  ├─ hash comparison       (skip render if data unchanged)
  ├─ renderWeatherPng()  → KV 'render_png'     (PNG bytes, TTL 1h)
  ├─                     → KV 'render_updated'  (timestamp, TTL 1h)
  └─                     → KV 'render_hash'     (data hash, TTL 1h)
```

The `/weather.png` endpoint reads from KV first. On cache miss, it fetches and renders on-demand.

### Provider pattern

Weather data fetching is abstracted behind a `WeatherProvider` interface (`src/providers/base.js`). To swap APIs, implement a new subclass and register it in the factory. The layout and firmware don't change.

Current provider: **OpenWeatherMap One Call 3.0**

## Files

```
worker/
├── src/
│   ├── index.js              # Worker entry: fetch + scheduled handlers, routing, KV cache
│   ├── render.jsx             # Worker-compatible render pipeline (satori + resvg-wasm + PNG encoder)
│   └── providers/
│       ├── base.js            # Abstract WeatherProvider class
│       ├── index.js           # Provider factory
│       └── openweathermap.js  # OpenWeatherMap One Call 3.0
├── renderer/                  # Local preview tooling (shared layout, Node.js render, preview scripts)
│   ├── src/
│   │   ├── layout.jsx         # Shared UI layout (imported by both local preview and Worker)
│   │   ├── render.jsx         # Node.js render pipeline (resvg-js, local preview only)
│   │   ├── preview.js         # Renders preview.svg + preview.png from sample data
│   │   ├── preview-all.js     # Renders all chart variants side-by-side in HTML
│   │   ├── preview-icons.jsx  # Erik Flowers icon sampler grid
│   │   └── preview-weather-icons.jsx  # Production icon showcase
│   ├── fonts/                 # FiraSans TTFs (downloaded by postinstall script)
│   ├── weather-sample.json    # Active sample data for preview
│   ├── weather-sample-dry.json       # Dry weather (temp chart)
│   ├── weather-sample-rain-snow.json # Rain + snow (precip chart)
│   └── package.json           # Renderer dependencies (satori, resvg-js, tsx)
├── wrangler.toml              # Cloudflare config: KV binding, cron, module rules, compat flags
└── package.json               # Worker dependencies (satori, react, resvg-wasm)
```

## Weather data format

The provider outputs this structure, consumed by `layout.jsx`:

```json
{
  "temperature": { "current": 55, "high": 63, "low": 52 },
  "weather": "sunny",
  "rain_chance": [0, 0, 10, 20, 45, ...],
  "snow_chance": [0, 0, 0, 0, 0, ...],
  "hourly_temp": [55, 54, 53, 52, ...],
  "rain_mm": 0.8,
  "snow_mm": 0,
  "uv": { "current": 6, "high": 9 },
  "sun": { "sunrise": "06:45 AM", "sunset": "07:28 PM" },
  "moon": { "illumination": 68, "phase": "Waxing Gibbous" },
  "wind": { "mph": 12, "dir": "NW" },
  "is_day": true,
  "updated": "2026-04-11T22:50:39"
}
```

- `weather` — One of: `sunny`, `cloudy`, `partly_cloudy`, `rainy`, `snowy`, `thunderstorm`, `fog`
- `rain_chance`, `snow_chance` — 24 hourly probability values (0–100)
- `hourly_temp` — 24 hourly temperatures (°F)
- `updated` — ISO 8601 timestamp in Eastern Time (no timezone suffix)

## Setup from scratch

### Prerequisites

- Node.js 18+
- Cloudflare account with **Workers Paid plan** ($5/mo) — the bundled WASM modules push the script to ~4.5 MB, above the free plan's 1 MB limit
- An OpenWeatherMap API key with [One Call 3.0](https://openweathermap.org/api/one-call-3) enabled (free tier: 1,000 calls/day — set a daily limit in the dashboard to avoid charges)

### 1. Install dependencies

```bash
cd worker
npm install

cd renderer
npm install    # also runs postinstall to download fonts
```

### 2. Create KV namespace

```bash
npx wrangler kv:namespace create WEATHER_KV
```

Copy the `id` from the output into `wrangler.toml`:

```toml
[[kv_namespaces]]
binding = "WEATHER_KV"
id = "<paste-id-here>"
```

### 3. Set API key secret

```bash
npx wrangler secret put WEATHER_API_KEY
```

### 4. Configure location (optional)

Edit `wrangler.toml` to change the default location:

```toml
[vars]
WEATHER_PROVIDER = "openweathermap"
WEATHER_LOCATION = "40.739214,-73.987265"   # lat,lon
```

### 5. Deploy

```bash
npm run deploy
```

Verify:
```bash
curl -I https://weather-esp32.leigh-herbert.workers.dev/weather.png
```

### Local development

**Worker** (requires API key in env or `.dev.vars` file):
```bash
cd worker
npm run dev              # wrangler dev on http://localhost:8787
```

**Layout preview** (no API key needed, uses sample data):
```bash
cd worker/renderer
npm run preview          # renders preview.svg + preview.png
npm run preview:all      # dry, rain-only, and rain+snow side-by-side
```

Open `preview.svg` or `preview-all.html` in a browser for fast layout iteration.

## Adding new providers

1. Create `src/providers/yourprovider.js` extending `WeatherProvider`
2. Implement `fetchRaw()` and `transform(data)` — output must include `rain_chance[]`, `snow_chance[]`, `hourly_temp[]`, and all other fields above
3. Register in `src/providers/index.js`
4. Set `WEATHER_PROVIDER = "yourprovider"` in `wrangler.toml`

## Cost

- **Cloudflare Workers Paid**: ~$5/month (required for >1 MB script size)
- **OpenWeatherMap One Call 3.0**: Free for 1,000 calls/day. At 480 calls/day (every 3 min), within limits. Set a daily cap in the OWM dashboard as a safety net.
