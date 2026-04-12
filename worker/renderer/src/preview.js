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
// The grayscale conversion and PNG encoding live in this file (not in
// render.jsx) so render.jsx stays portable across Node and Cloudflare Workers.
//
// Run with: `npm run preview`

import { readFile, writeFile } from 'node:fs/promises';
import { deflateSync } from 'node:zlib';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { renderSvg } from './render.jsx';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');
const SAMPLE = join(ROOT, 'weather-sample.json');
const PNG_OUTPUT = join(ROOT, 'preview.png');
const SVG_OUTPUT = join(ROOT, 'preview.svg');

// ─── grayscale PNG encoder (no dependencies beyond Node's built-in zlib) ──────

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (const b of buf) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function pngChunk(type, data) {
  const typeBytes = Buffer.from(type, 'ascii');
  const lenBuf = Buffer.allocUnsafe(4);
  lenBuf.writeUInt32BE(data.length);
  const crcBuf = Buffer.allocUnsafe(4);
  crcBuf.writeUInt32BE(crc32(Buffer.concat([typeBytes, data])));
  return Buffer.concat([lenBuf, typeBytes, data, crcBuf]);
}

/**
 * Convert an RGBA Uint8Array (width × height × 4 bytes) to a grayscale PNG.
 * Pixels are composited on a white background before luma conversion so
 * semi-transparent edges look correct on the e-paper white background.
 * Luma formula: ITU-R BT.601 — Y = (77R + 150G + 29B) >> 8
 */
function rgbaToGrayscalePng(width, height, pixels) {
  // Build one row buffer per scanline (filter byte 0 = None + gray values).
  const rowLen = width + 1; // 1 filter byte + width gray bytes
  const raw = Buffer.allocUnsafe(height * rowLen);

  for (let y = 0; y < height; y++) {
    raw[y * rowLen] = 0; // filter type: None
    for (let x = 0; x < width; x++) {
      const i = (y * width + x) * 4;
      const a = pixels[i + 3] / 255;
      // Composite on white background.
      const r = Math.round(pixels[i]     * a + 255 * (1 - a));
      const g = Math.round(pixels[i + 1] * a + 255 * (1 - a));
      const b = Math.round(pixels[i + 2] * a + 255 * (1 - a));
      raw[y * rowLen + x + 1] = (77 * r + 150 * g + 29 * b) >> 8;
    }
  }

  const compressed = deflateSync(raw);

  const ihdr = Buffer.allocUnsafe(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 0; // color type: grayscale
  ihdr[10] = 0; // compression method: deflate
  ihdr[11] = 0; // filter method: adaptive
  ihdr[12] = 0; // interlace: none

  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), // PNG signature
    pngChunk('IHDR', ihdr),
    pngChunk('IDAT', compressed),
    pngChunk('IEND', Buffer.alloc(0)),
  ]);
}

// ─── main ─────────────────────────────────────────────────────────────────────

const data = JSON.parse(await readFile(SAMPLE, 'utf-8'));

const started = Date.now();
const { svg, width, height, pixels } = await renderSvg(data);
const png = rgbaToGrayscalePng(width, height, pixels);
const elapsed = Date.now() - started;

await Promise.all([
  writeFile(SVG_OUTPUT, svg),
  writeFile(PNG_OUTPUT, png),
]);

console.log(`Rendered in ${elapsed}ms`);
console.log(`  ${SVG_OUTPUT} (${svg.length} bytes)`);
console.log(`  ${PNG_OUTPUT} (${png.length} bytes grayscale)`);
