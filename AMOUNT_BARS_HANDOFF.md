# Handoff: amount-based rain bars

Context + locked design + exact prototype code for the next work session. This was
prototyped in a prior conversation, reviewed, then **reverted** so a separate
change (the nowcast) could ship first. Nothing here is currently in the code —
this doc is the reapplication plan.

**Design revised 2026-07-15** in a follow-up session with the user: linear
height mapping (sqrt dropped) with a min-height floor, `RAIN_FULL_MM = 7.6`,
3 discrete probability shade buckets instead of a continuous ramp, and snow as
outlined bars. §3–§5 and §9 reflect the current agreement; the §4 code was
updated to match (it now deviates from the original prototype).

**IMPLEMENTED 2026-07-15** (same session, working tree — not yet committed or
deployed): all §4 changes applied, plus the §6 nowcast mm blending, plus one
further user decision — the snow outline is **dotted** (`strokeDasharray="3 3"`),
which also keeps a snow bar's top edge from reading as the temp line. Both §5
checks rendered and passed (results inline in §5). New fixtures:
`weather-sample-amountbars.json` (showcase: 3 amount sweeps × 3 shade buckets +
outlined snow group → `preview-amountbars.png`) and `weather-sample-stress.json`
(§5 worst case → `preview-stress.png`); the older rain-bearing fixtures gained
`hourly_rain_mm`/`hourly_snow_mm` arrays. Transform verified against a
synthetic OWM payload (nowcast max-blend for hour 0 / minutely buckets /
sub-floor trace / snow passthrough). Remaining before ship: §8 deploy + KV
cache bust, then eyeball the served PNG (dasharray on the CF-edge resvg) and
the physical panel.

Related memory (background + decision history): `rain-representation-work.md` in
the project memory dir.

---

## 1. Why this change

The chart's rain bars are driven by OWM `hourly.pop` (probability of ≥0.1 mm of
precip). Two problems for the user's real goal ("never leave without an umbrella
when I should have, without too many false positives"):

- **pop over-represents trace rain**: OWM reported 98–100% pop for a day whose
  total was 0.14" (a near-certain drizzle looks identical to a downpour, because
  bar height = probability). Other services (Google, Open-Meteo) showed "no
  rain" for the same hours.
- The umbrella decision really lives in **2 dimensions — chance × amount** — and
  bar height was spending the strongest visual channel on just `pop`.

Decision: make bar **height = amount** (how much water) and **shade =
probability** (how likely), so "certain but trivial" and "heavy but iffy" become
visually distinct. Provider was kept as OWM (it gives both signals; a provider
that only hands you a pre-blended % removes the tuning control the user wants).

## 2. Current baseline (what's in the code now)

Original bars: **height = `pop` (rain_chance/snow_chance)**, rain fill fixed
`#ccc`, snow fill fixed `#666`. In `worker/renderer/src/layout.jsx`,
`ForecastChart()`:

```js
const rainBars = hasRain ? rain_chance.map((pct, i) => {
  const barH = (pct / 100) * usableH;
  return { x: barX(i), y: inset + usableH - barH, w: barSlotW, h: barH };
}) : [];
```

The transform (`worker/src/providers/openweathermap.js`) currently discards the
per-hour precip volume — it only reads `h.rain?.['1h']` to decide rain-vs-snow
attribution, then throws it away. It keeps the *daily* total (`rain_in`).

The **nowcast** already shipped (commit `7317cda`, on master + prod): for the
current/next hour(s) where `current`/`minutely` show observed/imminent precip,
it sets `rain_chance`/`snow_chance` to 100. Today that drives full bar HEIGHT
(pop-height baseline). When bars become amount-based, the nowcast must ALSO feed
an amount into `hourly_rain_mm` for those buckets, or nowcast rain will show
zero-height bars — see §6. There is a code comment in `transform()` marking the
spot.

## 3. The locked design (revised 2026-07-15 with the user — supersedes the prototype)

- **Height = amount, linear.** Fixed ceiling `RAIN_FULL_MM = 7.6` mm/h — the US
  NWS/AMS "heavy rain" onset (0.30 in/h; for reference: UK Met Office calls
  steady rain heavy at 4 mm/h, Dark Sky's chart topped out ~10). Clamp above →
  full bar: "officially heavy; everything worse is just a lot of rain."
  Mapping is LINEAR — the prototype's sqrt curve was dropped 2026-07-15 because
  it re-inflated trace rain (0.5 mm/h drizzle took 26% of the 160px bar; linear
  gives it an honest 7%). Trace visibility is handled by a `MIN_BAR_PX = 4`
  floor for any nonzero amount, not by a curve. If linear feels too timid after
  living with it, try exponent 0.7 before going back to sqrt.
- **Shade = 3 discrete probability buckets**, not a continuous ramp (a ramp is
  false precision on a 16-level panel with grainy fills): low chance `#ddd`,
  likely `#bbb`, definitely `#999`. All ≥2 4bpp steps (0x22) apart so adjacent
  buckets stay distinguishable. Boundaries (user-confirmed 2026-07-15): <40%
  low, 40–75% likely, >75% definitely.
- **Snow = the same 3 shade buckets + a black 2px DOTTED outline**
  (`strokeDasharray="3 3"`, `crispEdges`), retiring the fixed `#666` snow fill.
  One probability legend for both precip types; outline-vs-solid carries the
  categorical rain/snow distinction, and the dotted (rather than solid) stroke
  keeps a snow bar's top edge from being mistaken for the temp line. The
  transform attributes each hour to rain OR snow (never both), so the two only
  meet side-by-side on transition days.
- **No numeric Y axis** — temperature already owns the vertical scale; a second
  numeric axis is the dual-axis trap. Convey absolute amount via the existing
  status text ("Heavy rain this evening") and optionally faint reference lines.

OWM detail: `rain.1h`/`snow.1h` are **always mm** even under `units=imperial`
(the units param only affects temp/wind). So 7.5 is mm/h; no conversion.

## 4. Exact prototype code to reapply

**A. `worker/src/providers/openweathermap.js` — plumb amounts through transform.**
Add the two arrays + pushes inside the 24-hour loop, and to the return object:

```js
// (declare alongside rain_chance/snow_chance)
const hourly_rain_mm = [];
const hourly_snow_mm = [];
// (inside the loop, after computing rainVol/snowVol)
hourly_rain_mm.push(Math.round(rainVol * 100) / 100);
hourly_snow_mm.push(Math.round(snowVol * 100) / 100);
// (in the returned object, next to rain_chance/snow_chance)
hourly_rain_mm,
hourly_snow_mm,
```

**B. `worker/src/providers/base.js` — document the contract:**

```
*   hourly_rain_mm: number[24] (mm/h per hour, rain volume),
*   hourly_snow_mm: number[24] (mm/h per hour, snow volume),
```

**C. `worker/renderer/src/layout.jsx` — constants (near BORDER):**

```js
const RAIN_FULL_MM = 7.6; // NWS/AMS "heavy rain" onset (0.30 in/h); clamp above
const MIN_BAR_PX = 4;     // floor so any nonzero amount stays visible

const precipBarH = (mm, usableH) => {
  if (!(mm > 0)) return 0;
  return Math.max(MIN_BAR_PX, Math.min(1, mm / RAIN_FULL_MM) * usableH);
};

// 3 discrete probability buckets (≥2 4bpp steps apart); boundaries tunable
const precipShade = (pop) => (pop > 75 ? '#999' : pop >= 40 ? '#bbb' : '#ddd');
```

**D. `layout.jsx` `ForecastChart()` — destructure the new fields:**

```js
const {
  hourly_temp, rain_chance, snow_chance,
  hourly_rain_mm = [], hourly_snow_mm = [],
  updated,
} = data;
```

**E. `layout.jsx` — bar geometry (height by amount, shade by pop):**

```js
const rainBars = hasRain ? rain_chance.map((pct, i) => {
  const barH = precipBarH(hourly_rain_mm[i] || 0, usableH);
  return { x: barX(i), y: inset + usableH - barH, w: barSlotW, h: barH, fill: precipShade(pct) };
}) : [];
const snowBars = hasSnow ? snow_chance.map((pct, i) => {
  const barH = precipBarH(hourly_snow_mm[i] || 0, usableH);
  return { x: barX(i), y: inset + usableH - barH, w: barSlotW, h: barH, fill: precipShade(pct) };
}) : [];
```

**F. `layout.jsx` — rect fills:** rain rect `fill="#ccc"` → `fill={bar.fill}`.
Snow rect `fill="#666"` → `fill={bar.fill}` PLUS `stroke="#000"`,
`strokeWidth={2}`, `shape-rendering="crispEdges"` — the outline is what marks
"snow" now.

> Gate decision (2026-07-15): a bar is drawn only when **pop ≥ PRECIP_THRESHOLD
> (5) AND the hour's mm > 0** — no `MIN_BAR_PX` stubs for probability-only
> hours. Implementation: the series-level `hasRain`/`hasSnow` (`status.js`)
> keeps its pop gate unchanged; the per-hour amount condition falls out of
> `precipBarH` returning 0 for mm ≤ 0 (the floor only applies to nonzero
> amounts). Nowcast hours pass the amount condition once §6 plumbs their mm.

## 5. Former blockers — resolved by the 2026-07-15 design, verify before shipping

The original pause reasons were (1) certain-rain `#66` colliding with the snow
fill and (2) the temp line/labels/gridlines drowning in dark bars. Both
dissolve under the revised §3 design: the darkest rain shade is now `#999`
(nothing dark for labels to fight; `#333` labels have 6 4bpp steps of
contrast) and snow is marked by outline rather than by owning the dark range.
The white-halo work should NOT be needed. Still verify before shipping:

1. ✅ **STRESS render** (done 2026-07-15 via `weather-sample-stress.json` →
   `preview-stress.png`: 24×100%-pop, 10 mm/h bars): the black temp line and
   `#333` labels read clearly on the `#999` wall — no halos needed. Gridlines
   DO vanish inside the fill (minor is the same `#999`; major `#888` is 1 step
   off) — accepted: the hour labels below the axis still anchor time, and this
   only occurs inside a full wall of certain heavy rain.
2. ✅ **Faint-bucket snow at low heights** (seen in `preview-rain-snow.png`:
   0.1 mm trace snow → 4px dotted-outline stub): reads as a faint dotted box —
   acceptable, slightly busy; if real mixed days look noisy, bump `MIN_BAR_PX`
   for snow or drop dots below some height.
3. Still to verify AFTER deploy: `strokeDasharray` on the CF-edge resvg (this
   chart lives in the nested-svg-as-data-URI path that has diverged from local
   preview before) and dot crispness on the physical panel.

## 6. Nowcast interaction

When amount bars land, update the nowcast block in `transform()` so observed/
imminent precip also produces a bar HEIGHT. Right now it only sets
`rain_chance[i] = 100`; add an amount into `hourly_rain_mm[i]` for those buckets.
Otherwise a nowcast hour would be certain-shaded but zero-height (and, per the
§4 gate decision, not drawn at all).

**Confirmed with user 2026-07-15: use the real observed/minutely mm** (no floor,
no fixed nominal value): hour 0 → `Math.max(existing, current.rain['1h'])`;
imminent hours → that bucket's `minutely` precipitation converted to mm/h.
Honest heights from the same data that triggered the nowcast — an observed
drizzle renders small, an observed downpour renders tall.

## 7. How to preview with live/real data

- Debug endpoint (always live, no cache): `GET /weather/{zip}.json` on
  `https://weather-esp32.leigh-herbert.workers.dev` returns the transformed data
  the renderer gets. Note: `hourly_rain_mm` only appears here once §4A is done
  AND deployed — for *local* preview use the harness below.
- **Local preview needs a subscribed OWM One Call 3.0 key.** The `.dev.vars` key
  in `worker/` is NOT subscribed (returns 401 on One Call 3.0). Ask the user for
  a key (they have the production secret).
- Recreate a throwaway harness at `worker/renderer/src/preview-live.jsx` (it was
  deleted during cleanup). Minimal version:

```js
import { writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { renderSvg } from './render.jsx';
import { rgbaToGrayscalePng } from './png-encode.js';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const WORKER = 'https://weather-esp32.leigh-herbert.workers.dev';
const zip = process.argv[2] || '10010';

let data;
if (process.env.STRESS) {                       // legibility worst-case
  data = await (await fetch(`${WORKER}/weather/${zip}.json`)).json();
  data.rain_chance = data.rain_chance.map(() => 100);
  data.hourly_rain_mm = data.rain_chance.map(() => 10);   // > RAIN_FULL_MM
  data.snow_chance = data.snow_chance.map(() => 0);
  data.hourly_snow_mm = data.snow_chance.map(() => 0);
} else if (process.env.OWM_KEY) {               // real current OWM data
  const { OpenWeatherMapProvider } = await import('../../src/providers/openweathermap.js');
  const locs = await (await fetch(`${WORKER}/admin/data`)).json();
  const loc = locs.locations.find((l) => l.zip === zip);
  data = await new OpenWeatherMapProvider(process.env.OWM_KEY, `${loc.lat},${loc.lon}`).fetch();
} else {                                        // estimate amounts from daily total
  data = await (await fetch(`${WORKER}/weather/${zip}.json`)).json();
  const totalMm = (data.rain_in || 0) * 25.4;
  const popSum = data.rain_chance.reduce((a, b) => a + b, 0) || 1;
  data.hourly_rain_mm = data.rain_chance.map((p) => Math.round(totalMm * (p / popSum) * 100) / 100);
  data.hourly_snow_mm = data.snow_chance.map(() => 0);
}
const { svg, width, height, pixels } = await renderSvg(data, { location: zip });
await writeFile(join(ROOT, 'preview-live.png'), rgbaToGrayscalePng(width, height, pixels));
console.log('rain_mm:', JSON.stringify(data.hourly_rain_mm));
```

Run from `worker/renderer`: `OWM_KEY=<key> npm exec tsx src/preview-live.jsx 10010`
(or `STRESS=1 …` for the legibility case). Writes `preview-live.png`. **Always
write previews to a persistent repo file** (don't render to /tmp and delete
same-turn) so the user can see them.

## 8. Deploy (only with user's OK)

Worker imports the same `renderer/src/layout.jsx`, so one deploy covers both
preview and device. Procedure (wrangler v3.114, colon syntax):

1. `cd worker && npx wrangler deploy`
2. Bust cache (deploys don't refresh served PNGs) — delete `render_png:{zip}` AND
   `render_hash:{zip}` for each zip (10010, 11211), namespace
   `bd1e4ef949604fcc8a02eb116c755dbc`:
   `npx wrangler kv:key delete --namespace-id bd1e4ef949604fcc8a02eb116c755dbc "render_png:10010"`
3. Verify: `GET /weather/{zip}.json` (live) + download the PNG.
4. Git: branch off master, commit, PR (repo: `KingLeigh/weather-esp32`).

## 9. Design decisions (2026-07-15 session) + remaining questions

Resolved with the user 2026-07-15:

- `RAIN_FULL_MM = 7.6` (NWS/AMS heavy-rain onset, 0.30 in/h). Fallback if bars
  feel chronically short in practice: drop toward 4–5 (Met Office heavy
  threshold) — single-constant change.
- Curve: **linear**, with a `MIN_BAR_PX = 4` floor for trace visibility (the
  sqrt curve re-inflated drizzle; see §3). If too timid, try exponent 0.7.
- Probability: 3 discrete shade buckets (`#ddd`/`#bbb`/`#999`), not a
  continuous ramp.
- Independent height+shade confirmed (vs a blended pop×amount "expected
  rainfall" bar) — keeps both signals.
- Snow: same shade buckets + black outline (see §3); `#666` retired.
- **Bucket boundaries CONFIRMED: <40% low / 40–75% likely / >75% definitely.**
  (The shipped nowcast sets pop=100, so nowcast hours land in "definitely"
  automatically.)
- **Gate CONFIRMED: pop AND amount** — series-level pop gate unchanged, per-hour
  bars require mm > 0 (see the §4 note).
- **Nowcast amounts CONFIRMED: real observed/minutely mm**, no floor or nominal
  value (see §6).

Still open / deferred:

- Umbrella-verdict glyph from chance × amount (trace floor + downpour
  override). User said "good idea, not yet."

## 10. Key files

- `worker/src/providers/openweathermap.js` — transform (+ shipped nowcast block)
- `worker/src/providers/base.js` — transformed-data contract doc
- `worker/renderer/src/layout.jsx` — `ForecastChart()`, colors, bar rendering
- `worker/renderer/src/status.js` — `PRECIP_THRESHOLD = 5`, chart status text
- `worker/renderer/src/preview.js` — sample-based preview (`npm run preview`)
