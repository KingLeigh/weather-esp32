# ESP32 Firmware (`src/`)

Arduino-based firmware for the LilyGo T5 4.7" S3 E-Paper board. Handles WiFi connectivity, weather data fetching, display rendering, and power management via deep sleep.

## How It Works

On each wake cycle, the firmware:

1. Initializes the e-paper display and allocates a framebuffer in PSRAM
2. Connects to WiFi and syncs time via NTP (EST/EDT timezone)
3. Fetches weather JSON from the Cloudflare Worker API
4. Renders all display elements into the framebuffer
5. Pushes the framebuffer to the e-paper display
6. Enters deep sleep for 10 minutes, then repeats from step 1

The e-paper retains its image while the board sleeps, so the display stays visible with near-zero power draw.

### Display Rendering

The display is 960x540 pixels with 4-bit grayscale (16 shades). The framebuffer is `EPD_WIDTH * EPD_HEIGHT / 2` bytes (each byte holds two pixels as nibbles). Drawing is done into the framebuffer using EPD47 library primitives, then flushed to the display in a single operation.

### Fonts

Three font sizes are used, all generated from FiraSans Bold:
- **FiraSans** (built-in, ~20px) -- timestamps, labels, sunrise/sunset
- **FiraSansMedium** (~72px) -- high/low temps, UV high
- **FiraSansLarge** (~120px) -- current temperature, UV current

The medium and large fonts contain only digits, degree symbol, and a few letters to minimize flash usage.

### Color Palette

All grayscale values are defined as named constants in `colors.h`:

| Constant | Value | Usage |
|---|---|---|
| `COLOR_BLACK` | `0x00` | Text, chart lines |
| `COLOR_DARK` | `0x40` | Major time markers |
| `COLOR_ICON` | `0x50` | Icons, battery fill |
| `COLOR_MEDIUM` | `0x60` | Minor time markers |
| `COLOR_OUTLINE` | `0xA0` | Battery outline, chart baseline |
| `COLOR_GRIDLINE` | `0xC0` | Chart gridlines |
| `COLOR_FILL` | `0xD0` | Chart area fill |
| `COLOR_WHITE` | `0xFF` | Background |

## Files

### `main.cpp`

Application entry point. Contains `setup()` and `loop()`, plus all display layout logic.

**Key functions:**

- `setup()` -- Runs on each boot/wake. Initializes hardware, fetches weather, renders display, enters deep sleep.
- `loop()` -- Contains a delay-based update loop with change detection, used when deep sleep is disabled (for testing).
- `render_display(weather, age_str, battery_percent)` -- Composes all visual elements into the framebuffer and pushes to the e-paper.
- `weather_data_changed(old, new, old_battery, new_battery)` -- Compares two weather snapshots field-by-field to determine if the display needs refreshing. Used in the non-deep-sleep loop path.
- `read_battery_percent()` -- Reads battery voltage from ADC and converts to 0-100%.
- `get_data_age_minutes(timestamp)` -- Parses an ISO 8601 timestamp and calculates how many minutes old the data is.
- `format_data_age(age_minutes, output, output_size)` -- Formats data age as a human-readable string (e.g., "35m", "1h 23m"). Returns empty string if data is fresh (under 30 minutes).
- `draw_battery_icon(x, y, percent, fb)` -- Draws a battery outline with proportional fill level.
- `get_moon_phase_bitmap(phase)` -- Maps a moon phase name string (e.g., "Waxing Crescent") to the corresponding bitmap array.
- `init_default_weather(data)` -- Populates a `WeatherData` struct with fallback defaults for when the API fetch fails.

### `weather_fetch.h`

WiFi connection and HTTP weather data fetching.

**Key functions:**

- `connectWiFi()` -- Connects to WiFi using credentials from `wifi_config.h`. Configures custom DNS (Google + Cloudflare) and initiates NTP time sync with EST/EDT timezone.
- `fetchWeatherData(data)` -- Fetches JSON from the weather API endpoint with retry logic (3 attempts, 2-second delay between retries). Parses the JSON response into a `WeatherData` struct.
- `disconnectWiFi()` -- Turns off WiFi radio to save power.
- `parse_weather_icon(weather_str, is_day)` -- Maps a weather condition string (e.g., "sunny", "rainy") to a `WeatherIcon` enum value. Handles day/night variants (sunny becomes moon at night, partly cloudy becomes partly cloudy night).

**Structs:**

- `WeatherData` -- Holds all weather fields: temperatures (current/high/low), weather icon enum, 24-hour precipitation array, precipitation type, UV (current/high), moon phase, sunrise/sunset times, update timestamp, and validity flag.

### `weather_icons.h`

Weather icon rendering and bitmap blitting.

**Key functions:**

- `draw_bitmap(bitmap, cx, cy, size, fb)` -- Draws a 4-bit grayscale bitmap centered at (cx, cy). Used for both weather icons (200x200) and moon phase icons (100x100).
- `draw_weather_icon(icon, cx, cy, fb)` -- Maps a `WeatherIcon` enum to its bitmap array and draws it.
- `draw_uv_icon(cx, cy, fb)` -- Draws a small hollow sun icon with 8 rays, used next to the UV index display.
- `draw_sunrise_icon(cx, cy, fb)` -- Draws a half-sun above a horizon line, used next to sunrise/sunset times.

**Enums:**

- `WeatherIcon` -- `SUNNY`, `MOON`, `CLOUDY`, `PARTLY_CLOUDY`, `PARTLY_CLOUDY_NIGHT`, `RAINY`, `SNOWY`, `THUNDERSTORM`, `FOG`

**Constants:**

- `WEATHER_ICON_SIZE` (200) -- Pixel dimensions of weather icon bitmaps
- `MOON_ICON_SIZE` (100) -- Pixel dimensions of moon phase bitmaps

### `precip_chart.h`

24-hour precipitation probability chart rendering.

**Key functions:**

- `draw_precip_chart(x, y, w, h, data, count, precip_type, fb)` -- Draws a complete precipitation chart: gridlines at 0/25/50/75%, filled area under the curve, line segments connecting data points, a label ("Rain", "Snow", or "Mixed"), and time marker lines.
- `draw_time_marker(target_hour, current_hour, count, chart_x, chart_w, chart_y, chart_bottom, color, thickness, fb)` -- Draws a dotted vertical line at a specific hour of day. Used for midnight/noon (thick, dark) and 6am/6pm (thin, lighter).

### `colors.h`

Named constants for the 4-bit grayscale color palette. Single source of truth for all display colors.

### `weather_icon_bitmaps.h`

Generated file containing 200x200 4-bit grayscale bitmap arrays for 9 weather icons: sun, moon, partly cloudy (day), partly cloudy (night), cloud, rainy, snowflake, lightning, fog. Generated by `tools/convert_icons.py`.

### `moon_phase_bitmaps.h`

Generated file containing 100x100 4-bit grayscale bitmap arrays for 8 moon phases: new, waxing crescent, first quarter, waxing gibbous, full, waning gibbous, last quarter, waning crescent. Generated by `tools/convert_moon_icons.py`.

### `fonts/font_large.h`

Generated FiraSans Bold font at ~120px line height. Contains only digits 0-9, degree symbol, and minus sign.

### `fonts/font_medium.h`

Generated FiraSans Bold font at ~72px line height. Contains digits 0-9, degree symbol, minus sign, and letters H, L.

### `wifi_config.h` / `wifi_config.h.template`

WiFi credentials and API endpoint URL. The template is committed to git; the actual `wifi_config.h` with real credentials is gitignored. Contains `WIFI_SSID`, `WIFI_PASSWORD`, `WEATHER_API_URL`, and `WIFI_TIMEOUT_MS`.
