import { WeatherProvider } from './base.js';

/**
 * WeatherAPI.com provider implementation
 * API docs: https://www.weatherapi.com/docs/
 */
export class WeatherAPIProvider extends WeatherProvider {
  constructor(apiKey, location) {
    super(apiKey, location);
    this.baseUrl = 'https://api.weatherapi.com/v1';
  }

  async fetchRaw() {
    // Fetch current + forecast data (includes UV and hourly precipitation)
    const url = `${this.baseUrl}/forecast.json?key=${this.apiKey}&q=${encodeURIComponent(this.location)}&days=1&aqi=no&alerts=no`;

    const response = await fetch(url);
    if (!response.ok) {
      throw new Error(`WeatherAPI error: ${response.status} ${response.statusText}`);
    }

    return await response.json();
  }

  transform(data) {
    const current = data.current;
    const today = data.forecast.forecastday[0];
    const hourly = today.hour;

    // Get current hour index
    const now = new Date();
    const currentHour = now.getHours();

    // Extract 12 hours of precipitation data starting from current hour
    const precipitation = [];
    for (let i = 0; i < 12; i++) {
      const hourIndex = (currentHour + i) % 24;
      const hourData = hourly[hourIndex];
      precipitation.push(hourData.chance_of_rain);
    }

    // Map weather condition to our icon types
    const weatherIcon = this._mapConditionToIcon(current.condition.code, current.is_day);

    // Calculate high/low from hourly forecast (more accurate than day forecast)
    const temps = hourly.map(h => h.temp_f);
    const high = Math.round(Math.max(...temps));
    const low = Math.round(Math.min(...temps));

    // Get max UV for the day
    const uvValues = hourly.map(h => h.uv);
    const uvHigh = Math.round(Math.max(...uvValues));

    return {
      temperature: {
        current: Math.round(current.temp_f),
        high: high,
        low: low
      },
      weather: weatherIcon,
      precipitation: precipitation,
      uv: {
        current: Math.round(current.uv),
        high: uvHigh
      },
      updated: new Date().toISOString()
    };
  }

  /**
   * Map WeatherAPI condition codes to our icon types
   * Full list: https://www.weatherapi.com/docs/weather_conditions.json
   */
  _mapConditionToIcon(code, isDay) {
    // Clear/Sunny
    if (code === 1000) {
      return 'sunny';
    }

    // Partly cloudy
    if (code === 1003) {
      return 'partly_cloudy';
    }

    // Cloudy/Overcast
    if ([1006, 1009].includes(code)) {
      return 'cloudy';
    }

    // Rain (light to heavy, including drizzle and showers)
    if ([1063, 1150, 1153, 1168, 1171, 1180, 1183, 1186, 1189, 1192, 1195, 1198, 1201, 1240, 1243, 1246].includes(code)) {
      return 'rainy';
    }

    // Snow (light to heavy, including sleet and ice pellets)
    if ([1066, 1069, 1072, 1114, 1117, 1204, 1207, 1210, 1213, 1216, 1219, 1222, 1225, 1237, 1249, 1252, 1255, 1258, 1261, 1264].includes(code)) {
      return 'snowy';
    }

    // Thunderstorm
    if ([1087, 1273, 1276, 1279, 1282].includes(code)) {
      return 'rainy'; // Use rainy icon for storms
    }

    // Fog/Mist
    if ([1030, 1135, 1147].includes(code)) {
      return 'cloudy';
    }

    // Default to partly cloudy
    return 'partly_cloudy';
  }
}
