// Status message system for the chart title.
//
// Each *provider* inspects the normalized weather data and either offers a
// candidate status ({ key, text }) or returns null. `selectStatus` gathers the
// candidates and returns the single highest-priority one, per the PRIORITY
// ranking below. Providers own their own (dynamic) text; the ranking is the
// only thing that decides which one wins.
//
// To add a status: write a provider that returns { key, text } | null, add it
// to PROVIDERS, and slot its key into PRIORITY at the right spot.

// ── shared weather helpers ──────────────────────────────────────────────────

// Extract the hour from an ISO-ish timestamp like "2026-04-11T14:30:00".
// Minutes are ignored — the chart and these statuses align to hourly buckets.
export function parseLocalHour(updated) {
  const m = /T(\d{2})/.exec(updated ?? '');
  return m ? parseInt(m[1], 10) : 0;
}

// Minimum hourly probability (%) to treat precipitation as "present" — used
// both for drawing the chart bars (layout.jsx) and for the precip status text.
export const PRECIP_THRESHOLD = 5;

// ── priority ranking (highest first) ────────────────────────────────────────
// The chart has room for exactly one status; when several apply, the one whose
// key sits earliest in this list wins. Re-rank by reordering; add a status by
// inserting its key. (Moon and other statuses will slot in here later.)
const PRIORITY = ['precip_today', 'precip_tomorrow'];

// ── providers: (data) => { key, text } | null ───────────────────────────────

// Precipitation outlook — confidence-aware wording (NWS-style) with natural
// time periods and a daily-total suffix. Emits `precip_today` or
// `precip_tomorrow` based on when it starts. Covers rain and snow; the text
// carries the type. Returns null when there's nothing worth saying.
function precipStatus(data) {
  const { rain_chance, snow_chance, updated } = data;
  const hasRain = rain_chance.some((p) => p >= PRECIP_THRESHOLD);
  const hasSnow = snow_chance.some((p) => p >= PRECIP_THRESHOLD);
  if (!hasRain && !hasSnow) return null;

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

  // Confidence thresholds aligned with NWS probability-to-language mappings:
  //   < 20% not mentioned · 20-50% "Chance of rain" · 60-70% "Rain likely" ·
  //   80%+ "Rain"/"Raining". Ref: weather.gov/media/pah/WeatherEducation/pop.pdf
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

  // When both rain and snow qualify, describe whichever is most imminent; the
  // chart still shows both bar types.
  const rainResult = hasRain ? describeSingleType(rain_chance, 'rain', 'Raining') : null;
  const snowResult = hasSnow ? describeSingleType(snow_chance, 'snow', 'Snowing') : null;

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

  if (!result) return null;

  const showTotal = result.peak > 50 && amount > 0 && !result.isTomorrow;
  const text = showTotal ? `${result.desc} · ${String(amount)}" today` : result.desc;
  return { key: result.isTomorrow ? 'precip_tomorrow' : 'precip_today', text };
}

// ── registry + selection ─────────────────────────────────────────────────────

const PROVIDERS = [precipStatus];

// Return the highest-priority applicable status ({ key, text }), or null.
export function selectStatus(data) {
  const rank = (c) => {
    const i = PRIORITY.indexOf(c.key);
    return i === -1 ? Infinity : i; // unknown keys sort last
  };
  return (
    PROVIDERS.map((fn) => fn(data))
      .filter(Boolean)
      .sort((a, b) => rank(a) - rank(b))[0] ?? null
  );
}
