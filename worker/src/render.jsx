// Worker-compatible render pipeline: WeatherData → grayscale PNG bytes.
//
// This is the Cloudflare Workers counterpart to renderer/src/render.jsx +
// renderer/src/preview.js. The layout (JSX) is shared; everything else is
// swapped for Worker-compatible equivalents:
//
//   Node (local preview)          →  Worker (this file)
//   ──────────────────────────────────────────────────
//   satori (auto-loads yoga WASM) →  satori/standalone + pre-compiled yoga
//   @resvg/resvg-js (native)      →  @resvg/resvg-wasm + pre-compiled WASM
//   node:fs (font loading)        →  wrangler Data imports (ArrayBuffer)
//   node:zlib deflateSync          →  CompressionStream('deflate')
//   Buffer                         →  Uint8Array + DataView
//
// Cloudflare Workers block runtime WebAssembly.compile(). Both yoga (satori's
// layout engine) and resvg ship as WASM and normally compile from base64 or
// ArrayBuffer at startup. We sidestep this by importing both .wasm files via
// wrangler's CompiledWasm module rule, which hands us pre-compiled
// WebAssembly.Module objects that can be instantiated without compilation.

import React from 'react';
import satori, { init as initSatori } from 'satori/standalone';
import yogaWasm from 'satori/yoga.wasm';

import { Resvg, initWasm as initResvg } from '@resvg/resvg-wasm';
import resvgWasm from '@resvg/resvg-wasm/index_bg.wasm';

// Font files — wrangler's "Data" module rule imports these as ArrayBuffers.
import firaSansRegular from '../renderer/fonts/FiraSans-Regular.ttf';
import firaSansBold from '../renderer/fonts/FiraSans-Bold.ttf';
import weatherIconsFont from '../renderer/node_modules/weathericons/font/weathericons-regular-webfont.ttf';

import { WeatherFrame } from '../renderer/src/layout.jsx';

const WIDTH = 960;
const HEIGHT = 540;

// ─── WASM initialization ────────────────────────────────────────────────────
// Both initSatori (yoga) and initResvg can only be called once. We combine
// them into a single promise so concurrent requests don't race.

let wasmReady = null;

function ensureWasm() {
  if (!wasmReady) {
    wasmReady = Promise.all([
      initSatori(yogaWasm),
      initResvg(resvgWasm),
    ]);
  }
  return wasmReady;
}

// ─── satori font descriptors ────────────────────────────────────────────────

const fonts = [
  { name: 'FiraSans', data: firaSansRegular, weight: 400, style: 'normal' },
  { name: 'FiraSans', data: firaSansBold,    weight: 700, style: 'normal' },
  { name: 'WeatherIcons', data: weatherIconsFont, weight: 400, style: 'normal' },
];

// ─── grayscale PNG encoder (no Node.js dependencies) ─────────────────────────
// Ported from renderer/src/preview.js. Changes:
//   - Buffer → Uint8Array + DataView
//   - deflateSync → CompressionStream('deflate') (async, Web Streams API)

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
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/** Write a PNG chunk (length + type + data + CRC). */
function pngChunk(type, data) {
  const typeBytes = new TextEncoder().encode(type);    // 4 ASCII bytes
  const combined = new Uint8Array(typeBytes.length + data.length);
  combined.set(typeBytes, 0);
  combined.set(data, typeBytes.length);

  const crc = crc32(combined);

  const chunk = new Uint8Array(4 + 4 + data.length + 4);
  const view = new DataView(chunk.buffer);
  view.setUint32(0, data.length);                      // length
  chunk.set(typeBytes, 4);                              // type
  chunk.set(data, 8);                                   // data
  view.setUint32(8 + data.length, crc);                // CRC
  return chunk;
}

/** Compress a Uint8Array using zlib (RFC 1950) via CompressionStream. */
async function zlibCompress(data) {
  const cs = new CompressionStream('deflate');
  const writer = cs.writable.getWriter();
  writer.write(data);
  writer.close();

  const chunks = [];
  const reader = cs.readable.getReader();
  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
  }

  // Concatenate
  const totalLen = chunks.reduce((sum, c) => sum + c.length, 0);
  const out = new Uint8Array(totalLen);
  let offset = 0;
  for (const c of chunks) {
    out.set(c, offset);
    offset += c.length;
  }
  return out;
}

/**
 * Convert RGBA pixels to a grayscale PNG.
 * Composites on white before luma conversion so semi-transparent edges
 * look correct on the e-paper's white background.
 * Luma: ITU-R BT.601 — Y = (77R + 150G + 29B) >> 8
 */
async function rgbaToGrayscalePng(width, height, pixels) {
  // Build raw scanlines: filter byte (0 = None) + width gray bytes per row.
  const rowLen = width + 1;
  const raw = new Uint8Array(height * rowLen);

  for (let y = 0; y < height; y++) {
    raw[y * rowLen] = 0; // filter type: None
    for (let x = 0; x < width; x++) {
      const i = (y * width + x) * 4;
      const a = pixels[i + 3] / 255;
      const r = Math.round(pixels[i]     * a + 255 * (1 - a));
      const g = Math.round(pixels[i + 1] * a + 255 * (1 - a));
      const b = Math.round(pixels[i + 2] * a + 255 * (1 - a));
      raw[y * rowLen + x + 1] = (77 * r + 150 * g + 29 * b) >> 8;
    }
  }

  const compressed = await zlibCompress(raw);

  // IHDR: 13 bytes
  const ihdr = new Uint8Array(13);
  const ihdrView = new DataView(ihdr.buffer);
  ihdrView.setUint32(0, width);
  ihdrView.setUint32(4, height);
  ihdr[8] = 8;  // bit depth
  ihdr[9] = 0;  // color type: grayscale
  ihdr[10] = 0; // compression: deflate
  ihdr[11] = 0; // filter: adaptive
  ihdr[12] = 0; // interlace: none

  // PNG signature + IHDR + IDAT + IEND
  const sig = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]);
  const ihdrChunk = pngChunk('IHDR', ihdr);
  const idatChunk = pngChunk('IDAT', compressed);
  const iendChunk = pngChunk('IEND', new Uint8Array(0));

  const totalLen = sig.length + ihdrChunk.length + idatChunk.length + iendChunk.length;
  const png = new Uint8Array(totalLen);
  let off = 0;
  png.set(sig, off);       off += sig.length;
  png.set(ihdrChunk, off); off += ihdrChunk.length;
  png.set(idatChunk, off); off += idatChunk.length;
  png.set(iendChunk, off);
  return png;
}

// ─── public API ──────────────────────────────────────────────────────────────

/**
 * Render a weather frame to a grayscale PNG (Uint8Array).
 *
 * @param {object} data - Normalized WeatherData (same shape as /weather.json).
 * @returns {Promise<Uint8Array>} 960×540 8bpp grayscale PNG bytes.
 */
export async function renderWeatherPng(data) {
  await ensureWasm();

  // JSX → SVG
  const svg = await satori(<WeatherFrame data={data} />, {
    width: WIDTH,
    height: HEIGHT,
    fonts,
  });

  // SVG → RGBA pixels
  const resvg = new Resvg(svg, {
    background: 'white',
    fitTo: { mode: 'width', value: WIDTH },
  });
  const rendered = resvg.render();
  const pixels = rendered.pixels; // Uint8Array, RGBA, row-major

  // RGBA → grayscale PNG
  return rgbaToGrayscalePng(rendered.width, rendered.height, pixels);
}
