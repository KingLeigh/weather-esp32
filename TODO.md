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
- [ ] **NTP time synchronization and clock display** - Add real-time clock to the display
  - Sync time via NTP during WiFi connection (happens every 5 minutes)
  - Maintain time between updates using ESP32 RTC
  - Display current time in addition to (or instead of) "last updated" timestamp
  - Could enable time-based features like automatic night mode based on actual time

## Visual Updates
- [ ] **UV Index icon update** - Replace small primitive sun with better design
  - Currently uses simple circle with rays
  - Could use a bitmap icon to match new weather icons

- [ ] **Layout tweaks** - General positioning/spacing improvements
  - TBD: Specific adjustments needed after testing other features

---

*Last updated: 2026-02-08*
