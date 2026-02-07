#!/bin/bash
#
# unmount.sh — Simple unmount helper for stripefs
#
# Usage:
#   ./scripts/unmount.sh [MOUNTPOINT]
#
# Defaults:
#   MOUNTPOINT = /mnt/striped
#

set -euo pipefail

MOUNT="${1:-/mnt/striped}"

if ! mountpoint -q "$MOUNT" 2>/dev/null; then
    echo "Warning: $MOUNT does not appear to be a mountpoint"
fi

echo "Unmounting $MOUNT ..."

if command -v fusermount3 &>/dev/null; then
    fusermount3 -u "$MOUNT"
elif command -v fusermount &>/dev/null; then
    fusermount -u "$MOUNT"
else
    echo "Error: neither fusermount3 nor fusermount found in PATH"
    exit 1
fi

echo "Unmounted successfully."
