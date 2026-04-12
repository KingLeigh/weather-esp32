import { OpenWeatherMapProvider } from './openweathermap.js';

/**
 * Weather provider factory
 *
 * To add a new provider:
 *   1. Create src/providers/yourprovider.js extending WeatherProvider (base.js)
 *   2. Implement fetchRaw() and transform() — output must include rain_chance[],
 *      snow_chance[], and all other fields documented in the Worker README
 *   3. Register it in the PROVIDERS map below
 *   4. Set WEATHER_PROVIDER = "yourprovider" in wrangler.toml
 */
const PROVIDERS = {
  'openweathermap': OpenWeatherMapProvider,
};

/**
 * Create a weather provider instance
 * @param {string} providerName - Name of the provider (e.g., 'weatherapi')
 * @param {string} apiKey - API key for the provider
 * @param {string} location - Location to fetch weather for
 * @returns {WeatherProvider} Provider instance
 */
export function createProvider(providerName, apiKey, location) {
  const ProviderClass = PROVIDERS[providerName];

  if (!ProviderClass) {
    throw new Error(`Unknown weather provider: ${providerName}. Available: ${Object.keys(PROVIDERS).join(', ')}`);
  }

  return new ProviderClass(apiKey, location);
}

/**
 * Get list of available provider names
 */
export function getAvailableProviders() {
  return Object.keys(PROVIDERS);
}
