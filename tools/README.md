# Development Tools (`tools/`)

Python scripts for converting PNG icon assets into C header files, plus an ESP32 test program for previewing icons on the physical display.

## How It Works

The e-paper display uses 4-bit grayscale bitmaps (16 shades, 2 pixels per byte). Icons are designed as standard PNG files, then converted into C `const uint8_t[]` arrays that get compiled directly into the firmware. This avoids runtime image decoding on the ESP32.

The conversion pipeline:
1. Design icons as PNG files (any size, with transparency)
2. Run a converter script to resize, grayscale-convert, and pack into nibble-packed byte arrays
3. Output is a `.h` file included by the firmware at compile time

## Files

### `convert_icons.py`

Converts weather icon PNGs (200x200) into `src/weather_icon_bitmaps.h`.

**Key function:**

- `png_to_epd47_bitmap(png_path, output_size)` -- Loads a PNG, resizes to `output_size` x `output_size` with Lanczos resampling, converts to grayscale (compositing RGBA over white), quantizes to 4-bit (0-15), packs pairs of pixels into bytes (high nibble first), and returns a C array definition string.
- `main()` -- Iterates over a hardcoded list of 9 icon filenames (sun, moon, partly, partly-night, cloud, rainy, snowflake, lighting, fog), converts each, and writes the combined output to `src/weather_icon_bitmaps.h`.

**Input:** PNG files from `~/Desktop/weather_icons/`
**Output:** `src/weather_icon_bitmaps.h` (~1.1MB, 9 icons at 20,000 bytes each)

**Requirements:** `pip3 install Pillow`

**Usage:**
```bash
python3 tools/convert_icons.py
```

### `convert_moon_icons.py`

Converts moon phase PNGs (100x100) into `src/moon_phase_bitmaps.h`.

**Key function:**

- `png_to_epd47_bitmap(png_path, output_size)` -- Same conversion logic as `convert_icons.py` but defaults to 100x100 output size. Array names are prefixed with `moon_` instead of `icon_`.
- `main()` -- Converts 8 moon phase icons (1-new, 2-crescent, 3-quarter, 4-gibbous, 5-full, 6-gibbous, 7-quarter, 8-crescent) and writes to `src/moon_phase_bitmaps.h`.

**Input:** PNG files from `~/Desktop/weather_icons/moon/`
**Output:** `src/moon_phase_bitmaps.h` (~248KB, 8 icons at 5,000 bytes each)

**Requirements:** `pip3 install Pillow`

**Usage:**
```bash
python3 tools/convert_moon_icons.py
```

### `icon_viewer.cpp`

An ESP32 test program that displays all weather icons on the e-paper in a grid layout for visual review. Not part of the normal build -- must be temporarily swapped in as `src/main.cpp`.

**What it displays:**
- Row 1: SUNNY, PARTLY_CLOUDY, CLOUDY
- Row 2: RAINY, SNOWY, THUNDERSTORM
- Row 3: FOG (centered)

Each icon is drawn at 180x180 pixels with a text label below.

**Usage:**
```bash
mv src/main.cpp src/main.cpp.bak
cp tools/icon_viewer.cpp src/main.cpp
pio run -t upload --upload-port /dev/cu.usbmodem2101
# Review the display, then restore:
mv src/main.cpp.bak src/main.cpp
```

## Classes

This directory contains no classes. All scripts are procedural Python with standalone functions, and the icon viewer is a single-file Arduino sketch using `setup()` / `loop()`.
