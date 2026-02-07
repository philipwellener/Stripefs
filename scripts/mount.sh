#!/bin/bash
#
# mount.sh — Simple mount helper for stripefs
#
# Usage:
#   ./scripts/mount.sh [MOUNTPOINT] [OST0 OST1 OST2 OST3]
#
# Defaults:
#   MOUNTPOINT = /mnt/striped
#   OSTs       = /data/ost0 /data/ost1 /data/ost2 /data/ost3
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
STRIPEFS_BIN="$PROJECT_DIR/stripefs"

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------

MOUNT="${1:-/mnt/striped}"
shift 2>/dev/null || true

if [ $# -ge 1 ]; then
    OSTS=("$@")
else
    OSTS=(/data/ost0 /data/ost1 /data/ost2 /data/ost3)
fi

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------

if [ ! -x "$STRIPEFS_BIN" ]; then
    echo "Error: stripefs binary not found at $STRIPEFS_BIN"
    echo "Build it first with:  make release"
    exit 1
fi

# ---------------------------------------------------------------------------
# Create directories if needed
# ---------------------------------------------------------------------------

echo "Creating directories if needed..."
mkdir -p "$MOUNT"
for ost in "${OSTS[@]}"; do
    mkdir -p "$ost"
done

# ---------------------------------------------------------------------------
# Build --ost flags
# ---------------------------------------------------------------------------

OST_ARGS=()
for ost in "${OSTS[@]}"; do
    OST_ARGS+=(--ost "$ost")
done

# ---------------------------------------------------------------------------
# Mount
# ---------------------------------------------------------------------------

echo "Mounting stripefs:"
echo "  Mount point: $MOUNT"
for i in "${!OSTS[@]}"; do
    echo "  OST$i:        ${OSTS[$i]}"
done
echo ""

exec "$STRIPEFS_BIN" \
    --mount "$MOUNT" \
    "${OST_ARGS[@]}" \
    --foreground
