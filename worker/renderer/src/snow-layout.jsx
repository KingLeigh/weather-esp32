// The UI for the snow-report display type, authored as JSX + flexbox CSS.
// Shared between the Worker (worker/src/render.jsx) and the local preview
// (src/preview-snowreport.jsx), exactly like layout.jsx is for the weather
// display. All of layout.jsx's Satori quirks apply here too — explicit
// display:flex, stringified numbers, and a REQUIRED viewBox on the nested
// <svg> (without it the Cloudflare-edge resvg renders the chart ~2.4% narrow,
// drifting it away from the outer-positioned labels; invisible locally).
//
// Data contract (produced by worker/src/providers/open-meteo-snow.js and by
// the checked-in snow-sample.json fixture):
//
//   {
//     resort: string,              // display name, e.g. "Breckenridge"
//     elevation_ft: number,
//     date: 'YYYY-MM-DD',          // location-local today
//     season_total_in: number,     // actuals through YESTERDAY only
//     season_median_in: number,    // 15-prior-season median, same elapsed window
//     pct_of_typical: number|null, // season_total / median × 100; null → "—"
//     bars: [{ date, dow, in, forecast }]   // 15 days: 7 back, today, 7 fwd;
//   }                                       // today is forecast:true (a morning
//                                           // render is mostly forecast)
//
// Design decisions (settled against real Breck data, 2026-10-18):
//   - Solid dark bars = snow that fell; dotted-outline light bars = expected.
//     Today is dotted, and marked only by its bold weekday label.
//   - Chart scale is FIXED at 12" so a quiet week can't inflate a 1.8"
//     dusting into a "big storm"; it expands only when a day exceeds 12".
//   - Hero: season total (actuals) + "% of median" as equal-size stats.

// eslint-disable-next-line no-unused-vars
import React from 'react';

const WIDTH = 960;
const HEIGHT = 540;

// Match layout.jsx so the two display types feel like siblings.
const PAGE_PADDING_X = 40;
const CONTENT_W = WIDTH - 2 * PAGE_PADDING_X;
const FG = '#000';
const FG_MUTED = '#333';
const GRIDLINE = '#888';

// Bar fills: actual = dark, forecast = light with a dotted outline ("not real
// yet"). All native 4bpp levels.
const BAR_PAST = '#444';
const BAR_FORECAST = '#ccc';

const MIN_BAR_PX = 3; // trace day stays visible

// ─── chart ───────────────────────────────────────────────────────────────────

function SnowChart({ bars }) {
  const chartH = 190;
  const inset = 2;
  const usableH = chartH - 2 * inset;
  const n = bars.length;
  const slotW = CONTENT_W / n;
  const barW = Math.round(slotW * 0.72);

  // Scale: FIXED 12" ceiling so bar height has stable meaning week to week —
  // a quiet fortnight must not auto-zoom a 1.8" dusting into a "big storm".
  // Only a day genuinely over 12" expands the scale (snapped to the gridline
  // step), so nothing ever clips. Gridlines every 4" keep the chart airy; the
  // per-bar value labels carry the exact numbers.
  const GRID_STEP = 4;
  const SCALE_FLOOR_IN = 12;
  const maxIn = Math.max(
    SCALE_FLOOR_IN,
    Math.ceil(Math.max(...bars.map((b) => b.in)) / GRID_STEP) * GRID_STEP,
  );
  const hFor = (inches) =>
    inches > 0 ? Math.max(MIN_BAR_PX, (inches / maxIn) * usableH) : 0;

  const geom = bars.map((b, i) => {
    const h = hFor(b.in);
    return {
      ...b,
      x: Math.round(i * slotW + (slotW - barW) / 2),
      y: inset + usableH - h,
      h,
      isToday: i === Math.floor(n / 2),
    };
  });

  const gridlines = [];
  for (let v = GRID_STEP; v <= maxIn; v += GRID_STEP) {
    const y = inset + usableH - (v / maxIn) * usableH;
    gridlines.push(
      <line key={`g${v}`} x1={0} y1={y} x2={CONTENT_W} y2={y}
        stroke={GRIDLINE} strokeWidth={1} shapeRendering="crispEdges" />
    );
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', padding: `0 ${PAGE_PADDING_X}px` }}>
      <div style={{ display: 'flex', position: 'relative', width: CONTENT_W, height: chartH }}>
        <svg width={CONTENT_W} height={chartH} viewBox={`0 0 ${CONTENT_W} ${chartH}`}
          style={{ position: 'absolute', left: 0, top: 0 }}>
          {gridlines}
          {geom.map((b, i) => (
            b.h > 0 && (b.forecast ? (
              // Forecast: light fill, dotted outline open at the bottom
              // (same open-path trick as the weather chart's snow bars).
              <path key={`f${i}`}
                d={`M ${b.x} ${b.y + b.h} L ${b.x} ${b.y} L ${b.x + barW} ${b.y} L ${b.x + barW} ${b.y + b.h}`}
                fill={BAR_FORECAST} stroke="#000" strokeWidth={2}
                strokeDasharray="3 3" shapeRendering="crispEdges" />
            ) : (
              <rect key={`a${i}`} x={b.x} y={b.y} width={barW} height={b.h}
                fill={BAR_PAST} shapeRendering="crispEdges" />
            ))
          ))}
          <line x1={0} y1={chartH - 1} x2={CONTENT_W + 1} y2={chartH - 1}
            stroke={FG} strokeWidth={2} shapeRendering="crispEdges" />
        </svg>
        {/* Per-bar value labels (divs, not svg <text>) */}
        {geom.map((b, i) => (
          b.in >= 1 && (
            <div key={`v${i}`} style={{
              position: 'absolute',
              left: b.x + barW / 2 - 30, top: b.y - 30, width: 60,
              display: 'flex', justifyContent: 'center',
              fontSize: 24, fontWeight: 700, color: FG_MUTED,
            }}>
              {String(Math.round(b.in))}
            </div>
          )
        ))}
      </div>
      {/* Day-of-week labels; today's is bold (its bar is also the first dotted
          one, so the solid/dotted boundary locates "now" as well). */}
      <div style={{ display: 'flex', flexDirection: 'row', width: CONTENT_W, height: 32 }}>
        {geom.map((b, i) => (
          <div key={`d${i}`} style={{
            display: 'flex', width: slotW, justifyContent: 'center',
            fontSize: 22, color: b.isToday ? FG : FG_MUTED,
            fontWeight: b.isToday ? 700 : 400, paddingTop: 6,
          }}>
            {b.dow}
          </div>
        ))}
      </div>
    </div>
  );
}

// ─── hero ────────────────────────────────────────────────────────────────────

// `size` sets the number's font size; the unit scales with it.
function Stat({ value, unit, label, size = 96 }) {
  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'flex-end' }}>
      <div style={{ display: 'flex', flexDirection: 'row', alignItems: 'flex-end' }}>
        <div style={{ fontSize: size, fontWeight: 700, color: FG, lineHeight: 1 }}>{value}</div>
        <div style={{ fontSize: Math.round(size * 0.42), fontWeight: 700, color: FG_MUTED, paddingBottom: 6, paddingLeft: 6 }}>{unit}</div>
      </div>
      <div style={{ fontSize: 28, color: FG_MUTED, paddingTop: 6 }}>{label}</div>
    </div>
  );
}

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

export function SnowFrame({ data }) {
  const [, m, d] = data.date.split('-').map(Number);
  const dateStr = `${MONTHS[m - 1]} ${d}`;
  const elevStr = `${data.elevation_ft.toLocaleString('en-US')} ft`;
  // Early-season / missing-baseline guard: render an em-dash rather than a
  // nonsense percentage when the median-to-date isn't meaningful yet.
  const pctStr = data.pct_of_typical == null ? '—' : String(data.pct_of_typical);

  return (
    <div style={{
      display: 'flex', flexDirection: 'column',
      width: WIDTH, height: HEIGHT, backgroundColor: '#fff',
      fontFamily: 'FiraSans',
    }}>
      <div style={{
        display: 'flex', flexDirection: 'row', justifyContent: 'space-between',
        padding: `36px ${PAGE_PADDING_X}px 0 ${PAGE_PADDING_X}px`, height: 250,
      }}>
        <div style={{ display: 'flex', flexDirection: 'column' }}>
          <div style={{ fontSize: 64, fontWeight: 700, color: FG, lineHeight: 1.05 }}>
            {data.resort}
          </div>
          <div style={{ fontSize: 30, color: FG_MUTED, paddingTop: 10 }}>
            {`${elevStr} · ${dateStr}`}
          </div>
        </div>
        <div style={{ display: 'flex', flexDirection: 'row' }}>
          <div style={{ display: 'flex', paddingRight: 56 }}>
            <Stat value={String(data.season_total_in)} unit={'in'} label={'this season'} size={96} />
          </div>
          <Stat value={pctStr} unit={'%'} label={'of median'} size={96} />
        </div>
      </div>
      <SnowChart bars={data.bars} />
    </div>
  );
}
