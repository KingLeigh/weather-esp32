// The UI for the weather display, authored as JSX + flexbox CSS.
//
// This runs through Satori, which supports a useful subset of CSS centered on
// flexbox. A few quirks to remember while editing:
//
//   1. Any element with 2+ children needs an explicit `display: 'flex'`.
//      Satori will throw "Expected <div> to have explicit display: flex..."
//      otherwise.
//
//   2. Bare text next to an interpolation in a child counts as multiple
//      children. `<div>H {x}°</div>` has 3 children (the string "H ", the
//      expression, and the string "°"). Use a template literal to collapse
//      the whole thing into one string: `<div>{`H ${x}°`}</div>`.
//
//   3. *Raw number children break Satori with the same error message*, even
//      though there's only one child. `<div>{42}</div>` throws; `<div>42</div>`
//      works, but that only helps for literals. For dynamic numbers, always
//      stringify: `<div>{String(x)}</div>` or embed them in a template literal.
//
// React import: tsx (used by npm run preview) auto-injects this, but
// wrangler's esbuild classic JSX transform needs it explicitly in scope.
// eslint-disable-next-line no-unused-vars
import React from 'react';

// ─── shared layout constants ─────────────────────────────────────────────────

const WIDTH = 960;
const HEIGHT = 540;

// Every major section (Hero, PrecipChart, Footer) is padded by this amount on
// the left and right so their content aligns vertically down the page. The
// chart's drawable width is derived from this rather than hardcoded so it
// stays flush with the rest of the layout if padding ever changes.
const PAGE_PADDING_X = 40;
const CONTENT_W = WIDTH - 2 * PAGE_PADDING_X;

const FG = '#000';
const FG_MUTED = '#333'; // Darker than the old #555 — 4bpp e-paper can't
                         // reliably render anything lighter on white.
const BG = '#fff';
const BORDER = '#000';

// Mapping from the normalized `weather` strings to Erik Flowers Weather
// Icons codepoints (Private Use Area of the WeatherIcons TTF). Each entry
// has a `day` and `night` codepoint; most conditions use the same glyph for
// both because a "rainy night" icon is indistinguishable from a "rainy day"
// icon aesthetically. Only sunny/clear and partly_cloudy swap based on
// `is_day`, because those glyphs contain a sun or moon that needs to match.
// `scale` is an optional per-icon fontSize multiplier (relative to the
// container size). Glyphs aren't perfectly normalized in the font — some
// (notably partly_cloudy, which packs a cloud + sun/moon into one glyph)
// overflow unless rendered smaller than the default. `dayScale` and
// `nightScale` optionally override `scale` for one variant, since the day
// and night glyphs for the same condition may not be drawn at matching
// optical sizes.
export const WEATHER_ICONS = {
  sunny:         { day: '\uf00d', night: '\uf02e' },                                // wi_day_sunny / wi_night_clear
  partly_cloudy: { day: '\uf002', night: '\uf031', dayScale: 0.68, nightScale: 0.76 }, // wi_day_cloudy / wi_night_cloudy
  cloudy:        { day: '\uf013', night: '\uf013' },                                // wi_cloudy
  rainy:         { day: '\uf019', night: '\uf019' },                                // wi_rain
  snowy:         { day: '\uf01b', night: '\uf01b' },                                // wi_snow
  thunderstorm:  { day: '\uf01e', night: '\uf01e' },                                // wi_thunderstorm
  fog:           { day: '\uf014', night: '\uf014' },                                // wi_fog
};

// Fallback codepoint for unknown weather strings from the provider.
const UNKNOWN_ICON = '\uf075'; // wi_na ("not available")

const DEFAULT_ICON_SCALE = 0.8;

// Stylized sun icon — stroked circle surrounded by 8 radial line rays.
// Drawn as inline SVG. Optionally accepts a `label` (e.g. "uv") that gets
// overlaid via an absolutely-positioned flex-centered div so font metrics
// handle the centering — SVG dominantBaseline alignment is unreliable.
function SunIcon({ size, label }) {
  const cx = size / 2;
  const cy = size / 2;
  const circleR = size * 0.30;
  const rayInner = size * 0.36;
  const rayOuter = size * 0.48;
  const strokeW = Math.max(3, Math.round(size * 0.05));

  const rays = [];
  for (let i = 0; i < 8; i++) {
    const angle = (i * Math.PI) / 4;
    rays.push({
      key: i,
      x1: cx + rayInner * Math.cos(angle),
      y1: cy + rayInner * Math.sin(angle),
      x2: cx + rayOuter * Math.cos(angle),
      y2: cy + rayOuter * Math.sin(angle),
    });
  }

  const labelFontSize = Math.max(10, Math.round(size * 0.29));

  return (
    <div
      style={{
        position: 'relative',
        width: size,
        height: size,
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        flexShrink: 0,
      }}
    >
      <svg
        width={size}
        height={size}
        style={{ position: 'absolute', left: 0, top: 0 }}
      >
        <circle
          cx={cx}
          cy={cy}
          r={circleR}
          stroke={FG}
          strokeWidth={strokeW}
          fill={BG}
        />
        {rays.map((ray) => (
          <line
            key={ray.key}
            x1={ray.x1}
            y1={ray.y1}
            x2={ray.x2}
            y2={ray.y2}
            stroke={FG}
            strokeWidth={strokeW}
            strokeLinecap="round"
          />
        ))}
      </svg>
      {label && (
        <div
          style={{
            fontSize: labelFontSize,
            fontWeight: 700,
            color: FG,
            lineHeight: 1,
            fontFamily: 'FiraSans',
          }}
        >
          {label}
        </div>
      )}
    </div>
  );
}

function resolveIconScale(entry, isDay) {
  if (!entry) return DEFAULT_ICON_SCALE;
  if (isDay === false && entry.nightScale != null) return entry.nightScale;
  if (isDay !== false && entry.dayScale != null) return entry.dayScale;
  if (entry.scale != null) return entry.scale;
  return DEFAULT_ICON_SCALE;
}

export function WeatherIcon({ weather, isDay, size }) {
  const entry = WEATHER_ICONS[weather];
  const codepoint = entry
    ? (isDay === false ? entry.night : entry.day)
    : UNKNOWN_ICON;

  // Erik Flowers' font has tall bearings — a glyph at `fontSize` px actually
  // occupies noticeably more vertical space than `fontSize` px, and Satori
  // won't clip overflow. Use a fontSize smaller than the container so the
  // whole glyph fits. Per-icon `scale` entries in WEATHER_ICONS let us tune
  // glyphs that are bigger than average (e.g., partly_cloudy).
  const scale = resolveIconScale(entry, isDay);
  const fontSize = Math.round(size * scale);

  return (
    <div
      style={{
        width: size,
        height: size,
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        flexShrink: 0,
      }}
    >
      <div
        style={{
          fontFamily: 'WeatherIcons',
          fontSize,
          lineHeight: 1,
          color: FG,
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
        }}
      >
        {codepoint}
      </div>
    </div>
  );
}

function Hero({ data }) {
  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'row',
        alignItems: 'flex-start',
        padding: `28px ${PAGE_PADDING_X}px 12px ${PAGE_PADDING_X}px`,
      }}
    >
      {/* Left: weather icon */}
      <WeatherIcon
        weather={data.weather}
        isDay={data.is_day}
        size={180}
      />

      {/* Center: temp row (big current temp + UV block) and H/L beneath */}
      <div
        style={{
          display: 'flex',
          flexDirection: 'column',
          marginLeft: 28,
          flexGrow: 1,
        }}
      >
        <div
          style={{
            display: 'flex',
            flexDirection: 'row',
            alignItems: 'flex-start',
          }}
        >
          <div
            style={{
              fontSize: 180,
              lineHeight: 0.9,
              fontWeight: 700,
              color: FG,
            }}
          >
            {`${data.temperature.current}°`}
          </div>

          {/* UV block: [sun icon] [big current] [smaller max], all on a
              shared baseline/center line. `marginLeft: auto` pushes the
              whole block to the right edge of the temp row, which (because
              the temp row stretches across the center column and the center
              column grows to fill the Hero) lands flush with the Hero's
              right padding — i.e., the right edge of the display. */}
          <div
            style={{
              display: 'flex',
              flexDirection: 'row',
              alignItems: 'center',
              marginLeft: 'auto',
              marginTop: 24,
            }}
          >
            <SunIcon size={64} label="UV" />
            <div
              style={{
                fontSize: 120,
                fontWeight: 700,
                color: FG,
                lineHeight: 0.9,
                marginLeft: 12,
              }}
            >
              {String(data.uv.current)}
            </div>
            <div
              style={{
                fontSize: 56,
                fontWeight: 600,
                color: FG_MUTED,
                lineHeight: 0.9,
                marginLeft: 10,
              }}
            >
              {String(data.uv.high)}
            </div>
          </div>
        </div>

        <div
          style={{
            display: 'flex',
            flexDirection: 'row',
            fontSize: 34,
            color: FG_MUTED,
            marginTop: 14,
          }}
        >
          <div>{`H ${data.temperature.high}°`}</div>
          <div style={{ marginLeft: 28 }}>{`L ${data.temperature.low}°`}</div>
        </div>
      </div>

      {/* TODO: wind display — position and styling TBD. */}
    </div>
  );
}

// Extract (hour, minute) from an ISO-ish timestamp string like
// "2026-04-11T14:30:00". We don't use Date parsing because that introduces
// timezone ambiguity between Node/Workers runtimes. The Worker is expected to
// format `updated` as the user's local time (see memory notes).
function parseLocalHourMinute(updated) {
  const m = /T(\d{2}):(\d{2})/.exec(updated ?? '');
  if (!m) return { hour: 0, minute: 0 };
  return { hour: parseInt(m[1], 10), minute: parseInt(m[2], 10) };
}

// Given local hour/minute of "now", compute the chart x positions and labels
// for every 6-hour clock boundary (00:00, 06:00, 12:00, 18:00) that falls
// within the 24-hour window [now, now + 24h].
function computeAxisLabels(nowHour, nowMinute, chartW) {
  const nowMinutes = nowHour * 60 + nowMinute;
  const windowEnd = nowMinutes + 24 * 60;
  const boundaryStep = 6 * 60;

  // First boundary strictly after `now`: smallest multiple of 360 that's > nowMinutes.
  let boundary = Math.floor(nowMinutes / boundaryStep) * boundaryStep + boundaryStep;

  const labelForHour = (h) => {
    const hh = ((h % 24) + 24) % 24;
    if (hh === 0) return '12a';
    if (hh === 12) return '12p';
    return hh < 12 ? `${hh}a` : `${hh - 12}p`;
  };

  const out = [];
  while (boundary < windowEnd) {
    const x = ((boundary - nowMinutes) / (24 * 60)) * chartW;
    const hourOfDay = (boundary / 60) % 24;
    out.push({ x, label: labelForHour(hourOfDay) });
    boundary += boundaryStep;
  }
  return out;
}

// Chart title lookup — keyed by `precip_type` from the provider.
const CHART_TITLES = {
  snow: '24-HOUR SNOW %',
  mixed: '24-HOUR PRECIPITATION %',
};
const DEFAULT_CHART_TITLE = '24-HOUR RAIN %';

// Width of each hour-axis label tile, centered on its tick.
const AXIS_LABEL_W = 50;

function PrecipChart({ data }) {
  const { precipitation, precip_type: precipType, updated } = data;
  const chartW = CONTENT_W;
  const chartH = 150;
  const n = precipitation.length;
  const strokeW = 3;
  // Inset the drawable area so the line stroke doesn't clip against the top
  // edge or the bottom baseline. padBottom is smaller than padTop because the
  // line almost never touches 0% — leaving a few pixels of headroom above the
  // baseline looks cleaner than symmetric insets.
  const padTop = strokeW;
  const padBottom = 2;
  const usableH = chartH - padTop - padBottom;

  // Map each hourly probability (0..100) to an (x, y) point across the width.
  const points = precipitation.map((pct, i) => {
    const x = n === 1 ? 0 : (i / (n - 1)) * chartW;
    const y = padTop + usableH - (pct / 100) * usableH;
    return [x, y];
  });

  const lineStr = points.map(([x, y]) => `${x},${y}`).join(' ');
  // Close the polygon down to the baseline so resvg fills the area beneath
  // the line with the area color.
  const areaStr = `${lineStr} ${chartW},${chartH} 0,${chartH}`;

  const label = CHART_TITLES[precipType] ?? DEFAULT_CHART_TITLE;

  const { hour: nowHour, minute: nowMinute } = parseLocalHourMinute(updated);
  const axisLabels = computeAxisLabels(nowHour, nowMinute, chartW);

  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'column',
        padding: `18px ${PAGE_PADDING_X}px 0 ${PAGE_PADDING_X}px`,
      }}
    >
      <div
        style={{
          display: 'flex',
          flexDirection: 'row',
          fontSize: 18,
          fontWeight: 600,
          color: FG_MUTED,
          letterSpacing: 2,
          marginBottom: 8,
        }}
      >
        <div>{label}</div>
      </div>

      {/* Chart area: filled-area time series. Satori passes inline SVG
          through to resvg, so we draw the polygon (area fill) first and the
          polyline (top edge) over it. */}
      <div
        style={{
          display: 'flex',
          width: chartW,
          height: chartH,
          borderBottom: `2px solid ${BORDER}`,
        }}
      >
        <svg width={chartW} height={chartH}>
          {/* crispEdges disables anti-aliasing on the fill so every interior
              pixel is exactly #ccc. Without this, resvg's edge-AA produces
              intermediate luma values that quantize unevenly to 4bpp on the
              e-paper panel and look grainy. The smooth black polyline on top
              visually hides the jagged edge of the fill. */}
          <polygon points={areaStr} fill="#ccc" shapeRendering="crispEdges" />
          <polyline
            points={lineStr}
            stroke={FG}
            strokeWidth={strokeW}
            fill="none"
          />
        </svg>
      </div>

      {/* Hour axis labels: positioned absolutely at the x coordinates of
          each 6-hour clock boundary in the window. */}
      <div
        style={{
          display: 'flex',
          position: 'relative',
          width: chartW,
          height: 22,
          marginTop: 4,
        }}
      >
        {axisLabels.map(({ x, label: lbl }) => (
          <div
            key={lbl}
            style={{
              position: 'absolute',
              left: x - AXIS_LABEL_W / 2,
              width: AXIS_LABEL_W,
              fontSize: 17,
              fontWeight: 600,
              color: FG_MUTED,
              display: 'flex',
              justifyContent: 'center',
            }}
          >
            {lbl}
          </div>
        ))}
      </div>
    </div>
  );
}

function Footer({ data }) {
  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'row',
        alignItems: 'flex-start',
        padding: `14px ${PAGE_PADDING_X}px 24px ${PAGE_PADDING_X}px`,
      }}
    >
      {/* Sun times */}
      <div
        style={{
          display: 'flex',
          flexDirection: 'row',
          fontSize: 26,
          color: FG,
        }}
      >
        <div>{`↑ ${data.sun.sunrise}`}</div>
        <div style={{ marginLeft: 24 }}>{`↓ ${data.sun.sunset}`}</div>
      </div>

      {/* TODO: moon phase will go somewhere later — Erik Flowers has
          wi_moon_* glyphs we can use. Layout position TBD. */}

      {/*
       * ⚠️ RESERVED REGION — DO NOT DRAW HERE ⚠️
       *
       * The bottom-right corner of the screen (roughly 200×44 px, at the
       * right end of the Footer row) is reserved for the firmware-side
       * overlay: battery percentage and "last updated N min ago" stale-age
       * indicator. The ESP32 draws those into the framebuffer AFTER the
       * server-rendered PNG is blitted, so anything we render here will be
       * covered up. Keep this area empty in the server layout.
       *
       * The dashed outline box was removed for the demo; it used to be
       * here as a visual marker and is tracked in version control if you
       * need to bring it back while iterating. See the project memory for
       * more context.
       */}
      <div style={{ flexGrow: 1 }} />
    </div>
  );
}

export function WeatherFrame({ data }) {
  return (
    <div
      style={{
        width: WIDTH,
        height: HEIGHT,
        background: BG,
        color: FG,
        fontFamily: 'FiraSans',
        display: 'flex',
        flexDirection: 'column',
      }}
    >
      <Hero data={data} />
      <PrecipChart data={data} />
      <Footer data={data} />
    </div>
  );
}
