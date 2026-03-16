# Weather Display for ESP32 E-Paper

An always-on weather display built on the LilyGo T5 4.7" S3 E-Paper board (ESP32-S3, 960x540 pixels, 4-bit grayscale). The device connects to WiFi, fetches weather data from a Cloudflare Worker backend, renders the display, and enters deep sleep to conserve battery. It wakes every 10 minutes to refresh.

## Overview

The system has two components:

1. **ESP32 Firmware** (`src/`) -- Arduino-based firmware that drives the e-paper display. Connects to WiFi, fetches a JSON weather payload from the backend, renders temperature, weather icon, precipitation chart, UV index, moon phase, sunrise/sunset, and battery level onto the 960x540 display.

2. **Cloudflare Worker** (`worker/`) -- A serverless backend that fetches weather data from WeatherAPI.com every 15 minutes via cron trigger, caches it in KV storage, and serves a simplified JSON response to the ESP32.

## What the Display Shows

- Current temperature (large font)
- High/low temperatures for the day
- Weather condition icon (sun, cloud, rain, snow, thunderstorm, fog, moon, partly cloudy)
- UV index (current and daily high)
- 24-hour precipitation probability chart with time markers at midnight, 6am, noon, 6pm
- Moon phase icon (8 phases)
- Sunrise and sunset times
- Battery level indicator
- Data staleness warning (shown when data is older than 30 minutes)

## Project Structure

```
weather-claude/
├── src/             ESP32 firmware source code
├── worker/          Cloudflare Worker backend
├── tools/           Icon conversion and testing utilities
├── platformio.ini   PlatformIO build configuration
├── preview.py       Python script to preview the display layout as PNG
└── TODO.md          Project roadmap
```

### `src/` -- ESP32 Firmware

The main firmware that runs on the LilyGo T5 4.7" S3 board. Built with PlatformIO and the Arduino framework. Uses the LilyGo-EPD47 library to drive the e-paper display. See [`src/README.md`](src/README.md) for details.

### `worker/` -- Cloudflare Worker Backend

A serverless API that proxies and caches weather data from WeatherAPI.com. Runs on Cloudflare's edge network with KV storage for caching. See [`worker/README.md`](worker/README.md) for details.

### `tools/` -- Utilities

Python scripts for converting PNG icon assets into C header files containing 4-bit grayscale bitmap arrays, plus an ESP32 test program for previewing icons on the physical display. See [`tools/README.md`](tools/README.md) for details.

### `platformio.ini`

PlatformIO build configuration. Targets the `esp32-s3-devkitc-1` board with the espressif32 platform and Arduino framework. Configures PSRAM, USB CDC, and pulls in the LilyGo-EPD47 and ArduinoJson libraries.

### `preview.py`

A Python (Pillow) script that renders a simulated version of the display layout as a PNG image. Useful for iterating on layout without flashing the board. Run with `python3 preview.py` (normal layout) or `python3 preview.py icons` (icon gallery).

## Getting Started

### Prerequisites

- [PlatformIO CLI](https://platformio.org/install/cli)
- A LilyGo T5 4.7" S3 E-Paper board
- A Cloudflare account (for the Worker backend)
- A [WeatherAPI.com](https://www.weatherapi.com/) API key

### ESP32 Setup

1. Copy the WiFi config template and fill in your credentials:
   ```
   cp src/wifi_config.h.template src/wifi_config.h
   ```

2. Build and flash:
   ```
   pio run -t upload --upload-port /dev/cu.usbmodem2101
   ```

3. Monitor serial output:
   ```
   pio device monitor --port /dev/cu.usbmodem2101 --baud 115200
   ```

### Worker Setup

1. Install dependencies:
   ```
   cd worker && npm install
   ```

2. Set your API key:
   ```
   wrangler secret put WEATHER_API_KEY
   ```

3. Deploy:
   ```
   wrangler deploy
   ```
