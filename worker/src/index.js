import { createProvider } from './providers/index.js';

/**
 * Cloudflare Worker for weather data
 *
 * Handles:
 * 1. HTTP GET /weather.json - Serve cached weather data to ESP32
 * 2. Scheduled cron - Fetch fresh weather data every 15 minutes
 */

export default {
  /**
   * HTTP request handler
   * Serves cached weather data from KV storage
   */
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    // Handle /weather.json endpoint
    if (url.pathname === '/weather.json') {
      // Get cached data from KV
      const cachedData = await env.WEATHER_KV.get('current', 'json');

      if (!cachedData) {
        // No cached data yet, fetch fresh data
        try {
          const weatherData = await fetchWeatherData(env);
          return jsonResponse(weatherData);
        } catch (error) {
          return jsonResponse({ error: 'Failed to fetch weather data' }, 500);
        }
      }

      return jsonResponse(cachedData);
    }

    // Handle root path - show info
    if (url.pathname === '/') {
      return new Response(`
        <h1>Weather API for ESP32</h1>
        <p>Endpoints:</p>
        <ul>
          <li><a href="/weather.json">/weather.json</a> - Current weather data</li>
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
   * Scheduled handler (cron trigger)
   * Fetches fresh weather data and updates KV cache
   */
  async scheduled(event, env, ctx) {
    try {
      console.log('Scheduled weather fetch triggered');
      const weatherData = await fetchWeatherData(env);

      // Store in KV with 1-hour expiration
      await env.WEATHER_KV.put('current', JSON.stringify(weatherData), {
        expirationTtl: 3600 // 1 hour
      });

      console.log('Weather data updated successfully');
    } catch (error) {
      console.error('Failed to fetch weather data:', error);
    }
  }
};

/**
 * Fetch weather data from configured provider
 */
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

/**
 * Helper to create JSON response with CORS headers
 */
function jsonResponse(data, status = 200) {
  return new Response(JSON.stringify(data, null, 2), {
    status,
    headers: {
      'Content-Type': 'application/json',
      'Access-Control-Allow-Origin': '*', // Allow ESP32 to fetch
      'Cache-Control': 'public, max-age=300' // Cache for 5 minutes
    }
  });
}
