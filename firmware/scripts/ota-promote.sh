#!/bin/bash
# Promote the current fast-channel firmware version to the slow channel.
#
# Reads firmware:channel:fast and writes the exact same version number to
# firmware:channel:slow. Run this after you've vetted the fast build on your
# own device — it ships the *same* binary to every other device, no rebuild.
#
# Usage: ota-promote.sh
#
# A real promote needs `wrangler login` first. See docs/ota.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKER_DIR="${SCRIPT_DIR}/../../worker"

cd "$WORKER_DIR"

echo "Reading firmware:channel:fast..."
# `kv key get` EXITS NON-ZERO (404) for a missing key in wrangler v3 (and prints
# the error to stdout), so key off the exit code rather than the captured text.
if FAST_VERSION="$(npx wrangler kv key get --binding=WEATHER_KV "firmware:channel:fast" 2>/dev/null)"; then
    FAST_VERSION="$(printf '%s' "$FAST_VERSION" | tr -d '[:space:]')"
else
    FAST_VERSION=""
fi

if [ -z "$FAST_VERSION" ]; then
    echo "ERROR: firmware:channel:fast is empty or unset — publish to fast first." >&2
    exit 1
fi

if ! printf '%s' "$FAST_VERSION" | grep -qE '^[0-9]+$'; then
    echo "ERROR: firmware:channel:fast is not an integer version (got '$FAST_VERSION')" >&2
    exit 1
fi

echo "Promoting version ${FAST_VERSION} to firmware:channel:slow..."
npx wrangler kv key put --binding=WEATHER_KV "firmware:channel:slow" "$FAST_VERSION"

echo "Promoted version ${FAST_VERSION} fast → slow."
