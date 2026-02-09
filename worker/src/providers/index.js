import { WeatherAPIProvider } from './weatherapi.js';

/**
 * Weather provider factory
 * Add new providers here to make them available
 */
const PROVIDERS = {
  'weatherapi': WeatherAPIProvider,
  // Add more providers here in the future:
  // 'openweathermap': OpenWeatherMapProvider,
  // 'weathergov': WeatherGovProvider,
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
