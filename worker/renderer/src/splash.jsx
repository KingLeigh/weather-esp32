// Splash screen layout — a pure brand splash. The setup instructions + QR live
// on the dedicated setup screen now (setup.jsx); this screen is shown when the
// device is unconfigured or has fallen back to the splash because of a bad
// state (e.g. can't reach WiFi with no weather to show).
//
// The lower ~120px is deliberately left empty: the firmware draws a contextual
// status message there on-device (onboarding hint, or the failure reason), so
// the baked art stays generic.
//
// Visual style is intentionally consistent with the live weather frame
// (layout.jsx): same FiraSans, same FG/BG, same icon font.

// eslint-disable-next-line no-unused-vars
import React from 'react';

const WIDTH = 960;
const HEIGHT = 540;

const FG = '#000';
const FG_MUTED = '#333';
const BG = '#fff';

const PAGE_PADDING_X = 60;

// Height of the bottom strip reserved for the firmware's on-device status
// message. Keep this clear of the brand block above.
const MESSAGE_STRIP_H = 100;

// Onboarding instructions baked into the splash — same size as the setup
// screen's step list (setup.jsx).
const STEP_FONT = 32;
const NUM_WIDTH = 38;
const LIST_WIDTH = 800;
const SPLASH_STEPS = [
  'Ensure your device is charged.',
  'Long press the white button to enter setup mode.',
];

// Decorative weather-icon strip for the splash. Codepoints are raw Erik
// Flowers Weather Icons glyphs (same font the live frame uses) — chosen for
// visual variety across the full range of forecast conditions.
const DECORATIVE_ICONS = [
  '', // wi_day_sunny
  '', // wi_day_cloudy (partly cloudy)
  '', // wi_cloudy
  '', // wi_rain
  '', // wi_thunderstorm
  '', // wi_snow
];

function IconStrip({ size }) {
  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'row',
        justifyContent: 'space-between',
        alignItems: 'center',
        width: '100%',
      }}
    >
      {DECORATIVE_ICONS.map((cp, i) => (
        <div
          key={i}
          style={{
            fontFamily: 'WeatherIcons',
            fontSize: size,
            lineHeight: 1,
            color: FG_MUTED,
          }}
        >
          {cp}
        </div>
      ))}
    </div>
  );
}

export function SplashFrame() {
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
      {/* Brand block (title + decorative icons), vertically centered in the
          space above the reserved message strip. */}
      <div
        style={{
          display: 'flex',
          flexDirection: 'column',
          flexGrow: 1,
          justifyContent: 'center',
        }}
      >
        {/* Title */}
        <div
          style={{
            display: 'flex',
            justifyContent: 'center',
            padding: `0 ${PAGE_PADDING_X}px`,
          }}
        >
          <div
            style={{ fontSize: 80, fontWeight: 700, color: FG, lineHeight: 1 }}
          >
            What's the Weather?
          </div>
        </div>

        {/* Decorative weather-icon strip beneath the title. Explicit height so
            descenders on rain/snow glyphs don't bleed into the row below. */}
        <div
          style={{
            display: 'flex',
            height: 110,
            marginTop: 8,
            padding: `0 ${PAGE_PADDING_X + 30}px`,
          }}
        >
          <IconStrip size={64} />
        </div>

        {/* Onboarding instructions (centered block, left-aligned items). */}
        <div style={{ display: 'flex', justifyContent: 'center', marginTop: 36 }}>
          <div
            style={{ display: 'flex', flexDirection: 'column', width: LIST_WIDTH }}
          >
            {SPLASH_STEPS.map((text, i) => (
              <div
                key={i}
                style={{
                  display: 'flex',
                  flexDirection: 'row',
                  alignItems: 'flex-start',
                  width: LIST_WIDTH,
                  marginBottom: i < SPLASH_STEPS.length - 1 ? 16 : 0,
                }}
              >
                <div
                  style={{
                    width: NUM_WIDTH,
                    flexShrink: 0,
                    fontSize: STEP_FONT,
                    fontWeight: 700,
                    color: FG,
                  }}
                >
                  {`${i + 1}.`}
                </div>
                <div
                  style={{
                    width: LIST_WIDTH - NUM_WIDTH,
                    fontSize: STEP_FONT,
                    fontWeight: 700,
                    color: FG,
                    lineHeight: 1.25,
                  }}
                >
                  {text}
                </div>
              </div>
            ))}
          </div>
        </div>
      </div>

      {/* Reserved bottom strip — left empty; the firmware draws a contextual
          status message here on-device. */}
      <div style={{ display: 'flex', height: MESSAGE_STRIP_H }} />
    </div>
  );
}
