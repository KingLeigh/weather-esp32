// Renders multiple weather scenarios into a single HTML page for comparison.
// Tests the precipitation summary text across different confidence levels
// and time periods.
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

// Base for deriving scenarios — dry with hourly_temp included.
const base = { ...drySample };

// ─── Scenarios ───────────────────────────────────────────────────────────────

const scenarios = [
  {
    label: 'Dry — no precipitation',
    data: base,
  },
  {
    label: 'Low chance of rain this afternoon (peak 20%, no total)',
    data: {
      ...base,
      updated: '2026-04-17T10:00:00',  // 10am
      rain_chance: [0,0,0,0,10,15,20,15,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
      rain_in: 0,
    },
  },
  {
    label: 'Rain likely this evening (peak 50%, with daily total)',
    data: {
      ...base,
      updated: '2026-04-17T14:00:00',  // 2pm
      rain_chance: [0,0,0,0,10,25,40,50,45,30,15,5,0,0,0,0,0,0,0,0,0,0,0,0],
      rain_in: 0.45,
    },
  },
  {
    label: 'Raining now (peak 80%, with daily total)',
    data: {
      ...base,
      weather: 'rainy',
      updated: '2026-04-17T15:00:00',  // 3pm
      rain_chance: [80,75,60,40,20,10,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
      rain_in: 1.2,
    },
  },
  {
    label: 'Chance of rain tomorrow (peak 25%, no total since different day)',
    data: {
      ...base,
      updated: '2026-04-17T20:00:00',  // 8pm
      rain_chance: [0,0,0,0,0,0,0,0,0,0,10,20,25,20,15,0,0,0,0,0,0,0,0,0],
      rain_in: 0.3,  // should NOT show — different calendar day
    },
  },
  {
    label: 'Rain + Snow (both present)',
    data: rainSnowSample,
  },
];

// ─── Render all scenarios ────────────────────────────────────────────────────

const started = Date.now();
const results = await Promise.all(scenarios.map((s) => renderSvg(s.data)));
const elapsed = Date.now() - started;

const frames = scenarios.map((s, i) => `
  <h2>${s.label}</h2>
  <div class="frame">${results[i].svg}</div>
`).join('\n');

const html = `<!DOCTYPE html>
<html>
<head>
  <title>Weather Display Preview</title>
  <style>
    body { margin: 20px; background: #f0f0f0; font-family: sans-serif; }
    h2 { margin: 16px 0 8px; color: #333; font-size: 16px; }
    .frame { background: white; display: inline-block; border: 1px solid #ccc; }
  </style>
</head>
<body>
  <h1>Weather Display — All Scenarios</h1>
  ${frames}
</body>
</html>`;

await writeFile(OUTPUT, html);
console.log(`Rendered ${scenarios.length} scenarios in ${elapsed}ms`);
console.log(`  ${OUTPUT}`);
