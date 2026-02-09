/**
 * Quick test script to verify WeatherAPI integration
 * Run: node test.js
 */

import { createProvider } from './src/providers/index.js';

const API_KEY = 'f617eacc951846d2a00154705260902';
const LOCATION = 'Manhattan';

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
