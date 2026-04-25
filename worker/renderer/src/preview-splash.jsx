// Local-only renderer for the firmware splash screen. Writes splash.svg (for
// quick browser-based iteration) and splash.png (the artifact that gets baked
// into the firmware build).
//
// Run with: `npm run preview:splash`

import { readFile, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import satori from 'satori';
import { Resvg } from '@resvg/resvg-js';

import { SplashFrame } from './splash.jsx';
import { rgbaToGrayscalePng } from './png-encode.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');
const FONTS_DIR = join(ROOT, 'fonts');
const WEATHER_ICONS_TTF = join(
  ROOT,
  'node_modules',
  'weathericons',
  'font',
  'weathericons-regular-webfont.ttf',
);

const PNG_OUTPUT = join(ROOT, 'splash.png');
const SVG_OUTPUT = join(ROOT, 'splash.svg');

const WIDTH = 960;
const HEIGHT = 540;

const [regular, bold, weatherIcons] = await Promise.all([
  readFile(join(FONTS_DIR, 'FiraSans-Regular.ttf')),
  readFile(join(FONTS_DIR, 'FiraSans-Bold.ttf')),
  readFile(WEATHER_ICONS_TTF),
]);

const fonts = [
  { name: 'FiraSans', data: regular, weight: 400, style: 'normal' },
  { name: 'FiraSans', data: bold, weight: 700, style: 'normal' },
  { name: 'WeatherIcons', data: weatherIcons, weight: 400, style: 'normal' },
];

const started = Date.now();

const svg = await satori(<SplashFrame />, { width: WIDTH, height: HEIGHT, fonts });

const resvg = new Resvg(svg, {
  background: 'white',
  fitTo: { mode: 'width', value: WIDTH },
});
const rendered = resvg.render();
const png = rgbaToGrayscalePng(rendered.width, rendered.height, rendered.pixels);

await Promise.all([
  writeFile(SVG_OUTPUT, svg),
  writeFile(PNG_OUTPUT, png),
]);

const elapsed = Date.now() - started;
console.log(`Rendered splash in ${elapsed}ms`);
console.log(`  ${SVG_OUTPUT} (${svg.length} bytes)`);
console.log(`  ${PNG_OUTPUT} (${png.length} bytes grayscale)`);
