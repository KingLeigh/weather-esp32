import { createProvider } from './providers/index.js';
import { renderWeatherPng } from './render.jsx';
import { adminPageHtml } from './admin.js';

/**
 * Cloudflare Worker — multi-location weather display renderer
 *
 * Fetches weather data for each configured location, renders 960x540
 * grayscale PNGs, caches them in KV, and serves them to ESP32 devices.
 *
 * Endpoints:
 *   GET /weather.png          — first location's PNG (backward compat)
 *   GET /weather/{zip}.png    — PNG for a specific zip code
 *   GET /admin                — location management page
 *   POST /admin               — add/remove locations, update settings
 *   GET /                     — info page
 */

// ─── constants ───────────────────────────────────────────────────────────────

const KV_TTL = 3600;             // 1 hour
const MAX_LOCATIONS = 5;
const MIN_POLL_MINUTES = 3;
const DEFAULT_POLL_MINUTES = 5;

// ─── request handler ─────────────────────────────────────────────────────────

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    // GET /weather.png — backward compat: serve first location
    if (url.pathname === '/weather.png') {
      const locations = await getLocations(env);
      if (locations.length === 0) {
        return new Response('No locations configured', { status: 404 });
      }
      return serveWeatherPng(env, locations[0]);
    }

    // GET /weather/{zip}.png
    const zipMatch = url.pathname.match(/^\/weather\/(\d+)\.png$/);
    if (zipMatch) {
      const zip = zipMatch[1];
      const locations = await getLocations(env);
      const loc = locations.find((l) => l.zip === zip);
      if (!loc) {
        return new Response(`Unknown zip code: ${zip}`, { status: 404 });
      }
      return serveWeatherPng(env, loc);
    }

    // GET /weather/{zip}.json — debug: returns the transformed weather data
    // the renderer would be given. Always live-fetched (no cache) so we see
    // the freshest provider output. Useful for diagnosing icon vs hourly
    // mismatches and other "what is the data actually saying" questions.
    const zipJsonMatch = url.pathname.match(/^\/weather\/(\d+)\.json$/);
    if (zipJsonMatch) {
      const zip = zipJsonMatch[1];
      const locations = await getLocations(env);
      const loc = locations.find((l) => l.zip === zip);
      if (!loc) {
        return new Response(`Unknown zip code: ${zip}`, { status: 404 });
      }
      try {
        const weatherData = await fetchWeatherData(env, `${loc.lat},${loc.lon}`);
        return jsonResponse(weatherData);
      } catch (error) {
        return new Response(`Fetch failed: ${error.message}`, { status: 500 });
      }
    }

    // GET /admin — admin page
    if (url.pathname === '/admin' && request.method === 'GET') {
      return new Response(adminPageHtml(), {
        headers: { 'Content-Type': 'text/html' },
      });
    }

    // POST /admin — admin actions
    if (url.pathname === '/admin' && request.method === 'POST') {
      return handleAdminPost(request, env);
    }

    // GET /admin/data — JSON data for admin page
    if (url.pathname === '/admin/data' && request.method === 'GET') {
      const locations = await getLocations(env);
      const pollInterval = await getPollInterval(env);
      return jsonResponse({ locations, pollInterval });
    }

    // GET /locations — slim public list used by the firmware setup flow.
    // Captive portal fetches this after the user's WiFi connects, to populate
    // the location dropdown (so users pick from registered zips, not type).
    if (url.pathname === '/locations' && request.method === 'GET') {
      const locations = await getLocations(env);
      const slim = locations.map((l) => ({ zip: l.zip, label: l.label }));
      return jsonResponse(slim);
    }

    // GET /firmware/check?current={N} — OTA version check.
    // The device sends its compiled-in FIRMWARE_VERSION as `current` and its
    // efuse MAC as the X-Device-Id header. We resolve a release channel (fast
    // for opted-in device ids, slow for everyone else), look up that channel's
    // latest version, and tell the device whether to update. The device acts
    // solely on the presence of a non-empty X-Firmware-Url header.
    if (url.pathname === '/firmware/check' && request.method === 'GET') {
      return handleFirmwareCheck(request, env, url);
    }

    // GET /firmware/{version}.bin — serve a firmware binary from KV.
    // (Stored in KV for now; migrate to R2 once it's enabled on the account —
    // see ROADMAP.md. The device-facing URL is identical either way.)
    const firmwareBinMatch = url.pathname.match(/^\/firmware\/(\d+)\.bin$/);
    if (firmwareBinMatch && request.method === 'GET') {
      const version = firmwareBinMatch[1];
      const bin = await env.WEATHER_KV.get(`firmware:bin:${version}`, 'arrayBuffer');
      if (!bin) {
        return new Response('Not Found', { status: 404 });
      }
      return binaryResponse(bin);
    }

    // Root info page
    if (url.pathname === '/') {
      const locations = await getLocations(env);
      const locationList = locations.length > 0
        ? locations.map((l) =>
            `<li><a href="/weather/${l.zip}.png">/weather/${l.zip}.png</a> — ${l.label}</li>`
          ).join('\n          ')
        : '<li>(no locations configured — visit <a href="/admin">/admin</a>)</li>';
      return new Response(`
        <h1>Weather Display API</h1>
        <p>Endpoints:</p>
        <ul>
          <li><a href="/weather.png">/weather.png</a> — default location</li>
          ${locationList}
        </ul>
        <p><a href="/admin">Admin page</a></p>
      `, {
        headers: { 'Content-Type': 'text/html' },
      });
    }

    return new Response('Not Found', { status: 404 });
  },

  // ─── scheduled handler ───────────────────────────────────────────────────

  async scheduled(event, env, ctx) {
    // Throttle: only run if enough time has passed since last poll.
    const pollInterval = await getPollInterval(env);
    const lastPoll = parseInt(await env.WEATHER_KV.get('last_poll_time') || '0');
    const now = Math.floor(Date.now() / 1000);

    if (now - lastPoll < pollInterval * 60) {
      return; // Too soon, skip.
    }
    await env.WEATHER_KV.put('last_poll_time', String(now));

    const locations = await getLocations(env);
    if (locations.length === 0) {
      console.log('No locations configured, skipping.');
      return;
    }

    console.log(`Polling ${locations.length} location(s)...`);

    for (const loc of locations) {
      try {
        const weatherData = await fetchWeatherData(env, `${loc.lat},${loc.lon}`);

        // Hash weather data (excluding timestamp) to detect changes.
        const dataForHash = { ...weatherData, updated: undefined };
        const newHash = simpleHash(JSON.stringify(dataForHash));
        const prevHash = await env.WEATHER_KV.get(`render_hash:${loc.zip}`);

        if (prevHash === newHash) {
          // Data unchanged — just refresh the timestamp.
          console.log(`[${loc.zip}] unchanged, refreshing timestamp`);
          await env.WEATHER_KV.put(`render_updated:${loc.zip}`, weatherData.updated, {
            expirationTtl: KV_TTL,
          });
          continue;
        }

        console.log(`[${loc.zip}] changed, rendering PNG...`);
        const png = await renderWeatherPng(weatherData);
        await Promise.all([
          env.WEATHER_KV.put(`render_png:${loc.zip}`, png, { expirationTtl: KV_TTL }),
          env.WEATHER_KV.put(`render_updated:${loc.zip}`, weatherData.updated, { expirationTtl: KV_TTL }),
          env.WEATHER_KV.put(`render_hash:${loc.zip}`, newHash, { expirationTtl: KV_TTL }),
        ]);
        console.log(`[${loc.zip}] PNG updated (${png.length} bytes)`);
      } catch (error) {
        console.error(`[${loc.zip}] failed:`, error.message);
      }
    }
  },
};

// ─── location management ─────────────────────────────────────────────────────

async function getLocations(env) {
  const stored = await env.WEATHER_KV.get('locations', 'json');
  if (stored && stored.length > 0) return stored;

  // Seed from env on first boot so existing single-location config works.
  const loc = env.WEATHER_LOCATION;
  if (!loc) return [];

  const [lat, lon] = loc.split(',').map((s) => s.trim());
  const seed = [{ zip: '00000', lat, lon, label: 'Default' }];
  await env.WEATHER_KV.put('locations', JSON.stringify(seed));
  return seed;
}

async function getPollInterval(env) {
  const stored = await env.WEATHER_KV.get('poll_interval');
  return stored ? parseInt(stored) : DEFAULT_POLL_MINUTES;
}

// ─── admin POST handler ──────────────────────────────────────────────────────

async function handleAdminPost(request, env) {
  let body;
  try {
    body = await request.json();
  } catch {
    return jsonResponse({ error: 'Invalid JSON' }, 400);
  }

  const { action } = body;

  if (action === 'add_location') {
    const { zip, lat, lon, label } = body;
    if (!zip || !lat || !lon) {
      return jsonResponse({ error: 'zip, lat, and lon are required' }, 400);
    }

    const locations = await getLocations(env);
    if (locations.length >= MAX_LOCATIONS) {
      return jsonResponse({ error: `Maximum ${MAX_LOCATIONS} locations allowed` }, 400);
    }
    if (locations.find((l) => l.zip === zip)) {
      return jsonResponse({ error: `Zip ${zip} already exists` }, 400);
    }

    locations.push({ zip, lat: String(lat), lon: String(lon), label: label || zip });
    await env.WEATHER_KV.put('locations', JSON.stringify(locations));
    return jsonResponse({ ok: true, locations });
  }

  if (action === 'remove_location') {
    const { zip } = body;
    if (!zip) return jsonResponse({ error: 'zip is required' }, 400);

    let locations = await getLocations(env);
    locations = locations.filter((l) => l.zip !== zip);
    await env.WEATHER_KV.put('locations', JSON.stringify(locations));

    // Clean up cached data for this location.
    await Promise.all([
      env.WEATHER_KV.delete(`render_png:${zip}`),
      env.WEATHER_KV.delete(`render_updated:${zip}`),
      env.WEATHER_KV.delete(`render_hash:${zip}`),
    ]);
    return jsonResponse({ ok: true, locations });
  }

  if (action === 'set_poll_interval') {
    const minutes = parseInt(body.minutes);
    if (isNaN(minutes) || minutes < MIN_POLL_MINUTES) {
      return jsonResponse({ error: `Minimum ${MIN_POLL_MINUTES} minutes` }, 400);
    }
    await env.WEATHER_KV.put('poll_interval', String(minutes));
    return jsonResponse({ ok: true, pollInterval: minutes });
  }

  return jsonResponse({ error: `Unknown action: ${action}` }, 400);
}

// ─── firmware OTA ────────────────────────────────────────────────────────────

/**
 * Resolve the OTA release channel for a device and report the latest version.
 *
 * Channel = "fast" if the device id is in the firmware:fast_devices KV array,
 * else "slow". The channel's latest version comes from
 * firmware:channel:{channel} (absent ⇒ 0). The device updates only when the
 * latest version M is greater than its reported current version N, which it
 * detects by the presence of a non-empty X-Firmware-Url header.
 */
async function handleFirmwareCheck(request, env, url) {
  const deviceId = request.headers.get('X-Device-Id');
  const current = parseInt(url.searchParams.get('current'), 10) || 0;

  const fastDevices = (await env.WEATHER_KV.get('firmware:fast_devices', 'json')) || [];
  const channel = deviceId && fastDevices.includes(deviceId) ? 'fast' : 'slow';

  const latest = parseInt(await env.WEATHER_KV.get(`firmware:channel:${channel}`, 'text'), 10) || 0;
  const update = latest > current;
  const updateUrl = update ? `/firmware/${latest}.bin` : null;

  const headers = {
    'Content-Type': 'application/json',
    'Access-Control-Allow-Origin': '*',
    'X-Firmware-Channel': channel,
    'X-Firmware-Latest': String(latest),
  };
  if (updateUrl) {
    headers['X-Firmware-Url'] = updateUrl;
  }

  return new Response(
    JSON.stringify(
      { channel, latest, current, update, url: updateUrl },
      null,
      2,
    ),
    { status: 200, headers },
  );
}

// ─── weather fetch + serve ───────────────────────────────────────────────────

async function serveWeatherPng(env, loc) {
  try {
    const [cachedPng, cachedUpdated, fwLatest] = await Promise.all([
      env.WEATHER_KV.get(`render_png:${loc.zip}`, 'arrayBuffer'),
      env.WEATHER_KV.get(`render_updated:${loc.zip}`, 'text'),
      // Latest available firmware (fast channel) — advertised on every weather
      // response so the device discovers OTA updates for free. Channels are
      // skipped for now: every device is treated as "fast".
      env.WEATHER_KV.get('firmware:channel:fast', 'text'),
    ]);

    const firmwareLatest = parseInt(fwLatest, 10) || 0;

    if (cachedPng) {
      return pngResponse(cachedPng, cachedUpdated || '', firmwareLatest);
    }

    // Cache miss — render on demand.
    const weatherData = await fetchWeatherData(env, `${loc.lat},${loc.lon}`);
    const png = await renderWeatherPng(weatherData);
    await Promise.all([
      env.WEATHER_KV.put(`render_png:${loc.zip}`, png, { expirationTtl: KV_TTL }),
      env.WEATHER_KV.put(`render_updated:${loc.zip}`, weatherData.updated, { expirationTtl: KV_TTL }),
    ]);
    return pngResponse(png, weatherData.updated, firmwareLatest);
  } catch (error) {
    return new Response(`Render failed: ${error.message}`, { status: 500 });
  }
}

async function fetchWeatherData(env, location) {
  const providerName = env.WEATHER_PROVIDER || 'openweathermap';
  const apiKey = env.WEATHER_API_KEY;

  if (!apiKey) {
    throw new Error('WEATHER_API_KEY environment variable not set');
  }

  const provider = createProvider(providerName, apiKey, location);
  return await provider.fetch();
}

// ─── helpers ─────────────────────────────────────────────────────────────────

/** djb2 string hash — fast, deterministic, good enough for change detection. */
function simpleHash(str) {
  let h = 5381;
  for (let i = 0; i < str.length; i++) {
    h = ((h << 5) + h) ^ str.charCodeAt(i);
  }
  return (h >>> 0).toString(16);
}

function jsonResponse(data, status = 200) {
  return new Response(JSON.stringify(data, null, 2), {
    status,
    headers: {
      'Content-Type': 'application/json',
      'Access-Control-Allow-Origin': '*',
    },
  });
}

function pngResponse(pngBytes, updated, firmwareLatest = 0) {
  return new Response(pngBytes, {
    headers: {
      'Content-Type': 'image/png',
      'X-Updated': updated,
      // Latest firmware version available to this device (fast channel). The
      // device compares it to its own FIRMWARE_VERSION and self-updates if newer
      // — free OTA discovery, no separate /firmware/check request needed.
      'X-Firmware-Latest': String(firmwareLatest),
      'Cache-Control': 'public, max-age=300',
      'Access-Control-Allow-Origin': '*',
    },
  });
}

function binaryResponse(body) {
  return new Response(body, {
    headers: {
      'Content-Type': 'application/octet-stream',
      'Cache-Control': 'public, max-age=3600',
      'Access-Control-Allow-Origin': '*',
    },
  });
}
