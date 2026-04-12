import { createProvider } from './providers/index.js';
import { renderWeatherPng } from './render.jsx';

/**
 * Cloudflare Worker — weather display renderer
 *
 * Fetches weather data from the configured provider, renders a 960x540
 * grayscale PNG via satori + resvg-wasm, caches it in KV, and serves it
 * to the ESP32 e-paper display.
 *
 * Endpoints:
 *   GET /weather.png  — cached rendered PNG with X-Updated header
 *   GET /             — info page
 *
 * Cron (every 3 minutes):
 *   Fetches weather, re-renders PNG only when data has changed.
 */

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    if (url.pathname === '/weather.png') {
      try {
        const [cachedPng, cachedUpdated] = await Promise.all([
          env.WEATHER_KV.get('render_png', 'arrayBuffer'),
          env.WEATHER_KV.get('render_updated', 'text'),
        ]);

        if (cachedPng) {
          return pngResponse(cachedPng, cachedUpdated || '');
        }

        // Cache miss — fetch, render, cache, and serve.
        const weatherData = await fetchWeatherData(env);
        const png = await renderWeatherPng(weatherData);
        await storeRenderedPng(env, png, weatherData.updated);

        return pngResponse(png, weatherData.updated);
      } catch (error) {
        return new Response(`Render failed: ${error.message}`, { status: 500 });
      }
    }

    if (url.pathname === '/') {
      return new Response(`
        <h1>Weather Display API</h1>
        <p>Endpoints:</p>
        <ul>
          <li><a href="/weather.png">/weather.png</a> - Rendered e-paper PNG (960x540 grayscale)</li>
        </ul>
        <p>Provider: ${env.WEATHER_PROVIDER || 'openweathermap'}</p>
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
   * Fetches weather data on every cycle to keep the `updated` timestamp
   * fresh. The expensive satori+resvg render is skipped if the weather
   * data hasn't changed since the last render.
   */
  async scheduled(event, env, ctx) {
    try {
      console.log('Scheduled weather fetch triggered');
      const weatherData = await fetchWeatherData(env);

      // Hash the weather-relevant fields (excluding `updated`, which
      // changes every cycle). Skip the render if unchanged.
      const dataForHash = { ...weatherData, updated: undefined };
      const newHash = simpleHash(JSON.stringify(dataForHash));
      const prevHash = await env.WEATHER_KV.get('render_hash', 'text');

      if (prevHash === newHash) {
        // Data unchanged — just refresh the timestamp + TTL.
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

/** Store the rendered PNG and its timestamp in KV. */
async function storeRenderedPng(env, pngBytes, updated) {
  await Promise.all([
    env.WEATHER_KV.put('render_png', pngBytes, { expirationTtl: KV_TTL }),
    env.WEATHER_KV.put('render_updated', updated, { expirationTtl: KV_TTL }),
  ]);
}

// ─── weather data ────────────────────────────────────────────────────────────

async function fetchWeatherData(env) {
  const providerName = env.WEATHER_PROVIDER || 'openweathermap';
  const apiKey = env.WEATHER_API_KEY;
  const location = env.WEATHER_LOCATION || 'Manhattan';

  if (!apiKey) {
    throw new Error('WEATHER_API_KEY environment variable not set');
  }

  const provider = createProvider(providerName, apiKey, location);
  return await provider.fetch();
}

// ─── response helpers ────────────────────────────────────────────────────────

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
