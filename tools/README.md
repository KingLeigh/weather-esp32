# Development Tools

## Icon Converter (`convert_icons.py`)

Converts PNG weather icons to EPD47 4-bit grayscale bitmap format for embedded display.

### Requirements:

```bash
pip3 install Pillow
```

### Usage:

1. Place your icon PNG files in `/Users/leighherbert/Desktop/weather_icons/`
   - Icons should be square (any size, will be resized to 200x200)
   - Supported formats: PNG with transparency preferred
   - Expected filenames:
     - `sun.png` → SUNNY
     - `partly.png` → PARTLY_CLOUDY
     - `cloud.png` → CLOUDY
     - `rainy.png` → RAINY
     - `snowflake.png` → SNOWY
     - `lighting.png` → THUNDERSTORM
     - `fog.png` → FOG

2. Run the converter:
   ```bash
   python3 tools/convert_icons.py
   ```

3. This generates `src/weather_icon_bitmaps.h` with bitmap arrays

4. Rebuild and upload:
   ```bash
   export PATH="$HOME/Library/Python/3.9/bin:$PATH"
   pio run -t upload --upload-port /dev/cu.usbmodem1101
   ```

### Adding new icons:

1. Add the PNG file to the icon directory
2. Edit `tools/convert_icons.py` and add the filename to the `icons` list
3. Edit `src/weather_icons.h` and add the new enum value + case statement
4. Run the converter and rebuild

### Technical details:

- Output format: 4-bit grayscale (0x00=black, 0xFF=white)
- Packing: 2 pixels per byte (nibble-packed)
- Size: 200x200 pixels = 20,000 bytes per icon
- Transparent areas are rendered as white background

---

## Icon Viewer (`icon_viewer.cpp`)

A test program that displays all weather icons on the e-paper display for visual review.

### Usage:

1. Temporarily swap the main program:
   ```bash
   mv src/main.cpp src/main.cpp.bak
   cp tools/icon_viewer.cpp src/main.cpp
   ```

2. Build and upload:
   ```bash
   export PATH="$HOME/Library/Python/3.9/bin:$PATH"
   pio run -t upload --upload-port /dev/cu.usbmodem1101
   ```

3. Take a photo of the display showing all icons

4. Restore the main program:
   ```bash
   mv src/main.cpp.bak src/main.cpp
   pio run -t upload --upload-port /dev/cu.usbmodem1101
   ```

### What it displays:

Shows all 7 weather icons in a grid layout:
- Row 1: SUNNY, PARTLY_CLOUDY, CLOUDY
- Row 2: RAINY, SNOWY, THUNDERSTORM
- Row 3: FOG (centered)

Each icon is labeled with its name.
