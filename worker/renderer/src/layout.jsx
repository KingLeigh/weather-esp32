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

// Every major section (Hero, ForecastChart, Footer) is padded by this amount on
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
// Day/night variants (sun/moon visible) are only used for conditions where
// you can see the sky. For heavy weather (rain, snow, fog, etc.) the sky
// isn't visible, so a generic icon is used regardless of time of day.
//
// `scale` / `dayScale` / `nightScale` are optional fontSize multipliers —
// some glyphs (especially compound ones like partly_cloudy) overflow their
// nominal bounding box and need scaling down.
// `yOffset` / `dayYOffset` / `nightYOffset` shift the glyph vertically (in px,
// negative = up) to correct for icons where the visual weight sits low or
// high within the font's baseline.
export const WEATHER_ICONS = {
  // Sky visible — day/night variants
  sunny:         { day: '\uf00d', night: '\uf02e' },                                                      // wi_day_sunny / wi_night_clear
  partly_cloudy: { day: '\uf002', night: '\uf031', dayScale: 0.68, nightScale: 0.76, dayYOffset: 28 },    // wi_day_cloudy / wi_night_cloudy
  haze:          { day: '\uf0b6', night: '\uf04a', nightYOffset: -12 },                                   // wi_day_haze / wi_night_fog (no night haze glyph)

  // Sky not visible — same icon day and night
  cloudy:        { day: '\uf013', night: '\uf013', yOffset: -2 },                                         // wi_cloudy
  drizzle:       { day: '\uf01c', night: '\uf01c', yOffset: -14 },                                        // wi_sprinkle
  rainy:         { day: '\uf019', night: '\uf019', yOffset: -40 },                                        // wi_rain
  sleet:         { day: '\uf0b5', night: '\uf0b5', yOffset: -46 },                                        // wi_sleet
  snowy:         { day: '\uf01b', night: '\uf01b', yOffset: -46 },                                        // wi_snow
  thunderstorm:  { day: '\uf01e', night: '\uf01e', yOffset: -46 },                                        // wi_thunderstorm
  fog:           { day: '\uf014', night: '\uf014', yOffset: -26 },                                        // wi_fog
  smoke:         { day: '\uf062', night: '\uf062' },                                                      // wi_smoke
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
        viewBox={`0 0 ${size} ${size}`}
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

function resolveIconYOffset(entry, isDay) {
  if (!entry) return 0;
  if (isDay === false && entry.nightYOffset != null) return entry.nightYOffset;
  if (isDay !== false && entry.dayYOffset != null) return entry.dayYOffset;
  if (entry.yOffset != null) return entry.yOffset;
  return 0;
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
  // yOffset shifts the glyph vertically. Negative = up, positive = down.
  // Satori's flexbox subset doesn't support CSS transforms, so we use
  // `marginTop` to nudge the rendered glyph within its container.
  const yOffset = resolveIconYOffset(entry, isDay);

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
          marginTop: yOffset,
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
        padding: `32px ${PAGE_PADDING_X}px 6px ${PAGE_PADDING_X}px`,
      }}
    >
      {/* Left: weather icon */}
      <WeatherIcon
        weather={data.weather}
        isDay={data.is_day}
        size={180}
      />

      {/* Center: current temp, H/L stack, and UV block on one row */}
      <div
        style={{
          display: 'flex',
          flexDirection: 'row',
          alignItems: 'flex-start',
          marginLeft: 28,
          flexGrow: 1,
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

        {/* H/L stacked vertically, between current temp and UV */}
        <div
          style={{
            display: 'flex',
            flexDirection: 'column',
            justifyContent: 'center',
            marginLeft: 20,
            alignSelf: 'center',
            fontSize: 48,
            fontWeight: 700,
            color: FG_MUTED,
            lineHeight: 1.2,
          }}
        >
          <div>{`H ${data.temperature.high}°`}</div>
          <div>{`L ${data.temperature.low}°`}</div>
        </div>

        {/* UV block pushed to right edge */}
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

      {/* TODO: wind display — position and styling TBD. */}
    </div>
  );
}

// Extract the hour from an ISO-ish timestamp like "2026-04-11T14:30:00".
// Minutes are ignored — the chart aligns to hourly buckets matching the data
// granularity, so axis labels and gridlines land on exact bar edges.
function parseLocalHour(updated) {
  const m = /T(\d{2})/.exec(updated ?? '');
  return m ? parseInt(m[1], 10) : 0;
}

// Format minutes-since-local-midnight as a compact marker label.
// 405 → "6:45a", 1168 → "7:28p".
function formatClockLabel(min) {
  const h24 = Math.floor(min / 60);
  const m = min % 60;
  const suffix = h24 >= 12 ? 'p' : 'a';
  const h = h24 % 12 === 0 ? 12 : h24 % 12;
  return `${h}:${String(m).padStart(2, '0')}${suffix}`;
}

// Compute chart x positions and labels for every 3-hour clock boundary
// (00, 03, 06, 09, 12, 15, 18, 21) in the window starting at nowHour.
//
// `n` is the number of hourly data points. Positions use the same
// `slot / (n - 1)` mapping as the temperature line (see ForecastChart), so
// gridlines, axis labels, and the per-gridline temperature labels all share
// an x for a given hour offset — a label drawn at `slot` lands exactly on the
// line's vertex at `slot`. Each returned entry carries its `slot` (the hour
// offset, which doubles as the index into hourly_temp).
function computeAxisLabels(nowHour, chartW, n) {
  const slotW = chartW / (n - 1);

  const labelForHour = (h) => {
    const hh = ((h % 24) + 24) % 24;
    if (hh === 0) return '00';
    if (hh === 12) return '12';
    return String(hh < 12 ? hh : hh - 12);
  };

  const out = [];
  // First 3-hour boundary strictly after nowHour.
  let h = Math.floor(nowHour / 3) * 3 + 3;
  while (h < nowHour + n) {
    const slot = h - nowHour;
    const x = slot * slotW;
    const hh = h % 24;
    const isMajor = hh === 0 || hh === 12;
    out.push({ x, slot, label: labelForHour(hh), isMajor });
    h += 3;
  }
  return out;
}

// Threshold: show precip chart if any hour has >= this % chance.
const PRECIP_THRESHOLD = 5;

// Width of each hour-axis label tile, centered on its tick.
const AXIS_LABEL_W = 50;

// ─── night ribbon / sun markers ─────────────────────────────────────────────
// The strip directly above the chart doubles as a "night ribbon": a solid bar
// spanning the hours the sun is down. It can wrap into two segments (one at
// each edge of the window) when "now" itself falls at night. The sunrise/sunset
// time labels sit in the DAY portion of that same strip, just outside their
// marker, so the strip costs no height beyond the old floating labels.
const SUN_STRIP_H = 22;     // strip the sun time-labels occupy above the chart
const RIBBON_H = 10;        // night-ribbon thickness
const NIGHT_FILL = '#999';  // solid night fill — a soft grey wash, lighter than the temp line
// Vertical placement of the ribbon: true = flush with the chart's top edge
// (its bottom sits on y=0); false = centered in the label strip, floating just
// above the chart (aligned with the time labels). Flip to compare.
const RIBBON_FLUSH = false;
const SUN_LABEL_W = 58;     // width of a sun time-label tile
const SUN_LABEL_PAD = 6;    // gap between a marker and its label

// ─── shared axis labels (used by both chart types) ──────────────────────────

function AxisLabels({ chartW, updated, n }) {
  const nowHour = parseLocalHour(updated);
  // Drop a label that lands exactly on the last hour (x === chartW): centered
  // there it overruns the right edge and collides with the firmware-drawn
  // battery overlay in the bottom-right. The gridline itself still renders.
  const axisLabels = computeAxisLabels(nowHour, chartW, n)
    .filter(({ slot }) => slot !== n - 1);

  return (
    <div
      style={{
        display: 'flex',
        position: 'relative',
        width: chartW,
        height: 22,
        marginTop: 8,
      }}
    >
      {axisLabels.map(({ x, slot, label: lbl }) => (
        <div
          key={slot}
          style={{
            position: 'absolute',
            left: x - AXIS_LABEL_W / 2,
            width: AXIS_LABEL_W,
            fontSize: 24,
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
  );
}

// ─── shared chart gridlines (vertical lines at 3-hour boundaries) ───────────

const GRIDLINE_MAJOR = '#888';  // midnight, noon — one shade darker
const GRIDLINE_MINOR = '#999';  // every other 3-hour mark + horizontals

function chartGridlines(chartW, chartH, updated, n, yTop, yBottom) {
  // yTop / yBottom define the drawable region within the chart; gridlines
  // are clipped to this range so they intersect cleanly with horizontal
  // reference lines. Defaults to full chart height if not provided.
  const y1 = yTop ?? 0;
  const y2 = yBottom ?? chartH - 4;
  const nowHour = parseLocalHour(updated);
  const labels = computeAxisLabels(nowHour, chartW, n);

  return labels.map(({ x, isMajor }, i) => (
    <line
      key={`g${i}`}
      x1={x}
      y1={y1}
      x2={x}
      y2={y2}
      stroke={isMajor ? GRIDLINE_MAJOR : GRIDLINE_MINOR}
      strokeWidth={1}
      shapeRendering="crispEdges"
    />
  ));
}

// ─── forecast chart (temperature line + optional precip bars) ───────────────

function ForecastChart({ data, hasRain, hasSnow }) {
  const { hourly_temp, rain_chance, snow_chance, updated } = data;
  const chartW = CONTENT_W;
  const chartH = 164;
  const n = hourly_temp.length;
  const strokeW = 3;

  // ── Temperature y-axis: auto-scaled to rounded steps ──────────────
  const minTemp = Math.min(...hourly_temp);
  const maxTemp = Math.max(...hourly_temp);
  const rawRange = maxTemp - minTemp;
  const step = rawRange <= 10 ? 5 : 10;
  const scaleMin = Math.floor(minTemp / step) * step;
  const scaleMax = Math.ceil(maxTemp / step) * step;
  const range = scaleMax - scaleMin || 1;

  const inset = 2;
  const usableH = chartH - 2 * inset;

  const yForTemp = (t) => inset + usableH - ((t - scaleMin) / range) * usableH;

  const points = hourly_temp.map((t, i) => {
    const x = n === 1 ? 0 : (i / (n - 1)) * chartW;
    const y = yForTemp(t);
    return [x, y];
  });

  const lineStr = points.map(([x, y]) => `${x},${y}`).join(' ');

  // ── Precipitation bars (rendered underneath temp line) ─────────────
  const barW = chartW / n;
  const rainBars = hasRain ? rain_chance.map((pct, i) => {
    const barH = (pct / 100) * usableH;
    return { x: i * barW, y: inset + usableH - barH, w: barW, h: barH };
  }) : [];
  const snowBars = hasSnow ? snow_chance.map((pct, i) => {
    const barH = (pct / 100) * usableH;
    return { x: i * barW, y: inset + usableH - barH, w: barW, h: barH };
  }) : [];

  // ── Temperature label placement ────────────────────────────────────
  // Vertical midpoint of the plotted region. A point above this line
  // (hotter — smaller y) gets its label *below* the curve; a point below
  // it gets the label *above*. This keeps every label clear of the chart's
  // top and bottom edges regardless of where the curve sits.
  const midY = inset + usableH / 2;

  // ── Title + summary ───────────────────────────────────────────────
  const hasPrecip = hasRain || hasSnow;

  // Precipitation summary — confidence-aware, with natural time periods.
  //
  // Confidence tiers based on peak probability:
  //   < 20%  → no text (chart bars still visible if above PRECIP_THRESHOLD)
  //   5-30%  → "Chance of {type} {period}"
  //   30-60% → "{Type} likely {period}"
  //   > 60%  → "{Type} {period}" / "{Type-ing} now"
  //
  // Time periods (based on clock hour of first significant probability):
  //   Hour 0           → "now"
  //   4am-12pm         → "this morning"
  //   12pm-5pm         → "this afternoon"
  //   5pm-9pm          → "this evening"
  //   9pm-12am         → "tonight"
  //   12am-4am         → "overnight"
  //   4am+ next day    → "tomorrow"
  //
  // Daily total appended as "· X.X" today" only when confidence > 30%
  // AND total > 0 AND period is not "tomorrow" (different calendar day).

  const nowH = parseLocalHour(updated);

  // Map an hour offset (0-23) to a named time period.
  const periodForOffset = (offset) => {
    if (offset === 0) return 'now';
    const clockH = (nowH + offset) % 24;
    const isNextDay = nowH + offset >= 24;
    if (isNextDay && clockH >= 4) return 'tomorrow';
    if (clockH >= 4 && clockH < 12) return 'this morning';
    if (clockH >= 12 && clockH < 17) return 'this afternoon';
    if (clockH >= 17 && clockH < 21) return 'this evening';
    if (clockH >= 21) return 'tonight';
    return 'overnight'; // 0-4
  };

  // Confidence thresholds aligned with NWS (National Weather Service)
  // standard probability-to-language mappings:
  //   < 20%   — not mentioned in text (bars still render on chart)
  //   20-50%  — "Chance of rain"
  //   60-70%  — "Rain likely"
  //   80%+    — "Rain" / "Raining"
  // Ref: https://www.weather.gov/media/pah/WeatherEducation/pop.pdf
  //      https://forecast.weather.gov/glossary.php?word=slight+chance
  const TEXT_THRESHOLD = 20;

  const describeSingleType = (chances, typeNoun, typeVerb) => {
    const typeCap = typeNoun[0].toUpperCase() + typeNoun.slice(1);
    const peak = Math.max(...chances);
    if (peak < PRECIP_THRESHOLD) return null; // no bars on chart either

    // Find first hour above TEXT_THRESHOLD for the time description.
    const firstSignificant = chances.findIndex((p) => p >= TEXT_THRESHOLD);
    if (firstSignificant === -1) return null; // bars show but too low for text

    const period = periodForOffset(firstSignificant);
    const isTomorrow = period === 'tomorrow';

    let desc;
    if (peak <= 50) {
      desc = `Chance of ${typeNoun} ${period}`;
    } else if (peak <= 70) {
      desc = period === 'now' ? `${typeCap} likely now` : `${typeCap} likely ${period}`;
    } else {
      // 80%+ — high confidence, use present tense for "now"
      desc = period === 'now' ? `${typeVerb} now` : `${typeCap} ${period}`;
    }

    return { desc, isTomorrow, peak, firstHour: firstSignificant };
  };

  // Pick the most relevant precipitation type to describe in text.
  // When both rain and snow are present, describe whichever comes first
  // (i.e., is most imminent). If the earlier one doesn't meet the text
  // threshold, fall through to the later one. The chart handles the
  // visual representation of both types.
  let rightSummary = '';
  if (!hasPrecip) {
    rightSummary = '';
  } else {
    const rainResult = hasRain ? describeSingleType(rain_chance, 'rain', 'Raining') : null;
    const snowResult = hasSnow ? describeSingleType(snow_chance, 'snow', 'Snowing') : null;

    // Pick the earliest, or whichever is non-null.
    let result, amount;
    if (rainResult && snowResult) {
      if (rainResult.firstHour <= snowResult.firstHour) {
        result = rainResult; amount = data.rain_in;
      } else {
        result = snowResult; amount = data.snow_in;
      }
    } else if (rainResult) {
      result = rainResult; amount = data.rain_in;
    } else {
      result = snowResult; amount = data.snow_in;
    }

    if (!result) {
      rightSummary = '';
    } else {
      const showTotal = result.peak > 50 && amount > 0 && !result.isTomorrow;
      rightSummary = showTotal ? `${result.desc} · ${String(amount)}" today` : result.desc;
    }
  }

  // ── Sunrise/sunset markers ─────────────────────────────────────────
  // The provider gives local clock times as minutes since midnight. Map
  // each to an hour offset from nowH (the same reference the gridlines
  // use) and then to an x via the shared slot/(n-1) mapping, so the dotted
  // markers align with the grid. Times not in the data are skipped.
  const sun = data.sun || {};
  const sunMarks = [
    { key: 'sunrise', min: sun.sunrise_min },
    { key: 'sunset', min: sun.sunset_min },
  ]
    .filter((m) => Number.isFinite(m.min))
    .map((m) => {
      const offset = (((m.min / 60 - nowH) % 24) + 24) % 24;
      return { key: m.key, x: (offset / (n - 1)) * chartW, label: formatClockLabel(m.min) };
    });

  // Night-ribbon segments. The sun is down *before* a sunrise and *after* a
  // sunset, so each gap's day/night state is fixed by the marker that bounds it
  // — no is_day flag needed, and the window's night wraps to two segments (one
  // at each edge) automatically when "now" falls at night. x is clamped to the
  // chart so a marker just past the right edge still closes its segment.
  const sortedSun = sunMarks
    .map((m) => ({ ...m, x: Math.max(0, Math.min(chartW, m.x)) }))
    .sort((a, b) => a.x - b.x);
  const nightSegments = [];
  let segStart = 0;
  for (const mark of sortedSun) {
    if (mark.key === 'sunrise' && mark.x > segStart) nightSegments.push([segStart, mark.x]);
    segStart = mark.x;
  }
  const lastSun = sortedSun[sortedSun.length - 1];
  if (lastSun && lastSun.key === 'sunset' && chartW > lastSun.x) {
    nightSegments.push([lastSun.x, chartW]);
  }

  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'column',
        padding: `10px ${PAGE_PADDING_X}px 0 ${PAGE_PADDING_X}px`,
      }}
    >
      {/* Title row: fixed height so the chart below doesn't shift when
          the summary text is empty, and so long strings can't wrap onto
          a second line and push the chart off the bottom of the frame. */}
      <div
        style={{
          display: 'flex',
          flexDirection: 'row',
          alignItems: 'center',
          color: FG_MUTED,
          height: 50,
          marginBottom: 32,
        }}
      >
        <div
          style={{
            marginLeft: 'auto',
            fontSize: 42,
            fontWeight: 700,
            whiteSpace: 'nowrap',
          }}
        >
          {rightSummary}
        </div>
      </div>

      <div
        style={{
          display: 'flex',
          position: 'relative',
          width: chartW,
          height: chartH,
        }}
      >
        {/* viewBox is REQUIRED: Satori embeds this nested <svg> as an
            <image> data-URI. Without a viewBox it emits width="Infinity"
            height="NaN", leaving resvg to guess the coordinate system when
            scaling into the 880px box — local resvg fills the box (1:1) but
            the Cloudflare-edge resvg renders the content ~2.4% narrow,
            drifting the gridlines/line away from the outer-positioned labels.
            An explicit viewBox makes the mapping deterministic everywhere. */}
        <svg width={chartW} height={chartH} viewBox={`0 0 ${chartW} ${chartH}`} style={{ position: 'absolute', left: 0, top: 0 }}>
          {chartGridlines(chartW, chartH, updated, n, inset, inset + usableH)}
          {/* Horizontal reference lines at each temp step */}
          {(() => {
            const lines = [];
            for (let t = scaleMin; t <= scaleMax; t += step) {
              const y = yForTemp(t);
              lines.push(
                <line
                  key={`ht${t}`}
                  x1={0}
                  y1={y}
                  x2={chartW}
                  y2={y}
                  stroke={GRIDLINE_MINOR}
                  strokeWidth={1}
                  shapeRendering="crispEdges"
                />
              );
            }
            return lines;
          })()}
          {/* Rain bars (light grey, behind temp line) */}
          {rainBars.map((bar, i) => (
            bar.h > 0 && (
              <rect
                key={`r${i}`}
                x={bar.x}
                y={bar.y}
                width={bar.w}
                height={bar.h}
                fill="#ccc"
                shapeRendering="crispEdges"
              />
            )
          ))}
          {/* Snow bars (dark grey, behind temp line) */}
          {snowBars.map((bar, i) => (
            bar.h > 0 && (
              <rect
                key={`s${i}`}
                x={bar.x}
                y={bar.y}
                width={bar.w}
                height={bar.h}
                fill="#666"
                shapeRendering="crispEdges"
              />
            )
          ))}
          {/* Temperature line (on top of everything) */}
          <polyline
            points={lineStr}
            stroke={FG}
            strokeWidth={strokeW}
            fill="none"
          />
          {/* X-axis baseline — a crisp SVG line rather than a CSS border so
              its ends don't anti-alias into a stray pixel at the right edge
              (same crispEdges technique as the gridlines). */}
          <line
            x1={0}
            y1={chartH - 1}
            x2={chartW + 1}
            y2={chartH - 1}
            stroke={BORDER}
            strokeWidth={2}
            shapeRendering="crispEdges"
          />
        </svg>
        {/* Night ribbon: solid bar(s) in the strip above the chart marking the
            hours the sun is down, flush with the chart's top edge. The dotted
            markers drop from its edges; the time labels sit in the day gaps. */}
        {nightSegments.map(([x0, x1], i) => (
          <div
            key={`night-${i}`}
            style={{
              position: 'absolute',
              left: x0,
              top: RIBBON_FLUSH ? -RIBBON_H : -(SUN_STRIP_H + RIBBON_H) / 2,
              width: x1 - x0,
              height: RIBBON_H,
              backgroundColor: NIGHT_FILL,
            }}
          />
        ))}
        {/* Temperature labels at each 3-hour gridline. Each sits on the
            curve's vertex for that hour (shared x-mapping), placed above or
            below the line per the midY rule so it never clips an edge. A label
            on the last hour (slot n-1, x === chartW) is skipped — it would
            overrun the right edge / firmware battery overlay. */}
        {computeAxisLabels(nowH, chartW, n)
          .filter(({ slot }) => slot !== n - 1)
          .map(({ slot, x }) => {
          const t = hourly_temp[slot];
          const y = yForTemp(t);
          const below = y <= midY; // upper half (hotter) → label below curve
          return (
            <div
              key={`tl${slot}`}
              style={{
                position: 'absolute',
                left: x - 30,
                top: below ? y + 12 : y - 32,
                width: 60,
                fontSize: 20,
                fontWeight: 700,
                color: FG,
                display: 'flex',
                justifyContent: 'center',
              }}
            >
              {String(t)}
            </div>
          );
        })}
        {/* Sunrise/sunset time labels — placed in the DAY portion of the strip,
            just outside the marker (before a sunset, after a sunrise) so they
            never sit on top of the night ribbon, vertically centered to it. */}
        {sunMarks.map(({ key, x, label }) => {
          const isSunset = key === 'sunset';
          return (
            <div
              key={`sun-lbl-${key}`}
              style={{
                position: 'absolute',
                left: isSunset ? x - SUN_LABEL_PAD - SUN_LABEL_W : x + SUN_LABEL_PAD,
                top: -SUN_STRIP_H,
                width: SUN_LABEL_W,
                height: SUN_STRIP_H,
                fontSize: 18,
                fontWeight: 700,
                color: FG,
                display: 'flex',
                alignItems: 'center',
                justifyContent: isSunset ? 'flex-end' : 'flex-start',
              }}
            >
              {label}
            </div>
          );
        })}
      </div>

      <AxisLabels chartW={chartW} updated={updated} n={n} />
    </div>
  );
}

function Footer({ data }) {
  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'row',
        alignItems: 'center',
        padding: `4px ${PAGE_PADDING_X}px 8px ${PAGE_PADDING_X}px`,
      }}
    >
      {/* TODO: moon phase, sunrise/sunset could go here in future */}

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
      <ForecastChart
        data={data}
        hasRain={data.rain_chance.some((p) => p >= PRECIP_THRESHOLD)}
        hasSnow={data.snow_chance.some((p) => p >= PRECIP_THRESHOLD)}
      />
      <Footer data={data} />
    </div>
  );
}
