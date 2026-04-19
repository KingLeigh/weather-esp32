// Generates an interactive HTML page for tuning WEATHER_ICONS y-offsets.
// Inlines the Erik Flowers font as base64 so the page works standalone.
//
// Run with: `npm run tune:icons`
// Open:     icon-tuner.html in a browser

import { readFile, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { WEATHER_ICONS } from './layout.jsx';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');
const WI_FONT = join(
  ROOT,
  'node_modules',
  'weathericons',
  'font',
  'weathericons-regular-webfont.ttf',
);
const OUT = join(ROOT, 'icon-tuner.html');

// Build a list of icon cases matching the production preview logic.
function buildCases() {
  const out = [];
  for (const [weather, entry] of Object.entries(WEATHER_ICONS)) {
    if (entry.day === entry.night) {
      out.push({ key: weather, weather, isDay: true, label: weather, codepoint: entry.day, variant: null });
    } else {
      out.push({ key: `${weather}:day`,   weather, isDay: true,  label: `${weather} (day)`,   codepoint: entry.day,   variant: 'day' });
      out.push({ key: `${weather}:night`, weather, isDay: false, label: `${weather} (night)`, codepoint: entry.night, variant: 'night' });
    }
  }
  return out;
}

// Pre-compute the scale each icon uses (matching resolveIconScale).
function resolveScale(entry, isDay) {
  const DEFAULT = 0.8;
  if (!entry) return DEFAULT;
  if (isDay === false && entry.nightScale != null) return entry.nightScale;
  if (isDay !== false && entry.dayScale != null) return entry.dayScale;
  if (entry.scale != null) return entry.scale;
  return DEFAULT;
}

// Pre-compute starting yOffset for each icon (matching resolveIconYOffset).
function resolveYOffset(entry, isDay) {
  if (!entry) return 0;
  if (isDay === false && entry.nightYOffset != null) return entry.nightYOffset;
  if (isDay !== false && entry.dayYOffset != null) return entry.dayYOffset;
  if (entry.yOffset != null) return entry.yOffset;
  return 0;
}

const cases = buildCases();
const fontBytes = await readFile(WI_FONT);
const fontBase64 = fontBytes.toString('base64');

// Initial state: each icon's current scale + yOffset, plus raw entry keys
// so we can emit exactly the same keys (scale / dayScale / nightScale / etc.).
const initialState = cases.map((c) => {
  const entry = WEATHER_ICONS[c.weather];
  return {
    ...c,
    scale: resolveScale(entry, c.isDay),
    yOffset: resolveYOffset(entry, c.isDay),
  };
});

const ICON_SIZE = 180; // match the Hero size

const html = `<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Icon Y-Offset Tuner</title>
  <style>
    @font-face {
      font-family: 'WeatherIcons';
      src: url(data:font/ttf;base64,${fontBase64}) format('truetype');
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; margin: 24px; color: #222; }
    h1 { margin-bottom: 8px; }
    .subtitle { color: #666; margin-bottom: 16px; }

    .controls { background: #fffbeb; border: 1px solid #fbbf24; border-radius: 8px;
                padding: 12px 16px; margin-bottom: 16px; font-size: 14px; }
    .controls strong { color: #b45309; }

    .step-picker { display: inline-flex; gap: 4px; margin-left: 12px; }
    .step-picker button { padding: 4px 10px; border: 1px solid #ccc; background: #fff;
                          cursor: pointer; border-radius: 4px; font-size: 13px; }
    .step-picker button.active { background: #2563eb; color: white; border-color: #2563eb; }

    .grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 16px; margin-bottom: 24px; }

    .cell { background: #f8f8f8; border: 1px solid #ddd; border-radius: 8px;
            padding: 12px; }
    .cell .title { font-weight: 600; font-size: 14px; margin-bottom: 8px; text-align: center; }
    .cell .canvas { position: relative; width: 260px; height: 260px; margin: 0 auto;
                    display: flex; align-items: center; justify-content: center; }

    /* Red box marks the 180x180 expected bounds, matching Hero usage */
    .cell .bbox { position: absolute; width: ${ICON_SIZE}px; height: ${ICON_SIZE}px;
                  border: 2px solid #e33; top: 50%; left: 50%; transform: translate(-50%, -50%);
                  pointer-events: none; }

    .cell .icon-container { width: ${ICON_SIZE}px; height: ${ICON_SIZE}px;
                            display: flex; align-items: center; justify-content: center;
                            position: relative; }
    .cell .icon { font-family: 'WeatherIcons'; line-height: 1; color: #000;
                  display: flex; align-items: center; justify-content: center; }

    .cell .actions { display: flex; gap: 6px; margin-top: 8px; justify-content: center; align-items: center; }
    .cell .nudge { padding: 4px 12px; border: 1px solid #ccc; background: #fff;
                   cursor: pointer; border-radius: 4px; font-size: 16px; font-weight: 700; }
    .cell .nudge:hover { background: #e8e8e8; }
    .cell .offset-val { font-family: monospace; min-width: 44px; text-align: center; font-size: 13px; color: #555; }
    .cell .reset { padding: 4px 8px; border: none; background: transparent; color: #2563eb;
                   cursor: pointer; font-size: 12px; text-decoration: underline; }

    .output { background: #111; color: #0f0; font-family: monospace; padding: 16px;
              border-radius: 8px; font-size: 13px; line-height: 1.6;
              white-space: pre; overflow-x: auto;
              position: sticky; bottom: 0; }
    .output-header { color: #aaa; font-size: 12px; margin-bottom: 6px;
                     display: flex; justify-content: space-between; align-items: center; }
    .output-header button { background: #2563eb; color: white; border: none; padding: 4px 10px;
                            border-radius: 4px; cursor: pointer; font-size: 12px; }
  </style>
</head>
<body>
  <h1>Icon Y-Offset Tuner</h1>
  <p class="subtitle">
    Nudge each icon up/down with the ↑ ↓ buttons. The red box is the 180×180 Hero bounding box.
    When you're happy, copy the output below and paste it back to Claude.
  </p>

  <div class="controls">
    <strong>Step size:</strong>
    <div class="step-picker">
      <button data-step="1">±1</button>
      <button data-step="2" class="active">±2</button>
      <button data-step="5">±5</button>
      <button data-step="10">±10</button>
    </div>
  </div>

  <div class="grid" id="grid"></div>

  <div class="output">
    <div class="output-header">
      <span>Copy this block and paste it back into the chat:</span>
      <button onclick="copyOutput()">Copy</button>
    </div>
    <div id="output"></div>
  </div>

<script>
  const cases = ${JSON.stringify(initialState)};
  const ICON_SIZE = ${ICON_SIZE};

  // Mutable state — initialized from current layout.jsx values.
  const state = {};
  for (const c of cases) {
    state[c.key] = { yOffset: c.yOffset, initial: c.yOffset };
  }

  let step = 2;

  function render() {
    const grid = document.getElementById('grid');
    grid.innerHTML = '';
    for (const c of cases) {
      const s = state[c.key];
      const fontPx = Math.round(ICON_SIZE * c.scale);
      const cell = document.createElement('div');
      cell.className = 'cell';
      cell.innerHTML = \`
        <div class="title">\${c.label}</div>
        <div class="canvas">
          <div class="bbox"></div>
          <div class="icon-container">
            <div class="icon" style="font-size: \${fontPx}px; margin-top: \${s.yOffset}px;">\${c.codepoint}</div>
          </div>
        </div>
        <div class="actions">
          <button class="nudge" data-key="\${c.key}" data-dir="-1">↑</button>
          <div class="offset-val">\${s.yOffset > 0 ? '+' : ''}\${s.yOffset}px</div>
          <button class="nudge" data-key="\${c.key}" data-dir="1">↓</button>
          <button class="reset" data-key="\${c.key}">reset</button>
        </div>
      \`;
      grid.appendChild(cell);
    }
    renderOutput();
  }

  function renderOutput() {
    const lines = [];
    for (const c of cases) {
      const s = state[c.key];
      if (s.yOffset === 0) continue;
      const key = c.variant === 'day' ? 'dayYOffset'
                : c.variant === 'night' ? 'nightYOffset'
                : 'yOffset';
      lines.push(\`  \${c.weather}: { \${key}: \${s.yOffset} },\`);
    }
    const text = lines.length === 0
      ? '// No offsets set.'
      : lines.join('\\n');
    document.getElementById('output').textContent = text;
  }

  document.addEventListener('click', (e) => {
    if (e.target.matches('.nudge')) {
      const key = e.target.dataset.key;
      const dir = parseInt(e.target.dataset.dir);
      state[key].yOffset += dir * step;
      render();
    }
    if (e.target.matches('.reset')) {
      const key = e.target.dataset.key;
      state[key].yOffset = state[key].initial;
      render();
    }
    if (e.target.matches('.step-picker button')) {
      step = parseInt(e.target.dataset.step);
      document.querySelectorAll('.step-picker button').forEach((b) =>
        b.classList.toggle('active', parseInt(b.dataset.step) === step));
    }
  });

  function copyOutput() {
    const text = document.getElementById('output').textContent;
    navigator.clipboard.writeText(text);
  }

  render();
</script>
</body>
</html>
`;

await writeFile(OUT, html);
console.log(`Wrote ${OUT} (${html.length} bytes, ${cases.length} icons)`);
