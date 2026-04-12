# Weather Renderer

Server-side renderer for the e-paper weather display. Takes a normalized
`WeatherData` JSON and produces a 960×540 PNG.

## Status

**Step 1 of 3.** Local preview only — no Cloudflare Worker integration yet, no
firmware changes yet. The existing `worker/` endpoint and the ESP32 firmware
are untouched and still work as before.

## Quick start

```
cd worker/renderer
npm install          # installs deps and runs fetch-fonts.js as postinstall
npm run preview      # renders preview.png from weather-sample.json
open preview.png     # or inspect however you like
```

Edit `src/layout.jsx` and re-run `npm run preview` to iterate. Nothing talks
to a weather API in step 1 — all data comes from `weather-sample.json`.

## Project layout

```
worker/renderer/
├── package.json
├── tsconfig.json
├── weather-sample.json    # test data matching the provider JSON shape
├── scripts/
│   └── fetch-fonts.js     # one-time download of FiraSans TTFs
├── fonts/                 # populated by fetch-fonts (gitignored)
└── src/
    ├── layout.jsx         # the UI — the file you edit
    ├── render.jsx         # pure fn: (data) → PNG bytes
    └── preview.js         # Node entry: read sample, render, write PNG
```

## Toolchain

- **[Satori](https://github.com/vercel/satori)** turns JSX + a flexbox CSS
  subset into an SVG string.
- **[@resvg/resvg-js](https://github.com/yisibl/resvg-js)** rasterizes the SVG
  to PNG. Step 2 swaps this for `@resvg/resvg-wasm` so it runs on a Cloudflare
  Worker with no other changes.
- **tsx** runs `.jsx` directly, so there's no build step.

## Satori CSS gotchas

A few things to remember while editing `layout.jsx`:

1. Any element with **two or more children** needs an explicit
   `display: 'flex'` style. Satori will throw otherwise.
2. Mixing raw text and child elements inside the same parent is not allowed —
   wrap text in its own `<div>`.
3. No CSS grid, no pseudo-elements, no media queries. Flexbox and absolute
   positioning only.
4. Fonts must be loaded explicitly — there are no system fonts or web fetches.

## Reserved overlay region

The bottom-right corner of the layout has a dashed rectangle labeled
"FIRMWARE OVERLAY". The server never draws inside that box — it's the region
the ESP32 will fill in locally with its battery icon and stale-age indicator
once step 3 lands. Don't put server-rendered content in there.

## Next steps

- **Step 2**: move the render function into the Cloudflare Worker, add a new
  `/weather.png` endpoint, cache PNGs in R2 keyed by location, leave
  `/weather.json` untouched so existing firmware keeps working.
- **Step 3**: replace most of `src/main.cpp` with a simple fetch-decode-blit
  loop using `PNGdec`, keep the battery/stale-age overlay code.
