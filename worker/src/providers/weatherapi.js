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
    // Use max of rain or snow chance to capture all precipitation types
    const precipitation = [];
    for (let i = 0; i < 12; i++) {
      const hourIndex = (currentHour + i) % 24;
      const hourData = hourly[hourIndex];
      const precipChance = Math.max(hourData.chance_of_rain || 0, hourData.chance_of_snow || 0);
      precipitation.push(precipChance);
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

    // Get moon data from astronomy
    const astro = today.astro;

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
      moon: {
        illumination: parseInt(astro.moon_illumination),
        phase: astro.moon_phase
      },
      is_day: current.is_day === 1,
      // Return Eastern Time formatted as YYYY-MM-DDTHH:MM:SS
      updated: (() => {
        const formatter = new Intl.DateTimeFormat('en-US', {
          timeZone: 'America/New_York',
          year: 'numeric',
          month: '2-digit',
          day: '2-digit',
          hour: '2-digit',
          minute: '2-digit',
          second: '2-digit',
          hour12: false
        });
        const parts = formatter.formatToParts(new Date());
        const get = (type) => parts.find(p => p.type === type)?.value || '00';
        return `${get('year')}-${get('month')}-${get('day')}T${get('hour')}:${get('minute')}:${get('second')}`;
      })()
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

    // Fog/Mist
    if ([1030, 1135, 1147].includes(code)) {
      return 'fog';
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
      return 'thunderstorm';
    }

    // Default to partly cloudy
    return 'partly_cloudy';
  }
}
