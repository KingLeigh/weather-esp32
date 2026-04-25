// Standalone 8bpp grayscale PNG encoder, no dependencies beyond Node's built-in
// zlib. Pixels are composited on a white background before luma conversion so
// semi-transparent edges look correct on the e-paper white background.
// Luma formula: ITU-R BT.601 — Y = (77R + 150G + 29B) >> 8

import { deflateSync } from 'node:zlib';

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

export function rgbaToGrayscalePng(width, height, pixels) {
  const rowLen = width + 1; // 1 filter byte + width gray bytes
  const raw = Buffer.allocUnsafe(height * rowLen);

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

  const compressed = deflateSync(raw);

  const ihdr = Buffer.allocUnsafe(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;  // bit depth
  ihdr[9] = 0;  // color type: grayscale
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
