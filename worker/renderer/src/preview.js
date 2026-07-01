// Local preview entry point: reads the checked-in sample JSON, renders the
// layout, and writes two files at the renderer root:
//
//   preview.svg  — the raw Satori SVG output. Open this in a browser for
//                  fast layout iteration (edit layout.jsx, rerun, reload the
//                  browser tab). This is what resvg will rasterize, so what
//                  you see in the browser matches the final pipeline very
//                  closely (modulo the grayscale conversion).
//
//   preview.png  — final 8bpp grayscale PNG, matching what the production
//                  Worker will serve and what the ESP32/PNGdec pipeline
//                  expects. Use this for the hardware smoke test.
//
// The grayscale conversion and PNG encoding live in `png-encode.js` (not in
// render.jsx) so render.jsx stays portable across Node and Cloudflare Workers.
//
// Run with: `npm run preview`

import { readFile, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { renderSvg } from './render.jsx';
import { rgbaToGrayscalePng } from './png-encode.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');
// Optional sample arg: `tsx src/preview.js weather-sample-night.json` renders
// that fixture to preview-<variant>.png/.svg. With no arg, the default sample
// renders to preview.png/.svg.
const sampleArg = process.argv[2] || 'weather-sample.json';
const variant = /^weather-sample(-[a-z0-9-]+)?\.json$/i.exec(sampleArg);
const suffix = variant && variant[1] ? variant[1] : '';

const SAMPLE = join(ROOT, sampleArg);
const PNG_OUTPUT = join(ROOT, `preview${suffix}.png`);
const SVG_OUTPUT = join(ROOT, `preview${suffix}.svg`);

// ─── main ─────────────────────────────────────────────────────────────────────

const data = JSON.parse(await readFile(SAMPLE, 'utf-8'));

const started = Date.now();
// Location for the message-status provider (see status.js). Real renders get
// the zip from the Worker; previews use a fixed one so '*'/zip rows can match.
const { svg, width, height, pixels } = await renderSvg(data, { location: '10010' });
const png = rgbaToGrayscalePng(width, height, pixels);
const elapsed = Date.now() - started;

await Promise.all([
  writeFile(SVG_OUTPUT, svg),
  writeFile(PNG_OUTPUT, png),
]);

console.log(`Rendered in ${elapsed}ms`);
console.log(`  ${SVG_OUTPUT} (${svg.length} bytes)`);
console.log(`  ${PNG_OUTPUT} (${png.length} bytes grayscale)`);
