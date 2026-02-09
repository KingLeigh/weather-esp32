/**
 * Base WeatherProvider interface
 * All weather API providers must implement this interface
 */
export class WeatherProvider {
  constructor(apiKey, location) {
    if (this.constructor === WeatherProvider) {
      throw new Error("WeatherProvider is abstract and cannot be instantiated directly");
    }
    this.apiKey = apiKey;
    this.location = location;
  }

  /**
   * Fetch weather data from the provider's API
   * @returns {Promise<Object>} Raw API response
   */
  async fetchRaw() {
    throw new Error("fetchRaw() must be implemented by subclass");
  }

  /**
   * Transform raw API response to our standard format
   * @param {Object} rawData - Raw API response
   * @returns {Object} Transformed weather data in standard format:
   * {
   *   temperature: { current: number, high: number, low: number },
   *   weather: string (sunny|cloudy|partly_cloudy|rainy|snowy),
   *   precipitation: number[] (12 hourly values, 0-100),
   *   uv: { current: number, high: number },
   *   updated: string (ISO 8601 timestamp)
   * }
   */
  transform(rawData) {
    throw new Error("transform() must be implemented by subclass");
  }

  /**
   * Convenience method: fetch and transform in one call
   * @returns {Promise<Object>} Transformed weather data
   */
  async fetch() {
    const rawData = await this.fetchRaw();
    return this.transform(rawData);
  }
}
