// Renders the production WeatherIcon component for every weather type in
// the layout's WEATHER_ICONS map, at the exact size used in the Hero (180).
// Use this to verify each glyph fits inside its container the same way
// `partly_cloudy` does — font glyphs can have different optical sizes and
// bearings, so it's worth eyeballing them all before trusting the layout.
//
// Writes preview-weather-icons.svg at the renderer root.
//
// Run with: `npm run preview:weather-icons`

import { readFile, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import satori from 'satori';

// Import the real production component so this showcase stays in sync with
// any changes we make to sizing/metrics in layout.jsx.
import { WeatherIcon, WEATHER_ICONS } from './layout.jsx';

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
const OUT = join(ROOT, 'preview-weather-icons.svg');

const WIDTH = 960;
const HEIGHT = 540;
const ICON_SIZE = 180; // matches the Hero's usage

// Build the list of (weather, isDay, label) triples we want to render.
// For types where day !== night (sunny, partly_cloudy) we emit both; for
// neutral types (cloudy, rainy, snowy, thunderstorm, fog) we emit just one.
function buildCases() {
  const out = [];
  for (const [weather, entry] of Object.entries(WEATHER_ICONS)) {
    if (entry.day === entry.night) {
      out.push({ weather, isDay: true, label: weather });
    } else {
      out.push({ weather, isDay: true,  label: `${weather} (day)` });
      out.push({ weather, isDay: false, label: `${weather} (night)` });
    }
  }
  return out;
}

const cases = buildCases();

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

// 3 columns × 3 rows fits 9 cases with room to spare at a readable size.
// If we ever have more cases, widen the grid.
const COLS = 3;

function Cell({ weather, isDay, label }) {
  // The outer "gutter" is bigger than the nominal icon size so we can see
  // overflow on all sides. The red solid box is drawn at exactly ICON_SIZE ×
  // ICON_SIZE so you can tell at a glance whether the glyph stays inside.
  const gutter = 60; // extra space around the box for overflow to be visible
  const cellSize = ICON_SIZE + gutter * 2;
  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        justifyContent: 'flex-start',
        width: cellSize,
      }}
    >
      <div
        style={{
          position: 'relative',
          width: cellSize,
          height: cellSize,
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
        }}
      >
        {/* Expected 180x180 bounding box. */}
        <div
          style={{
            position: 'absolute',
            left: gutter,
            top: gutter,
            width: ICON_SIZE,
            height: ICON_SIZE,
            border: '2px solid #e33',
          }}
        />
        {/* The actual production WeatherIcon, centered on the expected box. */}
        <WeatherIcon weather={weather} isDay={isDay} size={ICON_SIZE} />
      </div>
      <div
        style={{
          fontSize: 15,
          fontWeight: 600,
          color: '#333',
          marginTop: 4,
          textAlign: 'center',
        }}
      >
        {label}
      </div>
    </div>
  );
}

const ROW_H = 340; // tall enough for cell (300 cellSize) + label + gap
const HEADER_H = 50;

function Showcase() {
  const rows = [];
  for (let i = 0; i < cases.length; i += COLS) {
    rows.push(cases.slice(i, i + COLS));
  }
  return (
    <div
      style={{
        width: WIDTH,
        height: rows.length * ROW_H + HEADER_H,
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
          marginBottom: 8,
        }}
      >
        {`PRODUCTION WEATHER ICONS — ${cases.length} cases @ ${ICON_SIZE}px (red box = expected bounds)`}
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
          {row.map(({ weather, isDay, label }) => (
            <Cell
              key={`${weather}-${isDay}`}
              weather={weather}
              isDay={isDay}
              label={label}
            />
          ))}
        </div>
      ))}
    </div>
  );
}

const totalH = Math.ceil(cases.length / COLS) * ROW_H + HEADER_H;
const svgOut = await satori(<Showcase />, { width: WIDTH, height: totalH, fonts });
await writeFile(OUT, svgOut);
console.log(`Wrote ${OUT} (${svgOut.length} bytes, ${cases.length} cases)`);
