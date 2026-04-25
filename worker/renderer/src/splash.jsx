// Splash screen layout — rendered once locally and saved as a PNG artifact
// that gets baked into the firmware. Shown on the e-paper when the device has
// no network connection or hasn't been configured yet.
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
const COLUMN_GAP = 40;
const QR_SIZE = 240;

// Decorative weather-icon strip for the splash. Codepoints are raw Erik
// Flowers Weather Icons glyphs (same font the live frame uses) — chosen for
// visual variety across the full range of forecast conditions.
const DECORATIVE_ICONS = [
  '\uf00d', // wi_day_sunny
  '\uf002', // wi_day_cloudy (partly cloudy)
  '\uf013', // wi_cloudy
  '\uf019', // wi_rain
  '\uf01e', // wi_thunderstorm
  '\uf01b', // wi_snow
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

// Square placeholder for the eventual onboarding QR code. Dashed border with
// a "QR" label so it's visually obvious this is intentional empty space.
// Once we know what the QR encodes (captive portal URL?), swap this for a
// real qrcode-svg render.
function QRPlaceholder({ size }) {
  return (
    <div
      style={{
        width: size,
        height: size,
        position: 'relative',
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
        <rect
          x={2}
          y={2}
          width={size - 4}
          height={size - 4}
          fill="none"
          stroke={FG_MUTED}
          strokeWidth={3}
          strokeDasharray="12 10"
        />
      </svg>
      <div
        style={{
          fontSize: 56,
          fontWeight: 700,
          color: FG_MUTED,
          letterSpacing: 4,
        }}
      >
        QR
      </div>
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
      {/* Top: centered title */}
      <div
        style={{
          display: 'flex',
          justifyContent: 'center',
          padding: `30px ${PAGE_PADDING_X}px 0 ${PAGE_PADDING_X}px`,
        }}
      >
        <div
          style={{
            fontSize: 80,
            fontWeight: 700,
            color: FG,
            lineHeight: 1,
          }}
        >
          What's the Weather?
        </div>
      </div>

      {/* Decorative weather-icon strip beneath the title. Explicit height
          so descenders on rain/snow glyphs don't bleed into the bottom row. */}
      <div
        style={{
          display: 'flex',
          height: 100,
          padding: `20px ${PAGE_PADDING_X + 30}px 20px ${PAGE_PADDING_X + 30}px`,
        }}
      >
        <IconStrip size={56} />
      </div>

      {/* Bottom: two columns — instructions on left, QR on right */}
      <div
        style={{
          display: 'flex',
          flexDirection: 'row',
          alignItems: 'center',
          flexGrow: 1,
          padding: `0 ${PAGE_PADDING_X}px`,
        }}
      >
        {/* Left column: instructions */}
        <div
          style={{
            display: 'flex',
            flexDirection: 'column',
            flexGrow: 1,
            marginRight: COLUMN_GAP,
          }}
        >
          <div
            style={{
              fontSize: 32,
              fontWeight: 600,
              color: FG_MUTED,
              lineHeight: 1.1,
            }}
          >
            Setup required:
          </div>
          <div
            style={{
              fontSize: 36,
              fontWeight: 700,
              color: FG,
              lineHeight: 1.25,
              marginTop: 14,
            }}
          >
            1. Connect to USB power
          </div>
          <div
            style={{
              fontSize: 36,
              fontWeight: 700,
              color: FG,
              lineHeight: 1.25,
              marginTop: 6,
            }}
          >
            2. Scan QR code with phone
          </div>
        </div>

        {/* Right column: QR placeholder */}
        <QRPlaceholder size={QR_SIZE} />
      </div>
    </div>
  );
}
