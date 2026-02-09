/**
 * Quick test script to verify WeatherAPI integration
 *
 * Usage:
 *   WEATHER_API_KEY=your_key_here node test.js
 *
 * Or create a .env file (gitignored) with:
 *   WEATHER_API_KEY=your_key_here
 *   WEATHER_LOCATION=40.739214,-73.987265
 */

import { createProvider } from './src/providers/index.js';
import { readFileSync } from 'fs';

// Try to load from .env file if it exists
try {
  const envFile = readFileSync('.env', 'utf8');
  envFile.split('\n').forEach(line => {
    const [key, value] = line.split('=');
    if (key && value) process.env[key.trim()] = value.trim();
  });
} catch (e) {
  // .env file doesn't exist, that's ok
}

const API_KEY = process.env.WEATHER_API_KEY;
const LOCATION = process.env.WEATHER_LOCATION || '40.739214,-73.987265';

if (!API_KEY) {
  console.error('❌ Error: WEATHER_API_KEY environment variable not set');
  console.error('\nUsage: WEATHER_API_KEY=your_key node test.js');
  process.exit(1);
}

async function test() {
  console.log('Testing WeatherAPI.com provider...\n');

  try {
    const provider = createProvider('weatherapi', API_KEY, LOCATION);
    console.log(`Fetching weather for: ${LOCATION}`);

    const weatherData = await provider.fetch();

    console.log('\n✅ Success! Weather data:');
    console.log(JSON.stringify(weatherData, null, 2));

    // Validate data structure
    console.log('\n📊 Validation:');
    console.log(`  Temperature: ${weatherData.temperature.current}°F (${weatherData.temperature.low}°F - ${weatherData.temperature.high}°F)`);
    console.log(`  Weather: ${weatherData.weather}`);
    console.log(`  Precipitation hours: ${weatherData.precipitation.length}`);
    console.log(`  UV: ${weatherData.uv.current} (high: ${weatherData.uv.high})`);
    console.log(`  Updated: ${weatherData.updated}`);

  } catch (error) {
    console.error('❌ Error:', error.message);
    process.exit(1);
  }
}

test();
