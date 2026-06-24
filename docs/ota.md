# OTA firmware updates

The ESP32 weather display can pull and flash new firmware over WiFi, so a
gifted device never needs to be plugged in again. The Cloudflare Worker serves
the firmware binary and advertises the latest available version on every weather
response (`X-Firmware-Latest`); the device reads that header on each wake and
self-updates — right after rendering the weather — when a newer version is
available.

Discovery is therefore free (no extra request) and effectively every wake. There
is a single release stream: publish a version and every device picks it up on its
next wake. (A staged fast/slow channel scheme existed earlier but was removed —
see `ROADMAP.md` history.)

## How it's stored

Everything lives in Cloudflare KV (binding `WEATHER_KV`):

| Key | Value |
| --- | --- |
| `firmware:latest` | latest version number as a string, e.g. `"7"` |
| `firmware:bin:{version}` | the firmware binary (~1.2 MB) for that integer version |

> **Interim storage note:** the binary is kept in KV for now (it fits KV's 25 MB
> per-value limit). The intended long-term home is a dedicated R2 bucket,
> deferred until R2 is enabled on the account — see `ROADMAP.md`.

The single source of truth for the version number is `firmware/src/config.h`:

```c
inline constexpr int FIRMWARE_VERSION = 7;
```

Bump this integer (monotonically) for every release. `ota-publish.sh` reads it.

## One-time bootstrap

Do these once, before gifting any device.

1. **Authenticate wrangler** (needed by the publish script):

   ```sh
   cd worker
   npx wrangler login
   ```

2. **Flash the first OTA-capable build over USB.** The firmware currently on a
   device has no OTA codepath, so the very first build that *can* self-update
   must be installed once over the wire:

   ```sh
   export PATH="$HOME/Library/Python/3.9/bin:$PATH"
   cd firmware
   pio run -e firmware -t upload
   ```

   After this, the device can update itself over WiFi forever — no physical
   access required.

## Release flow

For each new release:

1. **Bump the version.** Edit `firmware/src/config.h` and increment
   `FIRMWARE_VERSION`.

2. **Publish.** This builds (with `--build`), uploads the binary to KV as
   `firmware:bin:{version}`, and points `firmware:latest` at the new version:

   ```sh
   bash firmware/scripts/ota-publish.sh --build
   ```

   Omit `--build` if you've already built the binary. Preview exactly what it
   will do without touching anything:

   ```sh
   bash firmware/scripts/ota-publish.sh --dry-run
   ```

3. **Verify.** Every device picks up the new build on its next wake (fetch
   weather → see `X-Firmware-Latest` is newer → download → reboot). Confirm your
   device boots, renders, and behaves; the debug screen's version line shows what
   it's running.

## Script reference

`firmware/scripts/ota-publish.sh` `cd`s into `worker/` internally so
`--binding=WEATHER_KV` resolves from `worker/wrangler.toml`. It expects
`wrangler login` to have been run.

```
ota-publish.sh [--build] [--version N] [--dry-run]
```

- `--build` — run `pio run -e firmware` before uploading.
- `--version N` — override the version (default: grep `FIRMWARE_VERSION` from
  `firmware/src/config.h`).
- `--dry-run` — print the resolved version and the exact wrangler commands
  without running them.

Uploads `firmware/.pio/build/firmware/firmware.bin` to KV as
`firmware:bin:{version}` and sets `firmware:latest` to the version.
