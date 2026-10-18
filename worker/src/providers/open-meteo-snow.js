/**
 * Open-Meteo snow-report provider — data source for the 'snow' display type.
 *
 * Unlike the WeatherProvider classes this is not stateless: a snow report
 * needs season-long context that no single API call provides, so it keeps two
 * pieces of state in KV alongside the per-call fetch:
 *
 *   1. Per render (1 call/day):  forecast API, past_days=7 + forecast_days=8
 *      → the 15 daily bars (7 actual, today + 7 forecast) and the location's
 *      IANA timezone.
 *
 *   2. snow_total:{zip}  — season-to-date ACTUALS through yesterday.
 *      Normally advanced by appending the past-days bars from the same
 *      forecast call (self-healing for gaps up to 7 days); a larger gap or a
 *      missing key triggers a full re-sync from the archive API, bridged with
 *      the past-days bars across the archive's ~5-day publication lag.
 *
 *   3. snow_baseline:{zip}  — the 15-prior-season median cumulative snowfall
 *      per day-of-season (Oct 1 = day 0), computed from one archive call when
 *      the location is first rendered and recomputed when the season rolls
 *      over. "% of median" is then pure arithmetic at render time.
 *
 * Forecasts are free (Open-Meteo, non-commercial, no key). Snowfall comes
 * back in inches via snowfall_unit=inch. The past-days values are model
 * analysis (assimilated from observations), not stored forecasts — but they
 * are still a grid cell at the resort's elevation, not a snow-stake report,
 * so totals read lower than resort marketing numbers. The %-of-median metric
 * is immune to that bias since both sides come from the same source.
 */

const FORECAST_URL = 'https://api.open-meteo.com/v1/forecast';
const ARCHIVE_URL = 'https://archive-api.open-meteo.com/v1/archive';

const PAST_DAYS = 7;
const FORECAST_DAYS = 8; // today + 7

const SEASON_START_MONTH = 10; // Oct 1
const SEASON_DAYS = 273;       // Oct 1 .. ~Jun 30 — baseline curve length
const BASELINE_SEASONS = 15;   // prior seasons in the median

// Below this median-to-date (inches) the "% of median" is early-season noise;
// the layout renders an em-dash instead.
const MIN_MEDIAN_IN = 5;

const DOW = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

// ─── date helpers (all on 'YYYY-MM-DD' strings, no Date-parsing pitfalls) ───

function parseDay(s) {
  const [y, m, d] = s.split('-').map(Number);
  return Date.UTC(y, m - 1, d);
}

function dayString(utcMs) {
  return new Date(utcMs).toISOString().slice(0, 10);
}

function addDays(s, n) {
  return dayString(parseDay(s) + n * 86400000);
}

function daysBetween(a, b) {
  return Math.round((parseDay(b) - parseDay(a)) / 86400000);
}

function dowOf(s) {
  return DOW[new Date(parseDay(s)).getUTCDay()];
}

/** Season start date ('YYYY-10-01') for the season containing `day`. */
function seasonStartFor(day) {
  const [y, m] = day.split('-').map(Number);
  const year = m >= SEASON_START_MONTH ? y : y - 1;
  return `${year}-10-01`;
}

function median(values) {
  const s = [...values].sort((a, b) => a - b);
  const mid = Math.floor(s.length / 2);
  return s.length % 2 ? s[mid] : (s[mid - 1] + s[mid]) / 2;
}

// ─── API calls ───────────────────────────────────────────────────────────────

async function fetchJson(url) {
  const response = await fetch(url);
  if (!response.ok) {
    const body = await response.text();
    throw new Error(`Open-Meteo error: ${response.status} ${response.statusText} - ${body}`);
  }
  const data = await response.json();
  // Fail loudly if the API ever hands back a unit we don't expect — every
  // downstream number (bars, season totals, the stored baseline) assumes
  // inches, and a silent cm response is exactly the bug this guards against.
  const unit = data.daily_units?.snowfall_sum;
  if (unit !== 'inch') {
    throw new Error(`Open-Meteo returned snowfall_sum in '${unit}', expected 'inch'`);
  }
  return data;
}

/** The daily forecast window: 7 past days + today + 7 forecast days. */
async function fetchForecastWindow(loc) {
  const url = `${FORECAST_URL}?latitude=${loc.lat}&longitude=${loc.lon}` +
    `&elevation=${loc.elevation_ft ? Math.round(loc.elevation_ft * 0.3048) : ''}` +
    `&daily=snowfall_sum&past_days=${PAST_DAYS}&forecast_days=${FORECAST_DAYS}` +
    // NB: precipitation_unit is what converts snowfall_sum to inches — there
    // is NO snowfall_unit parameter (the API silently ignores unknown params
    // and returns cm). Verified against the response's daily_units field.
    `&precipitation_unit=inch&timezone=auto`;
  const data = await fetchJson(url);
  const days = data.daily.time.map((date, i) => ({
    date,
    in: Math.round((data.daily.snowfall_sum[i] || 0) * 10) / 10,
  }));
  return { days, timezone: data.timezone };
}

/** Daily snowfall from the archive, as a {date → inches} map. Days the
 * archive hasn't published yet (~5-day lag) come back null and are omitted,
 * so callers can see exactly how far the record reaches. */
async function fetchArchiveDaily(loc, startDate, endDate) {
  const url = `${ARCHIVE_URL}?latitude=${loc.lat}&longitude=${loc.lon}` +
    `&elevation=${loc.elevation_ft ? Math.round(loc.elevation_ft * 0.3048) : ''}` +
    `&start_date=${startDate}&end_date=${endDate}` +
    `&daily=snowfall_sum&precipitation_unit=inch&timezone=auto`;
  const data = await fetchJson(url);
  const out = {};
  for (let i = 0; i < data.daily.time.length; i++) {
    const v = data.daily.snowfall_sum[i];
    if (v != null) out[data.daily.time[i]] = v;
  }
  return out;
}

// ─── baseline (15-prior-season median curve) ────────────────────────────────

async function getBaseline(env, loc, seasonStart) {
  const key = `snow_baseline:${loc.zip}`;
  const cached = await env.WEATHER_KV.get(key, 'json');
  if (cached && cached.seasonStart === seasonStart) return cached;

  // One archive call covering the 15 prior seasons.
  const startYear = Number(seasonStart.slice(0, 4)) - BASELINE_SEASONS;
  const archive = await fetchArchiveDaily(
    loc,
    `${startYear}-10-01`,
    addDays(seasonStart, -1),
  );

  // Cumulative-by-day-of-season per prior season, then the median across
  // seasons at each day index.
  const curves = [];
  for (let y = startYear; y < Number(seasonStart.slice(0, 4)); y++) {
    const s0 = `${y}-10-01`;
    let cum = 0;
    const curve = new Array(SEASON_DAYS);
    for (let d = 0; d < SEASON_DAYS; d++) {
      cum += archive[addDays(s0, d)] || 0;
      curve[d] = cum;
    }
    curves.push(curve);
  }
  const medians = new Array(SEASON_DAYS);
  for (let d = 0; d < SEASON_DAYS; d++) {
    medians[d] = Math.round(median(curves.map((c) => c[d])) * 10) / 10;
  }

  const baseline = { seasonStart, medians };
  await env.WEATHER_KV.put(key, JSON.stringify(baseline));
  return baseline;
}

// ─── season-to-date total (actuals through yesterday) ───────────────────────

async function getSeasonTotal(env, loc, seasonStart, today, pastDays) {
  const key = `snow_total:${loc.zip}`;
  const yesterday = addDays(today, -1);
  let state = await env.WEATHER_KV.get(key, 'json');

  // A stored total is usable if it's this season's and the past-days window
  // can bridge it to yesterday (gap ≤ PAST_DAYS). Otherwise re-sync from the
  // archive: it reaches to within ~5 days of now, and the same past-days
  // window bridges the rest.
  const usable = state && state.seasonStart === seasonStart &&
    daysBetween(state.through, yesterday) <= PAST_DAYS;

  if (!usable) {
    let total = 0;
    let through = addDays(seasonStart, -1); // nothing counted yet
    if (yesterday >= seasonStart) {
      const archive = await fetchArchiveDaily(loc, seasonStart, yesterday);
      for (let d = seasonStart; d <= yesterday; d = addDays(d, 1)) {
        if (archive[d] == null) break; // hit the publication lag
        total += archive[d];
        through = d;
      }
    }
    state = { seasonStart, total, through };
  }

  // Append from the forecast call's past days (model analysis): everything
  // after `through`, up to yesterday. Days the window can't see (shouldn't
  // happen given the `usable` check) stay uncounted rather than guessed.
  const byDate = Object.fromEntries(pastDays.map((d) => [d.date, d.in]));
  for (let d = addDays(state.through, 1); d <= yesterday; d = addDays(d, 1)) {
    if (byDate[d] == null) break;
    state.total += byDate[d];
    state.through = d;
  }
  state.total = Math.round(state.total * 10) / 10;

  await env.WEATHER_KV.put(key, JSON.stringify(state));
  return state;
}

// ─── public API ──────────────────────────────────────────────────────────────

/**
 * Assemble the full snow-report data for a 'snow' location (the SnowFrame
 * layout contract — see renderer/src/snow-layout.jsx).
 */
export async function fetchSnowReport(env, loc) {
  const { days, timezone } = await fetchForecastWindow(loc);

  // Location-local today: index PAST_DAYS of the window (defensive fallback
  // to Intl if the API ever changes its window semantics).
  const today = days[PAST_DAYS]?.date ??
    new Intl.DateTimeFormat('en-CA', { timeZone: timezone }).format(new Date());
  const seasonStart = seasonStartFor(today);

  const pastDays = days.filter((d) => d.date < today);
  const [baseline, totalState] = await Promise.all([
    getBaseline(env, loc, seasonStart),
    getSeasonTotal(env, loc, seasonStart, today, pastDays),
  ]);

  // Median over the same elapsed window: days fully in the books, i.e.
  // seasonStart .. yesterday inclusive = `elapsed` days = curve index
  // elapsed-1. Out of range (pre-season, deep summer) → null → "—".
  const elapsed = daysBetween(seasonStart, today);
  const medianToDate =
    elapsed >= 1 && elapsed <= baseline.medians.length
      ? baseline.medians[elapsed - 1]
      : null;
  const pct =
    medianToDate != null && medianToDate >= MIN_MEDIAN_IN
      ? Math.round((totalState.total / medianToDate) * 100)
      : null;

  // Location-local render timestamp, same format as the weather provider's
  // `updated` (feeds X-Updated and the date-only slice in the render hash).
  const fmt = new Intl.DateTimeFormat('en-CA', {
    timeZone: timezone,
    year: 'numeric', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
  });
  const parts = fmt.formatToParts(new Date());
  const get = (type) => parts.find((p) => p.type === type)?.value || '00';
  const updated = `${get('year')}-${get('month')}-${get('day')}T${get('hour')}:${get('minute')}:${get('second')}`;

  return {
    type: 'snow',
    resort: loc.label || loc.zip,
    elevation_ft: loc.elevation_ft || 0,
    date: today,
    timezone,
    season_total_in: Math.round(totalState.total),
    season_median_in: medianToDate != null ? Math.round(medianToDate) : null,
    pct_of_typical: pct,
    bars: days.map((d) => ({
      date: d.date,
      dow: dowOf(d.date),
      in: d.in,
      forecast: d.date >= today,
    })),
    updated,
  };
}

/** KV keys owned by this provider for a location (cleanup on remove). */
export function snowStateKeys(zip) {
  return [`snow_baseline:${zip}`, `snow_total:${zip}`, `snow_rendered:${zip}`];
}
