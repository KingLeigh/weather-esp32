// One-time download of the FiraSans TTFs we need for rendering.
// Runs as a postinstall hook and is also available as `npm run setup`.
// Safe to re-run: existing files are skipped.

import { access, mkdir, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const FONTS_DIR = join(__dirname, '..', 'fonts');

// Google Fonts' GitHub mirror. These URLs are stable for stable font versions.
const FONTS = [
  {
    name: 'FiraSans-Regular.ttf',
    url: 'https://github.com/google/fonts/raw/main/ofl/firasans/FiraSans-Regular.ttf',
  },
  {
    name: 'FiraSans-Bold.ttf',
    url: 'https://github.com/google/fonts/raw/main/ofl/firasans/FiraSans-Bold.ttf',
  },
];

async function exists(path) {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
}

async function main() {
  await mkdir(FONTS_DIR, { recursive: true });

  for (const { name, url } of FONTS) {
    const dest = join(FONTS_DIR, name);
    if (await exists(dest)) {
      console.log(`  ok  ${name} (already present)`);
      continue;
    }
    process.stdout.write(`  get ${name} ... `);
    const res = await fetch(url);
    if (!res.ok) {
      console.log(`FAILED (${res.status} ${res.statusText})`);
      console.error(`\nCould not download ${url}`);
      console.error('Check your network, or update the URL in scripts/fetch-fonts.js');
      process.exit(1);
    }
    const buf = Buffer.from(await res.arrayBuffer());
    await writeFile(dest, buf);
    console.log(`${buf.length} bytes`);
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
