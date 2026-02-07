#!/bin/bash
#
# benchmark.sh — Performance benchmark for stripefs
#
# Creates test files, measures cold/warm read throughput through the FUSE
# mount, compares against a single-directory baseline, and reports per-OST
# distribution from the stats virtual file.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
STRIPEFS_BIN="$PROJECT_DIR/stripefs"

NUM_FILES=100
FILE_SIZE_MB=4
FILE_SIZE_BYTES=$((FILE_SIZE_MB * 1024 * 1024))
TOTAL_MB=$((NUM_FILES * FILE_SIZE_MB))

MOUNT="$(mktemp -d /tmp/stripefs_bench_mount.XXXXXX)"
BENCH_DIR="$(mktemp -d /tmp/stripefs_bench_data.XXXXXX)"
OST_BASE="$(mktemp -d /tmp/stripefs_bench_osts.XXXXXX)"
BASELINE_DIR="$(mktemp -d /tmp/stripefs_bench_baseline.XXXXXX)"
SRC_DIR="$BENCH_DIR/source_files"

OST0="$OST_BASE/ost0"
OST1="$OST_BASE/ost1"
OST2="$OST_BASE/ost2"
OST3="$OST_BASE/ost3"

STRIPEFS_PID=""

cleanup() {
    echo ""
    echo "--- Cleaning up ---"

    # Unmount if still mounted
    if mountpoint -q "$MOUNT" 2>/dev/null; then
        fusermount3 -u "$MOUNT" 2>/dev/null || fusermount -u "$MOUNT" 2>/dev/null || true
    fi

    # Wait for the background process to exit
    if [ -n "$STRIPEFS_PID" ] && kill -0 "$STRIPEFS_PID" 2>/dev/null; then
        wait "$STRIPEFS_PID" 2>/dev/null || true
    fi

    rm -rf "$BENCH_DIR" "$OST_BASE" "$BASELINE_DIR" "$MOUNT"
    echo "Cleanup complete."
}

trap cleanup EXIT

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

time_read_all() {
    local dir="$1"
    local start end elapsed throughput

    sync
    start=$(date +%s%N)
    for f in "$dir"/testfile_*; do
        cat "$f" > /dev/null
    done
    end=$(date +%s%N)

    elapsed=$(( (end - start) ))
    # Avoid division by zero; express throughput in MB/s
    if [ "$elapsed" -eq 0 ]; then
        elapsed=1
    fi
    # elapsed is in nanoseconds; throughput = total_bytes / elapsed_ns * 1e9 / 1e6
    throughput=$(awk "BEGIN { printf \"%.2f\", ($TOTAL_MB * 1000000000.0) / $elapsed }")
    elapsed_sec=$(awk "BEGIN { printf \"%.3f\", $elapsed / 1000000000.0 }")

    echo "$elapsed_sec $throughput"
}

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------

if [ ! -x "$STRIPEFS_BIN" ]; then
    echo "Error: stripefs binary not found at $STRIPEFS_BIN"
    echo "Build it first with:  make release"
    exit 1
fi

echo "==========================================="
echo " stripefs benchmark"
echo "==========================================="
echo ""
echo "Configuration:"
echo "  Files:        $NUM_FILES x ${FILE_SIZE_MB}MB = ${TOTAL_MB}MB total"
echo "  Stripe size:  1MB"
echo "  OSTs:         4"
echo "  Cache:        256MB, TTL 60s"
echo ""

# ---------------------------------------------------------------------------
# Step 1: Create OST directories
# ---------------------------------------------------------------------------

echo "--- Creating OST directories ---"
mkdir -p "$OST0" "$OST1" "$OST2" "$OST3" "$MOUNT"
echo "  $OST0"
echo "  $OST1"
echo "  $OST2"
echo "  $OST3"
echo ""

# ---------------------------------------------------------------------------
# Step 2: Generate test files
# ---------------------------------------------------------------------------

echo "--- Generating $NUM_FILES test files (${FILE_SIZE_MB}MB each) ---"
mkdir -p "$SRC_DIR"
for i in $(seq 1 "$NUM_FILES"); do
    dd if=/dev/urandom of="$SRC_DIR/testfile_$(printf '%03d' "$i")" \
       bs=1M count="$FILE_SIZE_MB" 2>/dev/null
done
echo "  Done."
echo ""

# ---------------------------------------------------------------------------
# Step 3: Mount stripefs
# ---------------------------------------------------------------------------

echo "--- Mounting stripefs ---"
"$STRIPEFS_BIN" \
    --mount "$MOUNT" \
    --ost "$OST0" \
    --ost "$OST1" \
    --ost "$OST2" \
    --ost "$OST3" \
    --stripe-size 1048576 \
    --cache-size 256 \
    --ttl 60 \
    --foreground &
STRIPEFS_PID=$!

# Wait for the mount to become available
echo -n "  Waiting for mount"
for _ in $(seq 1 30); do
    if mountpoint -q "$MOUNT" 2>/dev/null; then
        break
    fi
    echo -n "."
    sleep 0.3
done
echo ""

if ! mountpoint -q "$MOUNT" 2>/dev/null; then
    echo "Error: stripefs failed to mount within timeout"
    exit 1
fi
echo "  Mounted at $MOUNT (pid $STRIPEFS_PID)"
echo ""

# ---------------------------------------------------------------------------
# Step 4: Copy files into the mountpoint
# ---------------------------------------------------------------------------

echo "--- Copying files into stripefs mount ---"
cp "$SRC_DIR"/testfile_* "$MOUNT/"
sync
echo "  Done."
echo ""

# ---------------------------------------------------------------------------
# Step 5: Cold-cache read
# ---------------------------------------------------------------------------

echo "--- Cold-cache read (all $NUM_FILES files) ---"
# Drop caches if possible (requires root)
if [ "$(id -u)" -eq 0 ]; then
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
fi

read -r cold_time cold_throughput <<< "$(time_read_all "$MOUNT")"
echo "  Time: ${cold_time}s    Throughput: ${cold_throughput} MB/s"
echo ""

# ---------------------------------------------------------------------------
# Step 6: Warm-cache read
# ---------------------------------------------------------------------------

echo "--- Warm-cache read (all $NUM_FILES files) ---"
read -r warm_time warm_throughput <<< "$(time_read_all "$MOUNT")"
echo "  Time: ${warm_time}s    Throughput: ${warm_throughput} MB/s"
echo ""

# ---------------------------------------------------------------------------
# Step 7: Single-directory baseline
# ---------------------------------------------------------------------------

echo "--- Single-directory baseline ---"
cp "$SRC_DIR"/testfile_* "$BASELINE_DIR/"
sync

read -r base_time base_throughput <<< "$(time_read_all "$BASELINE_DIR")"
echo "  Time: ${base_time}s    Throughput: ${base_throughput} MB/s"
echo ""

# ---------------------------------------------------------------------------
# Step 8: Per-OST distribution
# ---------------------------------------------------------------------------

echo "--- Per-OST distribution ---"
if [ -f "$MOUNT/.stripefs_stats" ]; then
    cat "$MOUNT/.stripefs_stats"
else
    echo "  (stats file not available)"
fi
echo ""

# ---------------------------------------------------------------------------
# Step 9: Results table
# ---------------------------------------------------------------------------

echo "==========================================="
echo " Results"
echo "==========================================="
printf "%-25s %10s %12s\n" "Test" "Time (s)" "MB/s"
printf "%-25s %10s %12s\n" "-------------------------" "----------" "------------"
printf "%-25s %10s %12s\n" "Cold-cache read"  "$cold_time" "$cold_throughput"
printf "%-25s %10s %12s\n" "Warm-cache read"  "$warm_time" "$warm_throughput"
printf "%-25s %10s %12s\n" "Single-dir baseline" "$base_time" "$base_throughput"
echo "==========================================="
echo ""
echo "Total data: ${TOTAL_MB}MB across $NUM_FILES files"

# ---------------------------------------------------------------------------
# Step 10: Unmount
# ---------------------------------------------------------------------------

echo ""
echo "--- Unmounting stripefs ---"
fusermount3 -u "$MOUNT" 2>/dev/null || fusermount -u "$MOUNT" 2>/dev/null
wait "$STRIPEFS_PID" 2>/dev/null || true
STRIPEFS_PID=""
echo "  Done."
