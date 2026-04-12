# Cloudflare Worker Backend (`worker/`)

A serverless API that fetches weather data from third-party providers, renders a 960x540 grayscale PNG for an e-paper display, caches both in Cloudflare KV, and serves them to the ESP32 device.

## Architecture

The Worker operates in two modes:

1. **Cron trigger** (every 15 minutes) — Fetches weather data from the configured provider, renders a grayscale PNG via the satori/resvg pipeline, and caches both the JSON and PNG in KV with a 1-hour TTL.

2. **HTTP handler** — Serves cached responses from KV. If the cache is cold, fetches and renders on demand.

### Endpoints

| Endpoint | Response | Headers |
|---|---|---|
| `GET /weather.json` | Weather data as JSON | Standard CORS + cache |
| `GET /weather.png` | 960x540 8bpp grayscale PNG | `X-Updated` (ISO timestamp), CORS + cache |
| `GET /` | HTML info page | — |

### Render pipeline

The PNG render pipeline runs entirely inside the Worker:

```
WeatherData (JSON)
  → layout.jsx (JSX components, shared with local preview)
  → satori/standalone (JSX → SVG, using pre-compiled yoga WASM)
  → @resvg/resvg-wasm (SVG → RGBA pixels, pre-compiled WASM)
  → grayscale PNG encoder (RGBA → luma → zlib via CompressionStream)
  → ~11-13 KB grayscale PNG
```

Cloudflare Workers block runtime `WebAssembly.compile()`, so both yoga and resvg WASM modules are imported as pre-compiled `WebAssembly.Module` objects via wrangler's `CompiledWasm` module rule. This is the standard pattern for WASM on Workers.

### Caching

```
Cron (every 15 min)
  ├─ fetchWeatherData()  → KV 'current'       (JSON, TTL 1h)
  ├─ renderWeatherPng()  → KV 'render_png'    (PNG bytes, TTL 1h)
  └─                     → KV 'render_updated' (timestamp, TTL 1h)
```

Both endpoints read from KV first. On cache miss (first request after deploy, or after KV TTL expires between cron runs), data is fetched and rendered on-demand before being cached.

### Provider pattern

Weather data fetching is abstracted behind a `WeatherProvider` interface (`src/providers/base.js`). To swap APIs, implement a new subclass and register it in the factory. The layout and firmware don't change.

Available providers:
- `openweathermap` — OpenWeatherMap One Call 3.0 (current default)
- `weatherapi` — WeatherAPI.com

## Files

```
worker/
├── src/
│   ├── index.js              # Worker entry: fetch + scheduled handlers, routing, KV cache
│   ├── render.jsx             # Worker-compatible render pipeline (satori + resvg-wasm + PNG encoder)
│   └── providers/
│       ├── base.js            # Abstract WeatherProvider class
│       ├── index.js           # Provider factory
│       ├── openweathermap.js  # OpenWeatherMap One Call 3.0
│       └── weatherapi.js      # WeatherAPI.com
├── renderer/                  # Local preview tooling (shared layout, Node.js render, preview scripts)
│   ├── src/
│   │   ├── layout.jsx         # Shared UI layout (imported by both local preview and Worker)
│   │   ├── render.jsx         # Node.js render pipeline (resvg-js, used by local preview only)
│   │   ├── preview.js         # Local preview: renders preview.svg + preview.png
│   │   ├── preview-icons.jsx  # Icon sampler grid
│   │   └── preview-weather-icons.jsx  # Production icon showcase
│   ├── fonts/                 # FiraSans TTFs (downloaded by postinstall script)
│   ├── weather-sample.json    # Sample data for local preview
│   └── package.json           # Renderer dependencies (satori, resvg-js, tsx)
├── wrangler.toml              # Cloudflare config: KV binding, cron, module rules, compat flags
├── package.json               # Worker dependencies (satori, react, resvg-wasm)
└── test.js                    # Local provider test script
```

## JSON response format

```json
{
  "temperature": { "current": 55, "high": 63, "low": 52 },
  "weather": "sunny",
  "precipitation": [0, 0, 10, 20, 45, 60, 40, 20, 10, 5, 0, 0, ...],
  "precip_type": "rain",
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
- `precipitation` — 24 hourly probability values (0–100), starting from the current hour
- `precip_type` — Dominant type: `rain`, `snow`, or `mixed`
- `updated` — ISO 8601 timestamp in Eastern Time (no timezone suffix)

## Setup from scratch

### Prerequisites

- Node.js 18+
- Cloudflare account with **Workers Paid plan** ($5/mo) — required because the bundled WASM modules (satori yoga + resvg) push the script size to ~4.5 MB, above the free plan's 1 MB limit
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

Paste your OpenWeatherMap API key when prompted. This is stored securely in Cloudflare and never appears in code or config files.

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
curl -s https://weather-esp32.leigh-herbert.workers.dev/weather.json | head -5
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
npm run preview:icons    # Erik Flowers icon sampler
```

Open `preview.svg` in a browser for fast layout iteration.

## Adding new providers

1. Create `src/providers/yourprovider.js` extending `WeatherProvider`
2. Implement `fetchRaw()` and `transform(data)` — output must match the JSON format above
3. Register in `src/providers/index.js`
4. Set `WEATHER_PROVIDER = "yourprovider"` in `wrangler.toml`

## Cost

- **Cloudflare Workers Paid**: ~$5/month (required for >1 MB script size)
- **OpenWeatherMap One Call 3.0**: Free for 1,000 calls/day. At 96 calls/day (every 15 min), well within limits. Set a daily cap in the OWM dashboard as a safety net.
