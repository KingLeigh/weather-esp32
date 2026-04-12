import { createProvider } from './providers/index.js';
import { renderWeatherPng } from './render.jsx';

/**
 * Cloudflare Worker for weather data
 *
 * Handles:
 * 1. HTTP GET /weather.json - Serve cached weather JSON to ESP32
 * 2. HTTP GET /weather.png  - Serve rendered weather PNG for e-paper display
 * 3. Scheduled cron - Fetch fresh data + render PNG every 15 minutes
 */

export default {
  /**
   * HTTP request handler
   */
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    // Handle /weather.json endpoint
    if (url.pathname === '/weather.json') {
      const cachedData = await env.WEATHER_KV.get('current', 'json');

      if (!cachedData) {
        try {
          const weatherData = await fetchWeatherData(env);
          await storeWeatherData(env, weatherData);
          return jsonResponse(weatherData);
        } catch (error) {
          return jsonResponse({ error: 'Failed to fetch weather data' }, 500);
        }
      }

      return jsonResponse(cachedData);
    }

    // Handle /weather.png endpoint — rendered e-paper image
    if (url.pathname === '/weather.png') {
      try {
        // Try cached PNG first.
        const [cachedPng, cachedUpdated] = await Promise.all([
          env.WEATHER_KV.get('render_png', 'arrayBuffer'),
          env.WEATHER_KV.get('render_updated', 'text'),
        ]);

        if (cachedPng) {
          return pngResponse(cachedPng, cachedUpdated || '');
        }

        // Cache miss — fetch weather, render, cache, and serve.
        const weatherData = await fetchWeatherData(env);
        await storeWeatherData(env, weatherData);

        const png = await renderWeatherPng(weatherData);
        await storeRenderedPng(env, png, weatherData.updated);

        return pngResponse(png, weatherData.updated);
      } catch (error) {
        return new Response(`Render failed: ${error.message}`, { status: 500 });
      }
    }

    // Root info page
    if (url.pathname === '/') {
      return new Response(`
        <h1>Weather API for ESP32</h1>
        <p>Endpoints:</p>
        <ul>
          <li><a href="/weather.json">/weather.json</a> - Current weather data</li>
          <li><a href="/weather.png">/weather.png</a> - Rendered e-paper PNG (960x540 grayscale)</li>
        </ul>
        <p>Provider: ${env.WEATHER_PROVIDER || 'weatherapi'}</p>
        <p>Location: ${env.WEATHER_LOCATION || 'Manhattan'}</p>
      `, {
        headers: { 'Content-Type': 'text/html' }
      });
    }

    return new Response('Not Found', { status: 404 });
  },

  /**
   * Scheduled handler (cron trigger, every 3 minutes)
   *
   * Fetches weather data on every cycle so the `updated` timestamp stays
   * fresh (the device uses it for staleness detection). But the expensive
   * satori+resvg render is skipped if the weather data hasn't actually
   * changed since the last render — a simple JSON hash comparison.
   */
  async scheduled(event, env, ctx) {
    try {
      console.log('Scheduled weather fetch triggered');
      const weatherData = await fetchWeatherData(env);

      // Always store the JSON (refreshes `updated` timestamp + KV TTL).
      await storeWeatherData(env, weatherData);

      // Hash the weather-relevant fields (everything except `updated`,
      // which changes every cycle). Skip the render if unchanged.
      const dataForHash = { ...weatherData, updated: undefined };
      const newHash = simpleHash(JSON.stringify(dataForHash));
      const prevHash = await env.WEATHER_KV.get('render_hash', 'text');

      if (prevHash === newHash) {
        // Data unchanged — refresh the PNG's timestamp + TTL without
        // re-rendering. The device sees a fresh `X-Updated` header.
        console.log('Weather unchanged — refreshing timestamp only');
        await env.WEATHER_KV.put('render_updated', weatherData.updated, {
          expirationTtl: KV_TTL,
        });
        return;
      }

      console.log('Weather changed — rendering PNG...');
      const png = await renderWeatherPng(weatherData);
      await Promise.all([
        storeRenderedPng(env, png, weatherData.updated),
        env.WEATHER_KV.put('render_hash', newHash, { expirationTtl: KV_TTL }),
      ]);

      console.log(`PNG updated (${png.length} bytes, hash=${newHash})`);
    } catch (error) {
      console.error('Failed to update weather data:', error);
    }
  }
};

// ─── hashing ─────────────────────────────────────────────────────────────────

/** djb2 string hash — fast, deterministic, good enough for change detection. */
function simpleHash(str) {
  let h = 5381;
  for (let i = 0; i < str.length; i++) {
    h = ((h << 5) + h) ^ str.charCodeAt(i);
  }
  return (h >>> 0).toString(16);
}

// ─── KV helpers ──────────────────────────────────────────────────────────────

const KV_TTL = 3600; // 1 hour

/** Store weather JSON in KV. */
async function storeWeatherData(env, weatherData) {
  await env.WEATHER_KV.put('current', JSON.stringify(weatherData), {
    expirationTtl: KV_TTL,
  });
}

/** Store the rendered PNG and its timestamp in KV. */
async function storeRenderedPng(env, pngBytes, updated) {
  await Promise.all([
    env.WEATHER_KV.put('render_png', pngBytes, { expirationTtl: KV_TTL }),
    env.WEATHER_KV.put('render_updated', updated, { expirationTtl: KV_TTL }),
  ]);
}

// ─── weather data ────────────────────────────────────────────────────────────

async function fetchWeatherData(env) {
  const providerName = env.WEATHER_PROVIDER || 'weatherapi';
  const apiKey = env.WEATHER_API_KEY;
  const location = env.WEATHER_LOCATION || 'Manhattan';

  if (!apiKey) {
    throw new Error('WEATHER_API_KEY environment variable not set');
  }

  const provider = createProvider(providerName, apiKey, location);
  return await provider.fetch();
}

// ─── response helpers ────────────────────────────────────────────────────────

function jsonResponse(data, status = 200) {
  return new Response(JSON.stringify(data, null, 2), {
    status,
    headers: {
      'Content-Type': 'application/json',
      'Access-Control-Allow-Origin': '*',
      'Cache-Control': 'public, max-age=300',
    },
  });
}

function pngResponse(pngBytes, updated) {
  return new Response(pngBytes, {
    headers: {
      'Content-Type': 'image/png',
      'X-Updated': updated,
      'Cache-Control': 'public, max-age=300',
      'Access-Control-Allow-Origin': '*',
    },
  });
}
