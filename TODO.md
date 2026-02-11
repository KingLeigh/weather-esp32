# Weather Display - TODO List

## Icon Improvements
- [ ] **Night mode icons** - Add moon and moon+cloud icons for nighttime weather
  - Need new icon designs for nighttime conditions
  - Update API to detect day/night and return appropriate icons
  - Add MOON and MOON_CLOUDY to WeatherIcon enum

## Display Modes
- [ ] **Full night mode** - Black pixels with white text for nighttime display
  - Invert all colors during night hours
  - Easier on the eyes in dark environments
  - Need to determine night hours (based on time or sunset/sunrise data?)

- [ ] **Horizontal mode** - Rotate display 90° for landscape orientation
  - Redesign layout for 540x960 (landscape)
  - May provide better use of screen real estate

## Time & Clock Features
- [x] **NTP time synchronization** - Sync time via NTP to calculate data freshness
  - NTP syncs during WiFi connection (every 5 minutes)
  - POSIX timezone string handles EST/EDT automatically
  - Data age shown only when stale (> 30 minutes)
- [ ] **Clock display** - Show current time on the display
  - NTP infrastructure is already in place
  - Add a visible clock element to the layout

## Visual Updates
- [ ] **UV Index icon update** - Replace small primitive sun with better design
  - Currently uses simple circle with rays
  - Could use a bitmap icon to match new weather icons

- [ ] **Layout tweaks** - General positioning/spacing improvements
  - TBD: Specific adjustments needed after testing other features

---

*Last updated: 2026-02-08*
