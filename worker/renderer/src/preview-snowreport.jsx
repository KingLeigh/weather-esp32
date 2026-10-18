// Local preview for the snow-report display type. The layout itself lives in
// snow-layout.jsx (shared with the Worker, like layout.jsx is for weather);
// this script just feeds it the checked-in fixture and writes PNG + SVG.
//
// snow-sample.json was built from real Open-Meteo Breckenridge data (archive
// API, 2008-2024) with "today" simulated as 2024-01-15 — mid-storm-cycle in
// the best recent season.
//
// Run with: `npm run preview:snowreport`

import { readFile, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import satori from 'satori';
import { Resvg } from '@resvg/resvg-js';

import { SnowFrame } from './snow-layout.jsx';
import { rgbaToGrayscalePng } from './png-encode.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');
const FONTS_DIR = join(ROOT, 'fonts');

const PNG_OUTPUT = join(ROOT, 'preview-snowreport.png');
const SVG_OUTPUT = join(ROOT, 'preview-snowreport.svg');

const WIDTH = 960;
const HEIGHT = 540;

const data = JSON.parse(await readFile(join(ROOT, 'snow-sample.json'), 'utf-8'));

const [regular, bold] = await Promise.all([
  readFile(join(FONTS_DIR, 'FiraSans-Regular.ttf')),
  readFile(join(FONTS_DIR, 'FiraSans-Bold.ttf')),
]);

const fonts = [
  { name: 'FiraSans', data: regular, weight: 400, style: 'normal' },
  { name: 'FiraSans', data: bold, weight: 700, style: 'normal' },
];

const started = Date.now();
const svg = await satori(<SnowFrame data={data} />, { width: WIDTH, height: HEIGHT, fonts });
const resvg = new Resvg(svg, { background: 'white', fitTo: { mode: 'width', value: WIDTH } });
const rendered = resvg.render();
const png = rgbaToGrayscalePng(rendered.width, rendered.height, rendered.pixels);

await Promise.all([writeFile(SVG_OUTPUT, svg), writeFile(PNG_OUTPUT, png)]);

const elapsed = Date.now() - started;
console.log(`Rendered snow report in ${elapsed}ms`);
console.log(`  ${SVG_OUTPUT} (${svg.length} bytes)`);
console.log(`  ${PNG_OUTPUT} (${png.length} bytes grayscale)`);
