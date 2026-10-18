import { createProvider } from './providers/index.js';
import { fetchSnowReport, snowStateKeys } from './providers/open-meteo-snow.js';
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
// Snow reports refresh once a day, so their cached PNG must outlive a day
// (plus margin) or every device wake past the first hour would trigger an
// on-demand re-render.
const KV_TTL_SNOW = 26 * 3600;
// Staleness budgets advertised to the device via X-Stale-After (seconds):
// how old this location's data may get before the firmware should show the
// OLD status code. Weather re-renders every few minutes, so an hour means
// something is genuinely wrong; snow renders once a day by design, so only
// a missed daily render (26h+) is a problem. Firmware v9 ignores these
// headers (it hardcodes 60 min); v10+ uses them, falling back to the old
// behavior when they're absent.
const STALE_AFTER_WEATHER = 3600;
const STALE_AFTER_SNOW = 26 * 3600;
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
        const weatherData = await fetchWeatherForLocation(env, loc);
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
        // Snow locations refresh once per location-local day, not every poll:
        // their data source updates daily and their API budget is one
        // forecast call per day (plus rare archive re-syncs).
        if (locationType(loc) === 'snow') {
          const rendered = await env.WEATHER_KV.get(`snow_rendered:${loc.zip}`, 'json');
          if (rendered && rendered.date === localToday(rendered.timezone)) {
            continue;
          }
        }

        const weatherData = await fetchWeatherForLocation(env, loc);

        // Hash weather data to detect changes. Keep the DATE (not the minute)
        // of `updated` so a re-render fires when the local day rolls over —
        // date-specific statuses (e.g. the message CSV) depend on it — while
        // minute-level timestamp changes still don't force a re-render.
        const dataForHash = { ...weatherData, updated: (weatherData.updated || '').slice(0, 10) };
        const newHash = simpleHash(JSON.stringify(dataForHash));
        const prevHash = await env.WEATHER_KV.get(`render_hash:${loc.zip}`);

        const ttl = locationType(loc) === 'snow' ? KV_TTL_SNOW : KV_TTL;

        if (prevHash === newHash) {
          // Data unchanged — just refresh the timestamps. render_at advances
          // too: the data was re-verified current, so its age (X-Data-Age)
          // restarts from this poll, not from the last pixel change.
          console.log(`[${loc.zip}] unchanged, refreshing timestamp`);
          await Promise.all([
            env.WEATHER_KV.put(`render_updated:${loc.zip}`, weatherData.updated, {
              expirationTtl: ttl,
            }),
            env.WEATHER_KV.put(`render_at:${loc.zip}`, String(Date.now()), { expirationTtl: ttl }),
            ...markSnowRendered(env, loc, weatherData),
          ]);
          continue;
        }

        console.log(`[${loc.zip}] changed, rendering PNG...`);
        const png = await renderWeatherPng(weatherData, { location: loc.zip });
        await Promise.all([
          env.WEATHER_KV.put(`render_png:${loc.zip}`, png, { expirationTtl: ttl }),
          env.WEATHER_KV.put(`render_updated:${loc.zip}`, weatherData.updated, { expirationTtl: ttl }),
          env.WEATHER_KV.put(`render_at:${loc.zip}`, String(Date.now()), { expirationTtl: ttl }),
          env.WEATHER_KV.put(`render_hash:${loc.zip}`, newHash, { expirationTtl: ttl }),
          ...markSnowRendered(env, loc, weatherData),
        ]);
        console.log(`[${loc.zip}] PNG updated (${png.length} bytes)`);
      } catch (error) {
        console.error(`[${loc.zip}] failed:`, error.message);
      }
    }
  },
};

// ─── display types ───────────────────────────────────────────────────────────

/** 'weather' (default) or 'snow'. Entries predating the type field are weather. */
function locationType(loc) {
  return loc.type === 'snow' ? 'snow' : 'weather';
}

// Snow renders happen on the first poll after this hour, location-local —
// not at midnight. By 5am yesterday's analysis value has settled (Open-Meteo
// keeps refining recent days as observations assimilate), and the day's
// forecast is as fresh as possible when the display is actually looked at.
const SNOW_RENDER_LOCAL_HOUR = 5;

/** The current snow "render day" ('YYYY-MM-DD') in an IANA timezone: the
 * local date as of SNOW_RENDER_LOCAL_HOUR hours ago, so the date doesn't
 * roll over (and trigger a render) until 5am local. */
function localToday(timezone) {
  const shifted = new Date(Date.now() - SNOW_RENDER_LOCAL_HOUR * 3600 * 1000);
  try {
    return new Intl.DateTimeFormat('en-CA', { timeZone: timezone }).format(shifted);
  } catch {
    return new Intl.DateTimeFormat('en-CA', { timeZone: 'UTC' }).format(shifted);
  }
}

/** KV put (as an array to spread into Promise.all) recording that a snow
 * location has data for its current local day — the cron's daily gate. */
function markSnowRendered(env, loc, data) {
  if (locationType(loc) !== 'snow') return [];
  return [
    env.WEATHER_KV.put(
      `snow_rendered:${loc.zip}`,
      JSON.stringify({ date: data.date, timezone: data.timezone }),
      { expirationTtl: KV_TTL_SNOW },
    ),
  ];
}

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

    const entry = {
      zip,
      lat: String(lat),
      lon: String(lon),
      label: label || zip,
      showDewPoint: Boolean(body.showDewPoint),
    };
    // 'snow' entries render the snow-report display from Open-Meteo instead
    // of the weather display from OWM. Elevation (feet) steers Open-Meteo's
    // downscaling to the resort's altitude rather than the grid-cell mean.
    if (body.type === 'snow') {
      entry.type = 'snow';
      const elevation = parseInt(body.elevation_ft, 10);
      if (!Number.isFinite(elevation) || elevation <= 0) {
        return jsonResponse({ error: 'elevation_ft (feet) is required for snow locations' }, 400);
      }
      entry.elevation_ft = elevation;
    }
    locations.push(entry);
    await env.WEATHER_KV.put('locations', JSON.stringify(locations));
    return jsonResponse({ ok: true, locations });
  }

  if (action === 'update_location') {
    const { zip } = body;
    if (!zip) return jsonResponse({ error: 'zip is required' }, 400);

    const locations = await getLocations(env);
    const loc = locations.find((l) => l.zip === zip);
    if (!loc) return jsonResponse({ error: `Unknown zip: ${zip}` }, 400);

    if ('showDewPoint' in body) loc.showDewPoint = Boolean(body.showDewPoint);
    await env.WEATHER_KV.put('locations', JSON.stringify(locations));

    // Bust this location's render cache: config changes alter the render
    // without changing the weather data, so the hash-based skip would
    // otherwise keep serving the old PNG until the weather itself moved.
    await Promise.all([
      env.WEATHER_KV.delete(`render_png:${zip}`),
      env.WEATHER_KV.delete(`render_updated:${zip}`),
      env.WEATHER_KV.delete(`render_at:${zip}`),
      env.WEATHER_KV.delete(`render_hash:${zip}`),
    ]);
    return jsonResponse({ ok: true, locations });
  }

  if (action === 'remove_location') {
    const { zip } = body;
    if (!zip) return jsonResponse({ error: 'zip is required' }, 400);

    let locations = await getLocations(env);
    locations = locations.filter((l) => l.zip !== zip);
    await env.WEATHER_KV.put('locations', JSON.stringify(locations));

    // Clean up cached data for this location (snow state keys are harmless
    // no-op deletes for weather locations).
    await Promise.all([
      env.WEATHER_KV.delete(`render_png:${zip}`),
      env.WEATHER_KV.delete(`render_updated:${zip}`),
      env.WEATHER_KV.delete(`render_at:${zip}`),
      env.WEATHER_KV.delete(`render_hash:${zip}`),
      ...snowStateKeys(zip).map((k) => env.WEATHER_KV.delete(k)),
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

// ─── weather fetch + serve ───────────────────────────────────────────────────

async function serveWeatherPng(env, loc) {
  const staleAfter = locationType(loc) === 'snow' ? STALE_AFTER_SNOW : STALE_AFTER_WEATHER;
  try {
    const [cachedPng, cachedUpdated, cachedRenderAt, fwLatest] = await Promise.all([
      env.WEATHER_KV.get(`render_png:${loc.zip}`, 'arrayBuffer'),
      env.WEATHER_KV.get(`render_updated:${loc.zip}`, 'text'),
      env.WEATHER_KV.get(`render_at:${loc.zip}`, 'text'),
      // Latest available firmware — advertised on every weather response so the
      // device discovers OTA updates for free (it self-updates when this exceeds
      // its own FIRMWARE_VERSION).
      env.WEATHER_KV.get('firmware:latest', 'text'),
    ]);

    const firmwareLatest = parseInt(fwLatest, 10) || 0;

    if (cachedPng) {
      // X-Data-Age from the stored render epoch; null (header omitted, firmware
      // falls back to X-Updated math) if the epoch predates this feature.
      const renderAt = parseInt(cachedRenderAt, 10);
      const ageSec = Number.isFinite(renderAt) && renderAt > 0
        ? Math.max(0, Math.round((Date.now() - renderAt) / 1000))
        : null;
      return pngResponse(cachedPng, cachedUpdated || '', firmwareLatest, ageSec, staleAfter);
    }

    // Cache miss — render on demand.
    const weatherData = await fetchWeatherForLocation(env, loc);
    const png = await renderWeatherPng(weatherData, { location: loc.zip });
    const ttl = locationType(loc) === 'snow' ? KV_TTL_SNOW : KV_TTL;
    await Promise.all([
      env.WEATHER_KV.put(`render_png:${loc.zip}`, png, { expirationTtl: ttl }),
      env.WEATHER_KV.put(`render_updated:${loc.zip}`, weatherData.updated, { expirationTtl: ttl }),
      env.WEATHER_KV.put(`render_at:${loc.zip}`, String(Date.now()), { expirationTtl: ttl }),
      ...markSnowRendered(env, loc, weatherData),
    ]);
    return pngResponse(png, weatherData.updated, firmwareLatest, 0, staleAfter);
  } catch (error) {
    return new Response(`Render failed: ${error.message}`, { status: 500 });
  }
}

// Fetch display data for a location entry, dispatching on its type:
// 'snow' locations go to the Open-Meteo snow provider; 'weather' locations
// (the default) to the configured weather provider (OWM).
// `showDewPoint` gates the dew-point display: the provider always emits
// `dew_point`, and stripping it here (a) makes the layout render the plain
// big-UV hero, and (b) keeps it out of the render hash, so locations without
// the feature don't re-render on dew-point-only changes.
async function fetchWeatherForLocation(env, loc) {
  if (locationType(loc) === 'snow') {
    return fetchSnowReport(env, loc);
  }
  const weatherData = await fetchWeatherData(env, `${loc.lat},${loc.lon}`);
  if (!loc.showDewPoint) delete weatherData.dew_point;
  return weatherData;
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

function pngResponse(pngBytes, updated, firmwareLatest = 0, ageSec = null, staleAfterSec = null) {
  const headers = {
    'Content-Type': 'image/png',
    // Render time as location-local wall clock. Kept for compatibility and
    // human readability (firmware v9 staleness; the debug screen's data-time
    // line) — v10+ prefers the age/budget headers below, which are immune to
    // the device-clock-vs-location timezone mismatch.
    'X-Updated': updated,
    // Latest available firmware version. The device compares it to its own
    // FIRMWARE_VERSION and self-updates if newer — free OTA discovery.
    'X-Firmware-Latest': String(firmwareLatest),
    'Cache-Control': 'public, max-age=300',
    'Access-Control-Allow-Origin': '*',
  };
  // Seconds since this PNG's data was rendered/verified, computed server-side
  // so no device clock is involved. Omitted when unknown (pre-feature cache).
  if (ageSec != null) headers['X-Data-Age'] = String(ageSec);
  // This location's staleness budget: the firmware shows OLD only when
  // X-Data-Age exceeds it (60 min weather, 26 h daily-cadence snow).
  if (staleAfterSec != null) headers['X-Stale-After'] = String(staleAfterSec);
  return new Response(pngBytes, { headers });
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
