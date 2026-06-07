#!/bin/bash
# Add a device to the fast-channel allowlist.
#
# Devices on this list receive new firmware as soon as it's published to the
# fast channel; everyone else waits for ota-promote.sh to move it to slow.
# Use this for your own test device(s).
#
# Usage: ota-add-fast-device.sh {deviceId}
#
#   deviceId  The device's chip ID — ESP.getEfuseMac() as lowercase 12-char
#             hex, shown on the setup screen and on the serial console.
#
# Reads the firmware:fast_devices KV key (a JSON array of device-id strings,
# defaulting to [] if absent), appends the id (deduped), and writes it back.
#
# A real run needs `wrangler login` first. See docs/ota.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKER_DIR="${SCRIPT_DIR}/../../worker"

if [ $# -ne 1 ] || [ -z "${1:-}" ]; then
    echo "Usage: ota-add-fast-device.sh {deviceId}" >&2
    exit 1
fi

DEVICE_ID="$1"

if ! printf '%s' "$DEVICE_ID" | grep -qE '^[0-9a-f]{12}$'; then
    echo "ERROR: deviceId must be a lowercase 12-char hex string (got '$DEVICE_ID')" >&2
    echo "       It's the chip ID shown on the setup screen / serial console." >&2
    exit 1
fi

cd "$WORKER_DIR"

echo "Reading firmware:fast_devices..."
# `kv key get` exits non-zero if the key is missing; default to an empty array.
CURRENT="$(npx wrangler kv key get --binding=WEATHER_KV "firmware:fast_devices" 2>/dev/null || true)"
if [ -z "$(printf '%s' "$CURRENT" | tr -d '[:space:]')" ]; then
    CURRENT="[]"
fi

# Parse, dedupe-append, and re-serialize with node (already required for npx).
UPDATED="$(printf '%s' "$CURRENT" | node -e '
    const fs = require("fs");
    const raw = fs.readFileSync(0, "utf8").trim() || "[]";
    const id = process.argv[1];
    let arr;
    try {
        arr = JSON.parse(raw);
    } catch (e) {
        console.error("ERROR: firmware:fast_devices is not valid JSON: " + raw);
        process.exit(1);
    }
    if (!Array.isArray(arr)) {
        console.error("ERROR: firmware:fast_devices is not a JSON array: " + raw);
        process.exit(1);
    }
    if (!arr.includes(id)) arr.push(id);
    process.stdout.write(JSON.stringify(arr));
' "$DEVICE_ID")"

echo "Writing firmware:fast_devices = ${UPDATED}"
npx wrangler kv key put --binding=WEATHER_KV "firmware:fast_devices" "$UPDATED"

echo "Device ${DEVICE_ID} is on the fast channel."
