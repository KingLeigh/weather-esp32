# Weather ESP32 Cloudflare Worker

Cloudflare Worker that fetches weather data from WeatherAPI.com (or other providers) and serves it in a format optimized for your ESP32 weather display.

## Architecture

- **Provider Pattern**: Easily swap weather APIs by implementing the `WeatherProvider` interface
- **KV Caching**: Weather data cached in Cloudflare KV for fast response times
- **Scheduled Updates**: Cron job fetches fresh data every 15 minutes
- **HTTP Endpoint**: ESP32 fetches data via simple GET request

## Setup

### 1. Get WeatherAPI.com API Key

1. Sign up at https://www.weatherapi.com/signup.aspx (free)
2. Get your API key from the dashboard
3. Keep it handy for step 4

### 2. Install Cloudflare Wrangler CLI

```bash
npm install -g wrangler
```

### 3. Login to Cloudflare

```bash
wrangler login
```

### 4. Create KV Namespace

```bash
wrangler kv:namespace create WEATHER_KV
```

This will output something like:
```
{ binding = "WEATHER_KV", id = "abc123..." }
```

Copy the `id` and update `wrangler.toml` line 7.

### 5. Set API Key as Secret

```bash
wrangler secret put WEATHER_API_KEY
# Paste your WeatherAPI.com API key when prompted
```

### 6. Deploy

```bash
npm install  # Install dependencies
wrangler deploy
```

Your worker will be deployed to: `https://weather-esp32.YOUR-SUBDOMAIN.workers.dev`

## Usage

### Fetch Weather Data

```bash
curl https://weather-esp32.YOUR-SUBDOMAIN.workers.dev/weather.json
```

Returns:
```json
{
  "temperature": {
    "current": 72,
    "high": 78,
    "low": 65
  },
  "weather": "partly_cloudy",
  "precipitation": [0, 0, 10, 20, 45, 60, 40, 20, 10, 5, 0, 0],
  "uv": {
    "current": 6,
    "high": 9
  },
  "updated": "2024-02-08T14:30:42Z"
}
```

## Configuration

Edit `wrangler.toml` to change settings:

- **WEATHER_LOCATION**: Location to fetch weather for (default: "Manhattan")
- **WEATHER_PROVIDER**: Provider name (default: "weatherapi")

To change location after deployment:
```bash
wrangler secret put WEATHER_LOCATION
# Enter: New York, NY
```

## Adding New Weather Providers

1. Create a new provider in `src/providers/`:

```javascript
// src/providers/openweathermap.js
import { WeatherProvider } from './base.js';

export class OpenWeatherMapProvider extends WeatherProvider {
  async fetchRaw() {
    // Implement API fetch
  }

  transform(data) {
    // Transform to standard format
  }
}
```

2. Register in `src/providers/index.js`:

```javascript
import { OpenWeatherMapProvider } from './openweathermap.js';

const PROVIDERS = {
  'weatherapi': WeatherAPIProvider,
  'openweathermap': OpenWeatherMapProvider,  // Add here
};
```

3. Update environment variable:
```bash
wrangler secret put WEATHER_PROVIDER
# Enter: openweathermap
```

## Local Development

```bash
npm run dev
```

Test locally at http://localhost:8787/weather.json

## Monitoring

View logs:
```bash
wrangler tail
```

## Cost

**100% Free** on Cloudflare's free tier:
- 100,000 requests/day
- KV: 100,000 reads/day
- Cron triggers: Unlimited

At 15-minute intervals (96 fetches/day) + ESP32 requests, you'll stay well within limits.
