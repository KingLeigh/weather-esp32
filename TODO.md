# Weather Display - TODO List

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

---

*Last updated: 2026-02-08*
