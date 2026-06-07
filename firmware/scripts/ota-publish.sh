#!/bin/bash
# Publish a firmware build to an OTA release channel.
#
# Uploads firmware/.pio/build/firmware/firmware.bin to KV under
# firmware:bin:{version} and points the chosen channel KV key at that version.
# Devices poll their channel (fast or slow, resolved by chip ID) and self-update.
# (Binary stored in KV in the interim; migrate to R2 later — see ROADMAP.md.)
#
# Usage:
#   ota-publish.sh [--channel fast|slow] [--build] [--version N] [--dry-run]
#
#   --channel  Release channel to point at this version (default: fast).
#   --build    Run `pio run -e firmware` before uploading.
#   --version  Override the version number (default: grep FIRMWARE_VERSION
#              from firmware/src/config.h).
#   --dry-run  Print the resolved version and the exact wrangler commands
#              without running them.
#
# A real publish needs `wrangler login` first. See docs/ota.md for the full
# release workflow (publish to fast → verify → ota-promote.sh to slow).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FIRMWARE_DIR="${SCRIPT_DIR}/.."
WORKER_DIR="${FIRMWARE_DIR}/../worker"
CONFIG_H="${FIRMWARE_DIR}/src/config.h"
FIRMWARE_BIN="${FIRMWARE_DIR}/.pio/build/firmware/firmware.bin"

CHANNEL="fast"
DO_BUILD=0
DRY_RUN=0
VERSION=""

while [ $# -gt 0 ]; do
    case "$1" in
        --channel)
            CHANNEL="${2:-}"
            shift 2
            ;;
        --build)
            DO_BUILD=1
            shift
            ;;
        --version)
            VERSION="${2:-}"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        *)
            echo "ERROR: unknown argument: $1" >&2
            echo "Usage: ota-publish.sh [--channel fast|slow] [--build] [--version N] [--dry-run]" >&2
            exit 1
            ;;
    esac
done

if [ "$CHANNEL" != "fast" ] && [ "$CHANNEL" != "slow" ]; then
    echo "ERROR: --channel must be 'fast' or 'slow' (got '$CHANNEL')" >&2
    exit 1
fi

# Resolve the version: --version wins, otherwise grep FIRMWARE_VERSION from config.h.
if [ -z "$VERSION" ]; then
    if [ ! -f "$CONFIG_H" ]; then
        echo "ERROR: config.h not found at $CONFIG_H and no --version given" >&2
        exit 1
    fi
    # Matches: inline constexpr int FIRMWARE_VERSION = N;
    VERSION="$(grep -E 'FIRMWARE_VERSION[[:space:]]*=' "$CONFIG_H" \
        | grep -oE '[0-9]+' | head -n1 || true)"
    if [ -z "$VERSION" ]; then
        echo "ERROR: could not find FIRMWARE_VERSION in $CONFIG_H." >&2
        echo "       Add 'inline constexpr int FIRMWARE_VERSION = N;' or pass --version N." >&2
        exit 1
    fi
fi

if ! printf '%s' "$VERSION" | grep -qE '^[0-9]+$'; then
    echo "ERROR: version must be a positive integer (got '$VERSION')" >&2
    exit 1
fi

BIN_CMD="npx wrangler kv key put --binding=WEATHER_KV \"firmware:bin:${VERSION}\" --path=../firmware/.pio/build/firmware/firmware.bin"
KV_CMD="npx wrangler kv key put --binding=WEATHER_KV \"firmware:channel:${CHANNEL}\" \"${VERSION}\""

if [ "$DRY_RUN" -eq 1 ]; then
    echo "[dry-run] resolved version: ${VERSION}"
    echo "[dry-run] channel: ${CHANNEL}"
    if [ "$DO_BUILD" -eq 1 ]; then
        echo "[dry-run] would build: export PATH=\"\$HOME/Library/Python/3.9/bin:\$PATH\" && pio run -e firmware"
    fi
    echo "[dry-run] (cd worker) ${BIN_CMD}"
    echo "[dry-run] (cd worker) ${KV_CMD}"
    exit 0
fi

if [ "$DO_BUILD" -eq 1 ]; then
    echo "Building firmware (pio run -e firmware)..."
    export PATH="$HOME/Library/Python/3.9/bin:$PATH"
    ( cd "$FIRMWARE_DIR" && pio run -e firmware )
fi

if [ ! -f "$FIRMWARE_BIN" ]; then
    echo "ERROR: firmware binary not found at $FIRMWARE_BIN" >&2
    echo "       Build it first with --build (or 'pio run -e firmware')." >&2
    exit 1
fi

cd "$WORKER_DIR"

echo "Uploading firmware v${VERSION} to KV (firmware:bin:${VERSION})..."
eval "$BIN_CMD"

echo "Pointing channel '${CHANNEL}' at version ${VERSION}..."
eval "$KV_CMD"

echo "Published version ${VERSION} to channel '${CHANNEL}'."
