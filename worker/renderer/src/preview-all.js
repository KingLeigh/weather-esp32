// Renders all chart variants into a single HTML page for comparison.
//
// Run with: `npm run preview:all`
// Open:     preview-all.html in a browser

import { readFile, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { renderSvg } from './render.jsx';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');
const OUTPUT = join(ROOT, 'preview-all.html');

const drySample = JSON.parse(await readFile(join(ROOT, 'weather-sample-dry.json'), 'utf-8'));
const rainSnowSample = JSON.parse(await readFile(join(ROOT, 'weather-sample-rain-snow.json'), 'utf-8'));

// Derive a rain-only sample by zeroing out snow.
const rainOnlySample = { ...rainSnowSample, snow_chance: Array(24).fill(0), snow_mm: 0 };

const started = Date.now();
const [dry, rainOnly, rainSnow] = await Promise.all([
  renderSvg(drySample),
  renderSvg(rainOnlySample),
  renderSvg(rainSnowSample),
]);
const elapsed = Date.now() - started;

const html = `<!DOCTYPE html>
<html>
<head>
  <title>Weather Display Preview</title>
  <style>
    body { margin: 20px; background: #f0f0f0; font-family: sans-serif; }
    h2 { margin: 16px 0 8px; color: #333; }
    .frame { background: white; display: inline-block; border: 1px solid #ccc; }
  </style>
</head>
<body>
  <h2>Dry (temperature chart)</h2>
  <div class="frame">${dry.svg}</div>

  <h2>Rain only (precipitation chart)</h2>
  <div class="frame">${rainOnly.svg}</div>

  <h2>Rain + Snow (precipitation chart)</h2>
  <div class="frame">${rainSnow.svg}</div>
</body>
</html>`;

await writeFile(OUTPUT, html);
console.log(`Rendered ${3} variants in ${elapsed}ms`);
console.log(`  ${OUTPUT}`);
