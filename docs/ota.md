# OTA firmware updates

The ESP32 weather display can pull and flash new firmware over WiFi, so a
gifted device never needs to be plugged in again. The Cloudflare Worker serves
the firmware binary; the device polls a release **channel** and self-updates
when a newer version is available.

There are two channels:

- **fast** — your own test device(s). Gets every new build immediately.
- **slow** — everyone else. Gets a build only after you've vetted it on fast
  and promoted it.

Which channel a device follows is resolved by its **chip ID** (`ESP.getEfuseMac()`
as a lowercase 12-char hex string). A device whose chip ID is in the
`firmware:fast_devices` allowlist follows the fast channel; all others follow
slow.

## How it's stored

Everything lives in Cloudflare KV (binding `WEATHER_KV`):

| Key | Value |
| --- | --- |
| `firmware:channel:fast` | version number as a string, e.g. `"3"` |
| `firmware:channel:slow` | version number as a string |
| `firmware:fast_devices` | JSON array of device-id strings, e.g. `["a1b2c3d4e5f6"]` |
| `firmware:bin:{version}` | the firmware binary (~1.1 MB) for that integer version |

> **Interim storage note:** the binary is kept in KV for now (it fits KV's 25 MB
> per-value limit). The intended long-term home is a dedicated R2 bucket,
> deferred until R2 is enabled on the account — see `ROADMAP.md`.

The single source of truth for the version number is
`firmware/src/config.h`:

```c
inline constexpr int FIRMWARE_VERSION = 1;
```

Bump this integer (monotonically) for every release. `ota-publish.sh` reads it.

## One-time bootstrap

Do these once, before gifting any device.

1. **Authenticate wrangler** (needed by all the scripts below):

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

2. **Publish to the fast channel.** This builds (with `--build`), uploads the
   binary to KV as `firmware:bin:{version}`, and points
   `firmware:channel:fast` at the new version:

   ```sh
   bash firmware/scripts/ota-publish.sh --channel fast --build
   ```

   `--channel` defaults to `fast`, so `--build` alone is enough. Omit `--build`
   if you've already built the binary.

   Preview exactly what it will do without touching anything:

   ```sh
   bash firmware/scripts/ota-publish.sh --channel fast --dry-run
   ```

3. **Verify on your own device.** Your test device (see
   [Adding a fast device](#adding-a-fast-device)) follows the fast channel and
   will pick up the new build on its next update check. Confirm it boots,
   renders, and behaves.

4. **Promote fast → slow.** Once vetted, ship the *exact same* version to every
   other device. This reads `firmware:channel:fast` and copies that version
   number to `firmware:channel:slow` — no rebuild, no re-upload:

   ```sh
   bash firmware/scripts/ota-promote.sh
   ```

The two channels staging the same binary means slow-channel devices only ever
receive a build you've actually run.

## Adding a fast device

To put a device on the fast channel, you need its chip ID — the lowercase
12-char hex string shown on the setup screen and printed to the serial console
on boot.

```sh
bash firmware/scripts/ota-add-fast-device.sh a1b2c3d4e5f6
```

This reads `firmware:fast_devices` (defaulting to `[]` if it doesn't exist
yet), appends the id (deduped), and writes the array back.

## Script reference

All scripts live in `firmware/scripts/` and `cd` into `worker/` internally so
`--binding=WEATHER_KV` resolves from `worker/wrangler.toml`. They expect
`wrangler login` to have been run.

### `ota-publish.sh`

```
ota-publish.sh [--channel fast|slow] [--build] [--version N] [--dry-run]
```

- `--channel fast|slow` — channel to point at this version (default `fast`).
- `--build` — run `pio run -e firmware` before uploading.
- `--version N` — override the version (default: grep `FIRMWARE_VERSION` from
  `firmware/src/config.h`).
- `--dry-run` — print the resolved version and the exact wrangler commands
  without running them.

Uploads `firmware/.pio/build/firmware/firmware.bin` to KV as
`firmware:bin:{version}` and sets `firmware:channel:{channel}` to the version.

### `ota-promote.sh`

```
ota-promote.sh
```

Reads `firmware:channel:fast` and writes that version to
`firmware:channel:slow`.

### `ota-add-fast-device.sh`

```
ota-add-fast-device.sh {deviceId}
```

Appends a chip ID to the `firmware:fast_devices` allowlist (deduped).
