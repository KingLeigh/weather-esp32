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
# `kv key get` exits non-zero if the key is missing; rescue with `|| true` so
# set -e doesn't abort before the friendly empty-check below.
FAST_VERSION="$(npx wrangler kv key get --binding=WEATHER_KV "firmware:channel:fast" 2>/dev/null || true)"
FAST_VERSION="$(printf '%s' "$FAST_VERSION" | tr -d '[:space:]')"

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
