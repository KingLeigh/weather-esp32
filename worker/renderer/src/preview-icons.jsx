// One-off script: render a showcase grid of Erik Flowers' Weather Icons so
// we can eyeball which ones to use for each condition. Writes
// preview-icons.svg at the renderer root — open that in a browser.
//
// Weather Icons is distributed as an icon font (weathericons npm package),
// so we load the TTF directly into Satori and render each icon as text at
// a specific Unicode codepoint in the Private Use Area. This is cleaner than
// the SVG approach: no animation to strip, perfect vector scaling at any
// size, and they'll flatten to grayscale correctly because they're single-
// color glyphs.
//
// Run with: `npm run preview:icons`

import { readFile, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import satori from 'satori';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');
const FONTS_DIR = join(ROOT, 'fonts');
const WI_FONT = join(
  ROOT,
  'node_modules',
  'weathericons',
  'font',
  'weathericons-regular-webfont.ttf',
);
const OUT = join(ROOT, 'preview-icons.svg');

const WIDTH = 960;
const HEIGHT = 540;

// [codepoint, wi_ name, human label]
// Codepoints from node_modules/weathericons/values/weathericons.xml.
const ICONS = [
  ['\uf00d', 'wi_day_sunny',        'sunny (day)'],
  ['\uf02e', 'wi_night_clear',      'clear (night)'],
  ['\uf002', 'wi_day_cloudy',       'partly cloudy (day)'],
  ['\uf031', 'wi_night_cloudy',     'partly cloudy (night)'],
  ['\uf013', 'wi_cloudy',           'cloudy'],
  ['\uf014', 'wi_fog',              'fog'],
  ['\uf0b6', 'wi_day_haze',         'haze'],
  ['\uf01c', 'wi_sprinkle',         'drizzle'],
  ['\uf019', 'wi_rain',             'rain'],
  ['\uf01a', 'wi_showers',          'showers'],
  ['\uf01b', 'wi_snow',             'snow'],
  ['\uf0b5', 'wi_sleet',            'sleet'],
  ['\uf01e', 'wi_thunderstorm',     'thunderstorm'],
  ['\uf01d', 'wi_storm_showers',    'thunderstorm + rain'],
  ['\uf021', 'wi_windy',            'windy'],
];

// ─── load fonts ───────────────────────────────────────────────────────────────

const [regular, bold, weatherIconFont] = await Promise.all([
  readFile(join(FONTS_DIR, 'FiraSans-Regular.ttf')),
  readFile(join(FONTS_DIR, 'FiraSans-Bold.ttf')),
  readFile(WI_FONT),
]);

const fonts = [
  { name: 'FiraSans', data: regular, weight: 400, style: 'normal' },
  { name: 'FiraSans', data: bold, weight: 700, style: 'normal' },
  { name: 'WeatherIcons', data: weatherIconFont, weight: 400, style: 'normal' },
];

// ─── layout ───────────────────────────────────────────────────────────────────

const ICON_FONT_SIZE = 80;
const CELL_W = 180;
const CELL_H = 160;
const COLS = 5;

function IconCell({ codepoint, wiName, label }) {
  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        justifyContent: 'flex-start',
        width: CELL_W,
        height: CELL_H,
      }}
    >
      <div
        style={{
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          width: ICON_FONT_SIZE + 20,
          height: ICON_FONT_SIZE + 20,
          fontFamily: 'WeatherIcons',
          fontSize: ICON_FONT_SIZE,
          color: '#000',
        }}
      >
        {codepoint}
      </div>
      <div
        style={{
          fontSize: 15,
          fontWeight: 600,
          color: '#333',
          marginTop: 6,
          textAlign: 'center',
        }}
      >
        {label}
      </div>
      <div
        style={{
          fontSize: 11,
          color: '#888',
          marginTop: 1,
        }}
      >
        {wiName}
      </div>
    </div>
  );
}

function Showcase() {
  const rows = [];
  for (let i = 0; i < ICONS.length; i += COLS) {
    rows.push(ICONS.slice(i, i + COLS));
  }
  return (
    <div
      style={{
        width: WIDTH,
        height: HEIGHT,
        background: '#fff',
        fontFamily: 'FiraSans',
        display: 'flex',
        flexDirection: 'column',
        padding: '16px 20px 10px 20px',
      }}
    >
      <div
        style={{
          fontSize: 20,
          fontWeight: 700,
          color: '#000',
          letterSpacing: 2,
          marginBottom: 4,
        }}
      >
        ERIK FLOWERS WEATHER ICONS — SAMPLER
      </div>
      {rows.map((row, i) => (
        <div
          key={i}
          style={{
            display: 'flex',
            flexDirection: 'row',
            justifyContent: 'space-around',
          }}
        >
          {row.map(([codepoint, wiName, label]) => (
            <IconCell
              key={wiName}
              codepoint={codepoint}
              wiName={wiName}
              label={label}
            />
          ))}
        </div>
      ))}
    </div>
  );
}

const svgOut = await satori(<Showcase />, { width: WIDTH, height: HEIGHT, fonts });
await writeFile(OUT, svgOut);
console.log(`Wrote ${OUT} (${svgOut.length} bytes)`);
