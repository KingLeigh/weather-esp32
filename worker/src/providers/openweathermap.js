import { WeatherProvider } from './base.js';

/**
 * OpenWeatherMap One Call 3.0 provider implementation
 * API docs: https://openweathermap.org/api/one-call-3
 *
 * Requires: Subscribe to One Call 3.0 at https://openweathermap.org/api/one-call-3
 * Free tier: 1,000 calls/day (set daily limit in dashboard to avoid charges)
 */
export class OpenWeatherMapProvider extends WeatherProvider {
  constructor(apiKey, location) {
    super(apiKey, location);
    this.baseUrl = 'https://api.openweathermap.org/data/3.0/onecall';
  }

  async fetchRaw() {
    // Location is expected as "lat,lon" string
    const [lat, lon] = this.location.split(',').map(s => s.trim());

    const url = `${this.baseUrl}?lat=${lat}&lon=${lon}&appid=${this.apiKey}&units=imperial&exclude=minutely,alerts`;

    const response = await fetch(url);
    if (!response.ok) {
      const body = await response.text();
      throw new Error(`OpenWeatherMap error: ${response.status} ${response.statusText} - ${body}`);
    }

    return await response.json();
  }

  transform(data) {
    const current = data.current;
    const hourly = data.hourly;  // 48 hours
    const daily = data.daily[0]; // Today

    // Extract 24 hours of rain and snow probability separately.
    // OWM gives a single `pop` (0-1) for overall precipitation probability.
    // We attribute it to rain or snow based on which volume is present for
    // that hour. If both are present, both get the full pop value (the chart
    // will overlay snow on top of rain).
    const rain_chance = [];
    const snow_chance = [];
    const hourly_temp = [];
    let totalRainMm = 0;
    let totalSnowMm = 0;

    for (let i = 0; i < 24 && i < hourly.length; i++) {
      const h = hourly[i];
      const pop = Math.round((h.pop || 0) * 100);
      const rainVol = h.rain?.['1h'] || 0;
      const snowVol = h.snow?.['1h'] || 0;

      if (snowVol > 0 && rainVol === 0) {
        rain_chance.push(0);
        snow_chance.push(pop);
      } else if (rainVol > 0 && snowVol === 0) {
        rain_chance.push(pop);
        snow_chance.push(0);
      } else if (rainVol > 0 && snowVol > 0) {
        // Both present — show both at full probability.
        rain_chance.push(pop);
        snow_chance.push(pop);
      } else {
        // No volume data but pop > 0 — default to rain.
        rain_chance.push(pop);
        snow_chance.push(0);
      }

      totalRainMm += rainVol;
      totalSnowMm += snowVol;
      hourly_temp.push(Math.round(h.temp));
    }

    // Map weather condition to our icon types
    const weatherIcon = this._mapConditionToIcon(current.weather[0]?.id || 800);

    // High/low from daily forecast
    const high = Math.round(daily.temp.max);
    const low = Math.round(daily.temp.min);

    // UV: current from current data, high from daily
    const uvCurrent = Math.round(current.uvi || 0);
    const uvHigh = Math.round(daily.uvi || 0);

    // Sunrise/sunset - convert unix timestamps to formatted time
    const sunrise = this._formatTime(daily.sunrise, data.timezone_offset);
    const sunset = this._formatTime(daily.sunset, data.timezone_offset);

    // Moon phase: OWM returns 0-1 float, map to phase name
    const moonPhase = this._mapMoonPhase(daily.moon_phase);

    // Is it daytime?
    const now = current.dt;
    const isDay = now >= daily.sunrise && now < daily.sunset;

    return {
      temperature: {
        current: Math.round(current.temp),
        high: high,
        low: low
      },
      weather: weatherIcon,
      rain_chance,
      snow_chance,
      hourly_temp,
      rain_in: Math.round(totalRainMm / 25.4 * 100) / 100,  // next-24h total, mm → inches
      snow_in: Math.round(totalSnowMm / 25.4 * 100) / 100,
      uv: {
        current: uvCurrent,
        high: uvHigh
      },
      sun: {
        sunrise: sunrise,
        sunset: sunset
      },
      moon: {
        illumination: this._estimateIllumination(daily.moon_phase),
        phase: moonPhase
      },
      is_day: isDay,
      updated: (() => {
        const formatter = new Intl.DateTimeFormat('en-US', {
          timeZone: data.timezone,
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
   * Format unix timestamp to "HH:MM AM/PM" using the location's timezone offset
   */
  _formatTime(unixTimestamp, timezoneOffset) {
    // Create date adjusted for timezone offset
    const date = new Date((unixTimestamp + timezoneOffset) * 1000);
    let hours = date.getUTCHours();
    const minutes = date.getUTCMinutes().toString().padStart(2, '0');
    const ampm = hours >= 12 ? 'PM' : 'AM';
    hours = hours % 12;
    if (hours === 0) hours = 12;
    return `${hours}:${minutes} ${ampm}`;
  }

  /**
   * Map OWM moon_phase (0-1) to phase name string
   * 0/1 = new, 0.25 = first quarter, 0.5 = full, 0.75 = last quarter
   */
  _mapMoonPhase(phase) {
    if (phase === 0 || phase === 1) return 'New Moon';
    if (phase < 0.25) return 'Waxing Crescent';
    if (phase === 0.25) return 'First Quarter';
    if (phase < 0.5) return 'Waxing Gibbous';
    if (phase === 0.5) return 'Full Moon';
    if (phase < 0.75) return 'Waning Gibbous';
    if (phase === 0.75) return 'Last Quarter';
    return 'Waning Crescent';
  }

  /**
   * Estimate moon illumination percentage from phase (0-1)
   * 0 = new (0%), 0.5 = full (100%)
   */
  _estimateIllumination(phase) {
    // Illumination follows a cosine curve: 0 at new, 100 at full
    return Math.round((1 - Math.cos(phase * 2 * Math.PI)) / 2 * 100);
  }

  /**
   * Map OpenWeatherMap condition IDs to our icon types.
   * Full list: https://openweathermap.org/weather-conditions
   */
  _mapConditionToIcon(id) {
    // Thunderstorm (2xx)
    if (id >= 200 && id < 300) return 'thunderstorm';

    // Drizzle (3xx)
    if (id >= 300 && id < 400) return 'drizzle';

    // Rain (5xx)
    if (id === 511) return 'sleet';                        // freezing rain
    if (id >= 500 && id < 600) return 'rainy';

    // Snow (6xx)
    if (id >= 611 && id <= 616) return 'sleet';            // sleet / shower sleet
    if (id >= 600 && id < 700) return 'snowy';

    // Atmosphere (7xx)
    if (id === 701 || id === 741) return 'fog';            // mist, fog
    if (id === 711) return 'smoke';                        // smoke
    if (id === 721) return 'haze';                         // haze
    if (id >= 700 && id < 800) return 'fog';               // dust, sand, ash, squall, tornado

    // Clear (800)
    if (id === 800) return 'sunny';

    // Clouds (80x)
    if (id === 801 || id === 802) return 'partly_cloudy';  // few/scattered clouds
    if (id === 803 || id === 804) return 'cloudy';         // broken/overcast

    return 'partly_cloudy';
  }
}
