// On-device menu layout — rendered once locally and baked into the firmware as
// a PNG (see preview-menu.jsx + firmware/scripts/bake-menu.sh). Shown on the
// e-paper when the user long-presses the button to open the menu. The cursor
// arrow is NOT part of this PNG — the firmware draws it on the left, at the
// selected row, over the white cursor column reserved here.
//
// ⚠️ ROW GEOMETRY COUPLING: the row centres below MUST stay in sync with the
// firmware cursor constants in firmware/src/main.cpp:
//   MENU_ROW_Y0  = ROW_Y0 + ROW_H/2   (centre of first row)
//   MENU_ROW_DY  = ROW_H              (row pitch)
//   MENU_ITEM_COUNT = ITEMS.length
// If you change ROW_Y0 / ROW_H / the item list here, update those too.

// eslint-disable-next-line no-unused-vars
import React from 'react';

const WIDTH = 960;
const HEIGHT = 540;

const FG = '#000';
const FG_MUTED = '#333';
const BG = '#fff';

// Row geometry (px). Row i vertical centre = ROW_Y0 + i*ROW_H + ROW_H/2.
const ROW_Y0 = 150; // top of the first row
const ROW_H = 90; // row pitch
const TEXT_X = 110; // left edge of item label; cursor column is to its left (white)

const ITEMS = ['Device setup', 'Debug mode', 'Factory reset', 'Exit menu'];

export function MenuFrame() {
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
      {/* Title */}
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
        Menu
      </div>

      {/* Divider under the title */}
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

      {/* Item rows (absolutely positioned for deterministic Y) */}
      {ITEMS.map((label, i) => (
        <div
          key={i}
          style={{
            position: 'absolute',
            left: 0,
            top: ROW_Y0 + i * ROW_H,
            width: WIDTH,
            height: ROW_H,
            display: 'flex',
            alignItems: 'center',
            paddingLeft: TEXT_X,
          }}
        >
          <div style={{ fontSize: 44, fontWeight: 700, color: FG }}>{label}</div>
        </div>
      ))}
    </div>
  );
}
