# Cloudflare Worker Backend (`worker/`)

A serverless API that fetches weather data from third-party providers, transforms it into a simplified JSON format, caches it in Cloudflare KV, and serves it to the ESP32 device.

## How It Works

The Worker operates in two modes:

1. **Cron trigger** (every 15 minutes) -- Fetches fresh weather data from WeatherAPI.com, transforms it into a compact JSON format, and stores it in KV with a 1-hour TTL.

2. **HTTP handler** (`GET /weather.json`) -- Serves the cached JSON from KV. If no cached data exists (cold start), fetches fresh data on demand.

This architecture means the ESP32 always gets a fast response from KV cache, and the weather data stays fresh via the background cron job.

### Provider Pattern

Weather data fetching is abstracted behind a provider interface. To swap weather APIs, implement a new `WeatherProvider` subclass and register it in the factory. The ESP32 firmware doesn't need to change -- only the Worker-side provider.

### JSON Response Format

```json
{
  "temperature": { "current": 38, "high": 41, "low": 29 },
  "weather": "partly_cloudy",
  "precipitation": [0, 0, 10, 20, 45, 60, 40, 20, 10, 5, 0, 0, ...],
  "precip_type": "rain",
  "uv": { "current": 1, "high": 2 },
  "sun": { "sunrise": "06:45 AM", "sunset": "05:30 PM" },
  "moon": { "illumination": 85, "phase": "Waxing Gibbous" },
  "is_day": true,
  "updated": "2026-02-08T14:30:00"
}
```

- `weather` -- One of: `sunny`, `cloudy`, `partly_cloudy`, `rainy`, `snowy`, `thunderstorm`, `fog`
- `precipitation` -- 24 hourly probability values (0-100), starting from the current hour
- `precip_type` -- Dominant type: `rain`, `snow`, or `mixed`
- `updated` -- ISO 8601 timestamp in Eastern Time

## Files

### `src/index.js`

Worker entry point. Exports the `fetch` and `scheduled` handlers.

**Key functions:**

- `fetch(request, env, ctx)` -- HTTP request handler. Routes `/weather.json` to serve cached data from KV, `/` to an info page, and everything else to 404.
- `scheduled(event, env, ctx)` -- Cron handler. Fetches fresh weather data and updates the KV cache.
- `fetchWeatherData(env)` -- Creates a weather provider instance from environment config and calls its `fetch()` method.
- `jsonResponse(data, status)` -- Helper to create JSON responses with CORS and cache headers.

### `src/providers/base.js`

Abstract base class for weather providers.

### `src/providers/weatherapi.js`

WeatherAPI.com provider implementation.

### `src/providers/index.js`

Provider factory and registry.

### `wrangler.toml`

Cloudflare Wrangler configuration. Defines the KV namespace binding (`WEATHER_KV`), environment variables (`WEATHER_PROVIDER`, `WEATHER_LOCATION`), and cron schedule (`*/15 * * * *`). The `WEATHER_API_KEY` is stored as a secret, not in this file.

## Classes

### `WeatherProvider` (`src/providers/base.js`)

Abstract base class that defines the provider interface. All weather API providers must extend this class.

- **Constructor** -- Takes `apiKey` and `location`. Throws if instantiated directly.
- `fetchRaw()` -- (abstract) Call the external weather API and return the raw response. Must be implemented by subclasses.
- `transform(rawData)` -- (abstract) Convert the raw API response into the standard JSON format described above. Must be implemented by subclasses.
- `fetch()` -- Convenience method that calls `fetchRaw()` then `transform()` in sequence. Inherited by all subclasses.

### `WeatherAPIProvider` (`src/providers/weatherapi.js`)

Extends `WeatherProvider`. Fetches data from `api.weatherapi.com/v1/forecast.json`.

- `fetchRaw()` -- Calls the WeatherAPI.com forecast endpoint with a 1-day forecast for the configured location.
- `transform(data)` -- Extracts and reshapes the raw response:
  - Builds a 24-hour precipitation array starting from the current hour, using the max of rain and snow probability for each hour.
  - Determines precipitation type (`rain`, `snow`, or `mixed`) based on the ratio of total rain vs snow probability across all hours.
  - Calculates high/low from hourly temperature data.
  - Extracts UV current and daily high from hourly UV values.
  - Pulls astronomy data (sunrise, sunset, moon phase, moon illumination).
  - Formats the update timestamp in Eastern Time.
- `_mapConditionToIcon(code, isDay)` -- Maps WeatherAPI condition codes to icon strings. Groups ~40 condition codes into 7 categories: sunny (1000), partly cloudy (1003), cloudy (1006-1009), fog (1030, 1135, 1147), rain (16 codes), snow (20 codes), thunderstorm (5 codes).

### Provider Factory (`src/providers/index.js`)

Not a class, but a factory module:

- `createProvider(providerName, apiKey, location)` -- Looks up the provider class by name in the `PROVIDERS` registry and returns a new instance. Throws if the provider name is unknown.
- `getAvailableProviders()` -- Returns a list of registered provider names. Currently only `"weatherapi"`.

## Setup

### 1. Get a WeatherAPI.com API Key

Sign up at https://www.weatherapi.com/signup.aspx (free tier is sufficient).

### 2. Install Dependencies

```bash
cd worker && npm install
```

### 3. Create KV Namespace

```bash
wrangler kv:namespace create WEATHER_KV
```

Copy the output `id` into `wrangler.toml`.

### 4. Set API Key

```bash
wrangler secret put WEATHER_API_KEY
```

### 5. Deploy

```bash
wrangler deploy
```

### Local Development

```bash
npm run dev
```

Test at http://localhost:8787/weather.json

## Adding New Providers

1. Create `src/providers/yourprovider.js` extending `WeatherProvider`
2. Implement `fetchRaw()` and `transform(data)`
3. Register in `src/providers/index.js`
4. Set `WEATHER_PROVIDER` environment variable to the new name

## Cost

100% free on Cloudflare's free tier (100,000 requests/day, unlimited cron triggers). At 15-minute intervals (96 fetches/day) plus ESP32 requests, usage stays well within limits.
