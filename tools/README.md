# Development Tools

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
