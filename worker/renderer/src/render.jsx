// Pure render function: (WeatherData) -> PNG bytes.
//
// This file intentionally has no I/O other than loading fonts from disk on
// first call. Step 2 will import `renderWeather` from a Cloudflare Worker and
// drop in an `@resvg/resvg-wasm`-based replacement for the Resvg import below.
// The Satori call and the layout are portable as-is.

import { readFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import satori from 'satori';
import { Resvg } from '@resvg/resvg-js';

import { WeatherFrame } from './layout.jsx';

const __dirname = dirname(fileURLToPath(import.meta.url));
const FONTS_DIR = join(__dirname, '..', 'fonts');
const WEATHER_ICONS_TTF = join(
  __dirname,
  '..',
  'node_modules',
  'weathericons',
  'font',
  'weathericons-regular-webfont.ttf',
);

const WIDTH = 960;
const HEIGHT = 540;

let cachedFonts = null;

async function loadFonts() {
  if (cachedFonts) return cachedFonts;
  const [regular, bold, weatherIcons] = await Promise.all([
    readFile(join(FONTS_DIR, 'FiraSans-Regular.ttf')),
    readFile(join(FONTS_DIR, 'FiraSans-Bold.ttf')),
    readFile(WEATHER_ICONS_TTF),
  ]);
  cachedFonts = [
    { name: 'FiraSans', data: regular, weight: 400, style: 'normal' },
    { name: 'FiraSans', data: bold, weight: 700, style: 'normal' },
    { name: 'WeatherIcons', data: weatherIcons, weight: 400, style: 'normal' },
  ];
  return cachedFonts;
}

/**
 * Render a weather frame to raw RGBA pixels and the intermediate SVG.
 * Used by preview.js (which converts to grayscale PNG and writes the SVG for
 * browser-based iteration) and will be used by the Cloudflare Worker (which
 * will swap Resvg for resvg-wasm).
 *
 * @param {object} data - Normalized WeatherData (matches the Worker JSON shape).
 * @returns {Promise<{svg: string, width: number, height: number, pixels: Uint8Array}>}
 */
export async function renderSvg(data) {
  const fonts = await loadFonts();

  const svg = await satori(<WeatherFrame data={data} />, {
    width: WIDTH,
    height: HEIGHT,
    fonts,
  });

  const resvg = new Resvg(svg, {
    background: 'white',
    fitTo: { mode: 'width', value: WIDTH },
  });

  const rendered = resvg.render();
  return {
    svg,
    width: rendered.width,
    height: rendered.height,
    pixels: rendered.pixels, // Uint8Array, RGBA, row-major
  };
}
