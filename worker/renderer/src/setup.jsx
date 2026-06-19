// Device Setup screen — shown while the AP/captive-portal is active and the
// firmware draws the real WiFi-join QR over the placeholder. Rendered once
// locally and baked into the firmware (see preview-setup.jsx + a bake step).
//
// Layout intentionally matches the on-device menu (menu.jsx): a "Configuration"
// title at the same position/size with the same divider underneath.
//
// ⚠️ QR PLACEMENT COUPLING: the QR box below MUST match the firmware's QR_AREA
// constants in firmware/src/main.cpp (QR_AREA_X=660, QR_AREA_Y=255,
// QR_AREA_W/H=240). The firmware white-fills that box and centers the real QR
// in it at runtime, so keep this region clear of text.

// eslint-disable-next-line no-unused-vars
import React from 'react';

const WIDTH = 960;
const HEIGHT = 540;

const FG = '#000';
const FG_MUTED = '#333';
const BG = '#fff';

// The firmware draws the real WiFi-join QR on the right at runtime (QR_AREA in
// main.cpp: x=660, y=255, 240×240) directly on blank white space — we do NOT
// draw a placeholder box here, so there's nothing for the QR to misalign
// against. We only reserve the horizontal space so the steps never run under it.
const QR_LEFT = 660; // = firmware QR_AREA_X

// Left column for the numbered steps: from the page margin to a gap before the
// reserved QR region, so the two never overlap.
const STEPS_LEFT = 60;
const STEPS_WIDTH = QR_LEFT - STEPS_LEFT - 40; // 560 — ends at x=620, clear of QR
const NUM_WIDTH = 38;
const STEP_FONT = 32; // smaller than the menu's 44 — there's more text here

const STEPS = [
  'Unlock your phone',
  'Scan the QR code and join the WiFi network',
  'Fill in the setup form on your phone',
];

export function SetupFrame() {
  return (
    <div
      style={{
        width: WIDTH,
        height: HEIGHT,
        background: BG,
        color: FG,
        fontFamily: 'FiraSans',
        display: 'flex',
        position: 'relative',
      }}
    >
      {/* Title — matches the menu screen */}
      <div
        style={{
          position: 'absolute',
          left: 60,
          top: 40,
          fontSize: 64,
          fontWeight: 700,
          color: FG,
        }}
      >
        Device setup
      </div>

      {/* Divider under the title — matches the menu screen */}
      <div
        style={{
          position: 'absolute',
          left: 60,
          top: 124,
          width: WIDTH - 120,
          height: 3,
          background: FG_MUTED,
          display: 'flex',
        }}
      />

      {/* Numbered steps, left column (clear of the QR area) */}
      <div
        style={{
          position: 'absolute',
          left: STEPS_LEFT,
          top: 175,
          width: STEPS_WIDTH,
          display: 'flex',
          flexDirection: 'column',
        }}
      >
        {STEPS.map((text, i) => (
          <div
            key={i}
            style={{
              display: 'flex',
              flexDirection: 'row',
              alignItems: 'flex-start',
              width: STEPS_WIDTH,
              marginBottom: 26,
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
                width: STEPS_WIDTH - NUM_WIDTH,
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

      {/* Right side intentionally left blank — the firmware draws the real
          WiFi-join QR here at runtime (see QR_LEFT note above). */}
    </div>
  );
}
