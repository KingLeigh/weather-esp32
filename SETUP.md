# Hardware Setup Guide

How to go from a brand-new, blank ESP32 board to a working weather display.

The device is a **LilyGo T5 4.7" S3 E-Paper** board (ESP32-S3, 960×540). It runs
self-serve firmware: after flashing, you configure WiFi and a location from your
phone via a captive-portal — no hardcoded credentials, no re-flashing to change
settings.

---

## What you'll need

- LilyGo T5 4.7" S3 E-Paper board + battery
- A **data-capable** USB-C cable
- A Mac with PlatformIO (one-time setup in Part 1)
- A phone (for the WiFi setup portal)
- The display location **already registered on the server** (see prerequisite)

## Prerequisite — register the location on the server

The device's setup screen only lets you pick from locations that already exist on
the weather server. Before setting up a device, make sure the location is there:

1. Open the admin page: <https://weather-esp32.leigh-herbert.workers.dev/admin>
2. Add the location (zip + lat/lon + label) if it isn't already listed.

If nothing is registered, device setup will report *"No locations registered on
server."*

---

## Part 1 — Build environment (one-time, on your Mac)

The firmware is built and flashed with **PlatformIO**.

PlatformIO is installed at `~/Library/Python/3.9/bin/pio`. Add it to your PATH for
the current shell session:

```bash
export PATH="$HOME/Library/Python/3.9/bin:$PATH"
```

(If `pio` isn't installed yet: `pip3 install --user platformio`.)

## Part 2 — Flash the firmware

1. Plug the board into USB. The **blue LED** should light up.
   - No LED / no power? **Flip the USB-C connector** — orientation matters on this board.
2. Build and upload the `firmware` environment:

   ```bash
   export PATH="$HOME/Library/Python/3.9/bin:$PATH"
   cd ~/code/weather-claude/firmware
   pio run -e firmware -t upload
   ```

   `pio` auto-detects the serial port. To pin it explicitly:
   `pio run -e firmware -t upload --upload-port /dev/cu.usbmodem2101`
   (find the port with `pio device list`).

### If a brand-new board won't upload

A blank ESP32-S3 often won't expose its USB serial port until it has firmware, so
the **first** upload can fail to connect. Put the chip into download mode by hand:

1. Hold **BOOT (IO00)**, tap **RST**, then release BOOT → drops into the ROM bootloader.
2. Re-run the `pio run -e firmware -t upload` command.
3. Press **RST** afterward to boot the freshly-flashed firmware.

To watch boot logs over serial: `pio device monitor -e firmware` (115200 baud).

## Part 3 — First-time device setup (WiFi + location)

After flashing, the device shows a splash screen and goes to sleep. Configure it
from your phone:

1. **Enter setup mode** — press and hold the **user button (IO21)** for ~1.5 s.
   The splash redraws with a **QR code** and the device starts its own WiFi hotspot.
2. **Join the device's hotspot** — scan the on-screen QR with your phone (it's a
   "join WiFi" code), or manually join the open network named
   **`WhatsTheWeather-XXXX`** (XXXX = last 4 hex digits of the board's MAC). A
   setup page should pop up automatically (captive portal); if it doesn't, open a
   browser to any address.
3. **Step 1 – WiFi** — pick your network from the scanned list, enter the
   password, tap **Connect**.
4. **Step 2 – Location** — pick the location from the dropdown (loaded from the
   server), tap **Save**.
5. The device verifies WiFi + the location, saves the config to flash (NVS), and
   restarts. It then fetches and displays the weather. ✅

Setup mode times out after **3 minutes** of inactivity and returns to the splash.

---

## Normal operation

- Wakes about every **10 minutes**, fetches `/weather/{zip}.png`, updates the
  e-paper, and deep-sleeps. Battery lasts ~months.
- The bottom-right corner shows the **battery** level and, if the data is older
  than 30 min, a **"stale" age** ("35m", "1h 23m").

## Re-configuring / handing the device off

- **Change WiFi or location:** long-press **IO21** again to re-enter setup mode.
- **Factory reset** (wipe WiFi + location, e.g. before gifting): enter setup mode,
  open the portal, tap **Factory reset** and confirm. (Settings survive deep
  sleep, power cycles, and even re-flashes — only a factory reset or NVS wipe
  clears them.)

---

## Reference

**Server:** <https://weather-esp32.leigh-herbert.workers.dev> · admin at `/admin`

**Buttons:**
| Button | Purpose |
|--------|---------|
| BOOT (IO00) | Hold + tap RST → flash download mode (only if upload won't connect) |
| RST | Reset / reboot |
| IO21 (user button) | Long-press (~1.5 s) → enter WiFi/location setup mode |

**PlatformIO environments** (`firmware/platformio.ini`):
| Env | Purpose |
|-----|---------|
| `firmware` | Normal build — the one you flash for real use |
| `firmware-debug` | Soft-restarts every 5 s instead of deep sleeping, so USB serial stays alive for log capture (button wake disabled) |
| `firmware-splash-test` | Renders the bundled splash only — no WiFi, no fetch |

**Common commands:**
```bash
pio run -e firmware -t upload        # build + flash
pio device monitor -e firmware       # serial logs @ 115200
pio device list                      # find the USB serial port
```

**Note — timezone:** the firmware's NTP timezone is hardcoded to `EST5EDT` (in
`firmware/src/main.cpp`). It must match the server's timezone, or the "stale" age
indicator will be wrong.

**Note — polling interval:** controlled by `SLEEP_MINUTES` in
`firmware/src/main.cpp` (currently 10 minutes).
