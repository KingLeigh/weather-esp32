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
        padding: `32px ${PAGE_PADDING_X}px 20px ${PAGE_PADDING_X}px`,
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

// Compute chart x positions and labels for every 6-hour clock boundary
// (00:00, 06:00, 12:00, 18:00) in the 24-hour window starting at nowHour.
// Each hour occupies exactly chartW/24 pixels.
function computeAxisLabels(nowHour, chartW) {
  const slotW = chartW / 24;

  const labelForHour = (h) => {
    const hh = ((h % 24) + 24) % 24;
    if (hh === 0) return '00';
    if (hh === 6) return '6a';
    if (hh === 12) return '12';
    if (hh === 18) return '6p';
    return hh < 12 ? `${hh}a` : `${hh - 12}p`;
  };

  const out = [];
  // First 6-hour boundary strictly after nowHour.
  let h = Math.floor(nowHour / 6) * 6 + 6;
  while (h < nowHour + 24) {
    const slot = h - nowHour;
    const x = slot * slotW;
    const hh = h % 24;
    const isMajor = hh === 0 || hh === 12;
    out.push({ x, label: labelForHour(hh), isMajor });
    h += 6;
  }
  return out;
}

// Threshold: show precip chart if any hour has >= this % chance.
const PRECIP_THRESHOLD = 5;

// Width of each hour-axis label tile, centered on its tick.
const AXIS_LABEL_W = 50;

// ─── shared axis labels (used by both chart types) ──────────────────────────

function AxisLabels({ chartW, updated }) {
  const nowHour = parseLocalHour(updated);
  const axisLabels = computeAxisLabels(nowHour, chartW);

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
      {axisLabels.map(({ x, label: lbl }) => (
        <div
          key={lbl}
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

// ─── shared chart gridlines (vertical lines at 6-hour boundaries) ───────────

const GRIDLINE_MAJOR = '#888';  // midnight, noon — one shade darker
const GRIDLINE_MINOR = '#999';  // everything else (6am/6pm, horizontals)

function chartGridlines(chartW, chartH, updated, yTop, yBottom) {
  // yTop / yBottom define the drawable region within the chart; gridlines
  // are clipped to this range so they intersect cleanly with horizontal
  // reference lines. Defaults to full chart height if not provided.
  const y1 = yTop ?? 0;
  const y2 = yBottom ?? chartH - 4;
  const nowHour = parseLocalHour(updated);
  const labels = computeAxisLabels(nowHour, chartW);

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

// ─── temperature chart ──────────────────────────────────────────────────────

function TempChart({ data }) {
  const { hourly_temp, updated } = data;
  const chartW = CONTENT_W;
  const chartH = 164;
  const n = hourly_temp.length;
  const strokeW = 3;

  // Auto-scale: round outward to the nearest step so gridlines land on
  // round numbers. Use 5° steps for narrow ranges, 10° for wider ones.
  // The rounded scale provides natural breathing room above and below
  // the data since scaleMin < minTemp and scaleMax > maxTemp.
  const minTemp = Math.min(...hourly_temp);
  const maxTemp = Math.max(...hourly_temp);
  const rawRange = maxTemp - minTemp;
  const step = rawRange <= 10 ? 5 : 10;
  const scaleMin = Math.floor(minTemp / step) * step;
  const scaleMax = Math.ceil(maxTemp / step) * step;
  const range = scaleMax - scaleMin || 1;

  // Small inset so gridlines at 0% and 100% aren't clipped at SVG edges.
  const inset = 2;
  const usableH = chartH - 2 * inset;

  const yForTemp = (t) => inset + usableH - ((t - scaleMin) / range) * usableH;

  const points = hourly_temp.map((t, i) => {
    const x = n === 1 ? 0 : (i / (n - 1)) * chartW;
    const y = yForTemp(t);
    return [x, y];
  });

  const lineStr = points.map(([x, y]) => `${x},${y}`).join(' ');

  // Find the center of the longest continuous run for high and low temps,
  // so the label sits in the middle of a plateau rather than at its left edge.
  const findRunCenter = (arr, value) => {
    let bestStart = 0, bestLen = 0;
    let start = -1, len = 0;
    for (let i = 0; i < arr.length; i++) {
      if (arr[i] === value) {
        if (start === -1) start = i;
        len++;
        if (len > bestLen) { bestStart = start; bestLen = len; }
      } else {
        start = -1; len = 0;
      }
    }
    return bestStart + (bestLen - 1) / 2;
  };

  const highCenter = findRunCenter(hourly_temp, maxTemp);
  const lowCenter = findRunCenter(hourly_temp, minTemp);

  // Interpolate x position for fractional indices.
  const xForIdx = (idx) => {
    if (n === 1) return 0;
    return (idx / (n - 1)) * chartW;
  };
  // Interpolate y position for fractional indices.
  const yForIdx = (idx) => {
    const floor = Math.floor(idx);
    const ceil = Math.min(floor + 1, n - 1);
    const frac = idx - floor;
    const t = hourly_temp[floor] * (1 - frac) + hourly_temp[ceil] * frac;
    return yForTemp(t);
  };

  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'column',
        padding: `10px ${PAGE_PADDING_X}px 0 ${PAGE_PADDING_X}px`,
      }}
    >
      <div
        style={{
          display: 'flex',
          flexDirection: 'row',
          alignItems: 'baseline',
          color: FG_MUTED,
          marginBottom: 18,
        }}
      >
        <div style={{ fontSize: 24, fontWeight: 600, letterSpacing: 1.5 }}>{'24H TEMP'}</div>
        <div style={{ marginLeft: 'auto', letterSpacing: 0, fontSize: 42, fontWeight: 700 }}>
          {'No umbrella needed!'}
        </div>
      </div>

      <div
        style={{
          display: 'flex',
          position: 'relative',
          width: chartW,
          height: chartH,
          borderBottom: `2px solid ${BORDER}`,
        }}
      >
        <svg width={chartW} height={chartH} style={{ position: 'absolute', left: 0, top: 0 }}>
          {chartGridlines(chartW, chartH, updated, inset, inset + usableH)}
          {/* Horizontal reference lines at each step increment */}
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
          <polyline
            points={lineStr}
            stroke={FG}
            strokeWidth={strokeW}
            fill="none"
          />
        </svg>
        {/* High temp label — below the peak (more space there) */}
        <div
          style={{
            position: 'absolute',
            left: xForIdx(highCenter) - 30,
            top: yForIdx(highCenter) + 12,
            width: 60,
            fontSize: 20,
            fontWeight: 700,
            color: FG,
            display: 'flex',
            justifyContent: 'center',
          }}
        >
          {`${maxTemp}°`}
        </div>
        {/* Low temp label — above the trough (more space there) */}
        <div
          style={{
            position: 'absolute',
            left: xForIdx(lowCenter) - 30,
            top: yForIdx(lowCenter) - 32,
            width: 60,
            fontSize: 20,
            fontWeight: 700,
            color: FG_MUTED,
            display: 'flex',
            justifyContent: 'center',
          }}
        >
          {`${minTemp}°`}
        </div>
      </div>

      <AxisLabels chartW={chartW} updated={updated} />
    </div>
  );
}

// ─── precipitation chart ────────────────────────────────────────────────────

function PrecipChart({ data, hasRain, hasSnow }) {
  const { rain_chance, snow_chance, updated } = data;
  const chartW = CONTENT_W;
  const chartH = 164;
  const n = rain_chance.length;
  const padTop = 2;
  const padBottom = 2;
  const usableH = chartH - padTop - padBottom;

  // Build touching bars for rain (light grey) and snow (dark grey).
  const barW = chartW / n;
  const rainBars = rain_chance.map((pct, i) => {
    const barH = (pct / 100) * usableH;
    return { x: i * barW, y: padTop + usableH - barH, w: barW, h: barH };
  });
  const snowBars = snow_chance.map((pct, i) => {
    const barH = (pct / 100) * usableH;
    return { x: i * barW, y: padTop + usableH - barH, w: barW, h: barH };
  });

  // Chart title: adapt to what's showing.
  const label = hasSnow && hasRain ? '24H PRECIP %'
    : hasSnow ? '24H SNOW %'
    : '24H RAIN %';

  // Right-aligned summary describing the precipitation outlook.
  //
  // Logic per type (rain / snow):
  //   - Currently active → "Rain until Xpm" (find first hour that drops below
  //     threshold). If it never drops, just "Rain now".
  //   - Not active yet → "Rain from Xpm" (first hour above threshold).
  // Amounts appended as "· X.XX" total" when > 0.

  const formatHour = (hourOffset) => {
    const nowH = parseLocalHour(updated);
    const h24 = (nowH + hourOffset) % 24;
    const h12 = h24 === 0 ? 12 : h24 > 12 ? h24 - 12 : h24;
    const ap = h24 < 12 ? 'am' : 'pm';
    return `${h12}${ap}`;
  };

  const describePrecip = (chances, label) => {
    const isNow = chances[0] >= PRECIP_THRESHOLD;
    if (isNow) {
      // Find when it stops (first hour below threshold).
      const stopIdx = chances.findIndex((p) => p < PRECIP_THRESHOLD);
      if (stopIdx === -1) return `${label} now`;
      return `${label} until ${formatHour(stopIdx)}`;
    }
    // Find when it starts.
    const startIdx = chances.findIndex((p) => p >= PRECIP_THRESHOLD);
    return `${label} from ${formatHour(startIdx)}`;
  };

  let summary = '';
  if (!hasRain && !hasSnow) {
    summary = 'No precipitation for 24 hrs';
  } else if (hasRain && hasSnow) {
    // Both present — keep it short, the chart shows timing visually.
    const totalIn = (data.rain_in || 0) + (data.snow_in || 0);
    summary = totalIn > 0 ? `Rain + Snow · ${String(totalIn)}" total` : 'Rain + Snow';
  } else {
    // Single type — room for full timing description.
    const desc = hasRain
      ? describePrecip(rain_chance, 'Rain')
      : describePrecip(snow_chance, 'Snow');
    const amount = hasRain ? data.rain_in : data.snow_in;
    summary = amount > 0 ? `${desc} · ${String(amount)}" total` : desc;
  }

  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'column',
        padding: `10px ${PAGE_PADDING_X}px 0 ${PAGE_PADDING_X}px`,
      }}
    >
      <div
        style={{
          display: 'flex',
          flexDirection: 'row',
          alignItems: 'baseline',
          color: FG_MUTED,
          marginBottom: 18,
        }}
      >
        <div style={{ fontSize: 24, fontWeight: 600, letterSpacing: 1.5 }}>{label}</div>
        {summary && (
          <div style={{ marginLeft: 'auto', letterSpacing: 0, fontSize: 42, fontWeight: 700 }}>
            {summary}
          </div>
        )}
      </div>

      {/* Chart area: touching bars, one per hour. Rain is light grey,
          snow is darker grey drawn on top. crispEdges avoids AA noise on
          the flat fills (see e-paper rendering lessons in project memory). */}
      <div
        style={{
          display: 'flex',
          width: chartW,
          height: chartH,
          borderBottom: `2px solid ${BORDER}`,
        }}
      >
        <svg width={chartW} height={chartH}>
          {chartGridlines(chartW, chartH, updated, padTop, padTop + usableH)}
          {/* Horizontal reference lines at 25%, 50%, 75%, 100% */}
          {[25, 50, 75, 100].map((pct) => {
            const y = padTop + usableH - (pct / 100) * usableH;
            return (
              <line
                key={`h${pct}`}
                x1={0}
                y1={y}
                x2={chartW}
                y2={y}
                stroke={GRIDLINE_MINOR}
                strokeWidth={1}
                shapeRendering="crispEdges"
              />
            );
          })}
          {/* Rain bars (light grey, behind) */}
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
          {/* Snow bars (dark grey, on top) */}
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
        </svg>
      </div>

      <AxisLabels chartW={chartW} updated={updated} />
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
      {(() => {
        const hasRain = data.rain_chance.some((p) => p >= PRECIP_THRESHOLD);
        const hasSnow = data.snow_chance.some((p) => p >= PRECIP_THRESHOLD);
        return (hasRain || hasSnow)
          ? <PrecipChart data={data} hasRain={hasRain} hasSnow={hasSnow} />
          : <TempChart data={data} />;
      })()}
      <Footer data={data} />
    </div>
  );
}
