#!/bin/bash
#
# test_stress.sh -- Concurrent I/O stress test for stripefs
#
# Spawns 8 worker processes, each writing 20 random-sized files (1KB-5MB),
# reading them back, and verifying md5sum integrity. Validates that no data
# corruption occurs under concurrent load and reports per-OST utilization.
#
# Exit code: 0 if all consistency checks pass, non-zero otherwise.

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
NUM_WORKERS=8
FILES_PER_WORKER=20
MIN_FILE_KB=1
MAX_FILE_KB=5120          # 5 MB in KB
STRIPE_SIZE=1048576       # 1 MB
CACHE_SIZE_MB=128
TTL_SECONDS=60
MOUNT_WAIT_SECONDS=10
FUSE_PID=""
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="${PROJECT_ROOT}/stripefs"

# ---------------------------------------------------------------------------
# Colours / formatting (disabled when not a tty)
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    BOLD='\033[1m'
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    CYAN='\033[0;36m'
    RESET='\033[0m'
else
    BOLD=''; RED=''; GREEN=''; YELLOW=''; CYAN=''; RESET=''
fi

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log_info()  { echo -e "${CYAN}[INFO]${RESET}  $*"; }
log_ok()    { echo -e "${GREEN}[OK]${RESET}    $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${RESET}  $*"; }
log_fail()  { echo -e "${RED}[FAIL]${RESET}  $*"; }
log_hdr()   { echo -e "\n${BOLD}=== $* ===${RESET}"; }

# Pick the right md5 command for the platform.
if command -v md5sum &>/dev/null; then
    md5cmd() { md5sum "$1" | awk '{print $1}'; }
elif command -v md5 &>/dev/null; then
    md5cmd() { md5 -q "$1"; }
else
    echo "ERROR: neither md5sum nor md5 found" >&2
    exit 1
fi

# Pick the right unmount command.
do_unmount() {
    local mnt="$1"
    if command -v fusermount3 &>/dev/null; then
        fusermount3 -u "$mnt"
    elif command -v fusermount &>/dev/null; then
        fusermount -u "$mnt"
    elif command -v umount &>/dev/null; then
        umount "$mnt"
    else
        log_fail "No unmount command found (fusermount3, fusermount, umount)"
        return 1
    fi
}

# ---------------------------------------------------------------------------
# Cleanup handler -- always runs on exit
# ---------------------------------------------------------------------------
cleanup() {
    log_hdr "Cleanup"

    # Kill the FUSE process if still running.
    if [ -n "$FUSE_PID" ] && kill -0 "$FUSE_PID" 2>/dev/null; then
        log_info "Unmounting stripefs at ${MOUNT}..."
        do_unmount "$MOUNT" 2>/dev/null || true
        # Give it a moment to exit cleanly.
        sleep 1
        if kill -0 "$FUSE_PID" 2>/dev/null; then
            log_warn "FUSE process still alive -- sending SIGKILL"
            kill -9 "$FUSE_PID" 2>/dev/null || true
        fi
    fi

    # Remove temp directories.
    if [ -d "${TMPBASE:-}" ]; then
        log_info "Removing temp directory ${TMPBASE}"
        rm -rf "$TMPBASE"
    fi

    log_info "Cleanup complete."
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Create temp directories
# ---------------------------------------------------------------------------
log_hdr "Setup"

TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/stripefs_stress.XXXXXX")"
log_info "Temp base: ${TMPBASE}"

OST0="${TMPBASE}/ost0"
OST1="${TMPBASE}/ost1"
OST2="${TMPBASE}/ost2"
OST3="${TMPBASE}/ost3"
MOUNT="${TMPBASE}/mnt"
CHECKSUMS_DIR="${TMPBASE}/checksums"

mkdir -p "$OST0" "$OST1" "$OST2" "$OST3" "$MOUNT" "$CHECKSUMS_DIR"
log_ok "Created 4 OST directories and mount point"

# ---------------------------------------------------------------------------
# Build stripefs
# ---------------------------------------------------------------------------
log_hdr "Build"

if [ ! -f "$BINARY" ]; then
    log_info "Binary not found -- building with 'make -C ${PROJECT_ROOT} release'"
    make -C "$PROJECT_ROOT" release
fi

if [ ! -x "$BINARY" ]; then
    log_fail "stripefs binary not found or not executable at ${BINARY}"
    exit 1
fi
log_ok "stripefs binary ready: ${BINARY}"

# ---------------------------------------------------------------------------
# Mount stripefs
# ---------------------------------------------------------------------------
log_hdr "Mount"

log_info "Mounting stripefs..."
"$BINARY" \
    --mount "$MOUNT" \
    --ost "$OST0" \
    --ost "$OST1" \
    --ost "$OST2" \
    --ost "$OST3" \
    --stripe-size "$STRIPE_SIZE" \
    --cache-size "$CACHE_SIZE_MB" \
    --ttl "$TTL_SECONDS" \
    --foreground &
FUSE_PID=$!

# Wait for mount to become ready.
log_info "Waiting for mount (PID ${FUSE_PID})..."
WAITED=0
while [ $WAITED -lt $MOUNT_WAIT_SECONDS ]; do
    if mount | grep -q "$MOUNT" 2>/dev/null || \
       [ -f "${MOUNT}/.stripefs_stats" ] 2>/dev/null; then
        break
    fi
    sleep 1
    WAITED=$((WAITED + 1))
done

if ! kill -0 "$FUSE_PID" 2>/dev/null; then
    log_fail "stripefs process died during startup"
    exit 1
fi

# Final readiness check -- try to stat the stats file.
if [ ! -f "${MOUNT}/.stripefs_stats" ]; then
    log_warn "Stats file not detected after ${MOUNT_WAIT_SECONDS}s -- proceeding anyway"
fi

log_ok "stripefs mounted at ${MOUNT} (PID ${FUSE_PID})"

# ---------------------------------------------------------------------------
# Worker function
# ---------------------------------------------------------------------------
# Each worker writes FILES_PER_WORKER files, records checksums, reads them
# back, and verifies integrity. Returns non-zero on any mismatch.
#
# Arguments: $1 = worker_id (0-based)
# ---------------------------------------------------------------------------
worker() {
    local wid="$1"
    local checksum_file="${CHECKSUMS_DIR}/worker_${wid}.md5"
    local worker_dir="${MOUNT}/worker_${wid}"
    local total_bytes=0
    local files_written=0
    local files_read=0
    local errors=0

    # Create worker directory.
    mkdir -p "$worker_dir"

    # ---- Write phase ----
    for i in $(seq 0 $((FILES_PER_WORKER - 1))); do
        # Random size between MIN_FILE_KB and MAX_FILE_KB (in KB).
        local size_kb=$(( (RANDOM % (MAX_FILE_KB - MIN_FILE_KB + 1)) + MIN_FILE_KB ))
        local filename="file_w${wid}_${i}.dat"
        local filepath="${worker_dir}/${filename}"

        # Write random data.
        dd if=/dev/urandom of="$filepath" bs=1024 count="$size_kb" 2>/dev/null
        if [ $? -ne 0 ]; then
            echo "WORKER ${wid}: ERROR writing ${filename}" >&2
            errors=$((errors + 1))
            continue
        fi

        # Compute and record checksum.
        local cksum
        cksum="$(md5cmd "$filepath")"
        echo "${cksum}  ${filename}  ${size_kb}" >> "$checksum_file"

        total_bytes=$((total_bytes + size_kb * 1024))
        files_written=$((files_written + 1))
    done

    # ---- Read-back / verify phase ----
    while IFS='  ' read -r expected_md5 fname fsize_kb; do
        local filepath="${worker_dir}/${fname}"

        if [ ! -f "$filepath" ]; then
            echo "WORKER ${wid}: MISSING ${fname}" >&2
            errors=$((errors + 1))
            continue
        fi

        local actual_md5
        actual_md5="$(md5cmd "$filepath")"

        if [ "$actual_md5" != "$expected_md5" ]; then
            echo "WORKER ${wid}: CHECKSUM MISMATCH ${fname} (expected ${expected_md5}, got ${actual_md5})" >&2
            errors=$((errors + 1))
        fi

        files_read=$((files_read + 1))
    done < "$checksum_file"

    # Write a result summary line for the parent to aggregate.
    echo "${wid} ${files_written} ${files_read} ${total_bytes} ${errors}"
}

# ---------------------------------------------------------------------------
# Spawn workers
# ---------------------------------------------------------------------------
log_hdr "Stress Test (${NUM_WORKERS} workers x ${FILES_PER_WORKER} files)"

RESULTS_DIR="${TMPBASE}/results"
mkdir -p "$RESULTS_DIR"

for wid in $(seq 0 $((NUM_WORKERS - 1))); do
    (
        result="$(worker "$wid")"
        echo "$result" > "${RESULTS_DIR}/worker_${wid}.result"
    ) &
done

log_info "All ${NUM_WORKERS} workers launched -- waiting for completion..."
wait
log_ok "All workers finished"

# ---------------------------------------------------------------------------
# Aggregate worker results
# ---------------------------------------------------------------------------
log_hdr "Worker Results"

TOTAL_WRITTEN=0
TOTAL_READ=0
TOTAL_BYTES=0
TOTAL_ERRORS=0

for wid in $(seq 0 $((NUM_WORKERS - 1))); do
    result_file="${RESULTS_DIR}/worker_${wid}.result"
    if [ ! -f "$result_file" ]; then
        log_fail "Worker ${wid}: result file missing (worker may have crashed)"
        TOTAL_ERRORS=$((TOTAL_ERRORS + 1))
        continue
    fi

    read -r r_wid r_written r_read r_bytes r_errors < "$result_file"
    log_info "Worker ${r_wid}: wrote=${r_written} read=${r_read} bytes=${r_bytes} errors=${r_errors}"

    TOTAL_WRITTEN=$((TOTAL_WRITTEN + r_written))
    TOTAL_READ=$((TOTAL_READ + r_read))
    TOTAL_BYTES=$((TOTAL_BYTES + r_bytes))
    TOTAL_ERRORS=$((TOTAL_ERRORS + r_errors))
done

# ---------------------------------------------------------------------------
# Global verification pass
# ---------------------------------------------------------------------------
log_hdr "Global Verification Pass"

VERIFY_ERRORS=0
VERIFY_COUNT=0

for wid in $(seq 0 $((NUM_WORKERS - 1))); do
    checksum_file="${CHECKSUMS_DIR}/worker_${wid}.md5"
    worker_dir="${MOUNT}/worker_${wid}"

    if [ ! -f "$checksum_file" ]; then
        log_fail "Worker ${wid}: checksum file missing"
        VERIFY_ERRORS=$((VERIFY_ERRORS + 1))
        continue
    fi

    while IFS='  ' read -r expected_md5 fname fsize_kb; do
        filepath="${worker_dir}/${fname}"
        VERIFY_COUNT=$((VERIFY_COUNT + 1))

        # Check file exists.
        if [ ! -f "$filepath" ]; then
            log_fail "MISSING: ${filepath}"
            VERIFY_ERRORS=$((VERIFY_ERRORS + 1))
            continue
        fi

        # Check file is not zero-length when it should not be.
        actual_size=$(wc -c < "$filepath" | tr -d ' ')
        expected_size=$((fsize_kb * 1024))
        if [ "$actual_size" -ne "$expected_size" ]; then
            log_fail "SIZE MISMATCH: ${fname} (expected ${expected_size}, got ${actual_size})"
            VERIFY_ERRORS=$((VERIFY_ERRORS + 1))
            continue
        fi

        # Verify checksum.
        actual_md5="$(md5cmd "$filepath")"
        if [ "$actual_md5" != "$expected_md5" ]; then
            log_fail "CHECKSUM MISMATCH: ${fname} (expected ${expected_md5}, got ${actual_md5})"
            VERIFY_ERRORS=$((VERIFY_ERRORS + 1))
        fi
    done < "$checksum_file"
done

if [ "$VERIFY_ERRORS" -eq 0 ]; then
    log_ok "All ${VERIFY_COUNT} files verified successfully"
else
    log_fail "${VERIFY_ERRORS} verification failures out of ${VERIFY_COUNT} files"
fi

# ---------------------------------------------------------------------------
# Stats report
# ---------------------------------------------------------------------------
log_hdr "stripefs Stats"

STATS_FILE="${MOUNT}/.stripefs_stats"
if [ -f "$STATS_FILE" ]; then
    STATS_JSON="$(cat "$STATS_FILE" 2>/dev/null || true)"

    if [ -n "$STATS_JSON" ]; then
        log_info "Raw stats from /.stripefs_stats:"
        echo "$STATS_JSON"
        echo ""

        # Parse key counters (best-effort with grep/sed -- no jq dependency).
        total_read_ops="$(echo "$STATS_JSON"  | grep '"total_read_ops"'  | sed 's/[^0-9]//g' || echo "?")"
        total_write_ops="$(echo "$STATS_JSON" | grep '"total_write_ops"' | sed 's/[^0-9]//g' || echo "?")"
        bytes_read="$(echo "$STATS_JSON"      | grep '"bytes_read_from_osts"' | sed 's/[^0-9]//g' || echo "?")"
        bytes_written="$(echo "$STATS_JSON"   | grep '"bytes_written_to_osts"' | sed 's/[^0-9]//g' || echo "?")"
        cache_hits="$(echo "$STATS_JSON"      | grep '"hits"'     | head -1 | sed 's/[^0-9]//g' || echo "?")"
        cache_misses="$(echo "$STATS_JSON"    | grep '"misses"'   | head -1 | sed 's/[^0-9]//g' || echo "?")"

        log_info "IO operations   -- reads: ${total_read_ops}, writes: ${total_write_ops}"
        log_info "Bytes from OSTs -- read: ${bytes_read}, written: ${bytes_written}"
        log_info "Cache           -- hits: ${cache_hits}, misses: ${cache_misses}"

        # Per-OST utilization.
        echo ""
        log_info "Per-OST utilization:"
        for ost_idx in 0 1 2 3; do
            # Extract per-OST block: find the block with matching ost_index.
            ost_bytes_r="$(echo "$STATS_JSON" | grep -A4 "\"ost_index\": ${ost_idx}" | grep '"bytes_read"'    | sed 's/[^0-9]//g' || echo "0")"
            ost_bytes_w="$(echo "$STATS_JSON" | grep -A4 "\"ost_index\": ${ost_idx}" | grep '"bytes_written"' | sed 's/[^0-9]//g' || echo "0")"
            ost_read_ops="$(echo "$STATS_JSON" | grep -A4 "\"ost_index\": ${ost_idx}" | grep '"read_ops"'     | sed 's/[^0-9]//g' || echo "0")"
            ost_write_ops="$(echo "$STATS_JSON" | grep -A4 "\"ost_index\": ${ost_idx}" | grep '"write_ops"'   | sed 's/[^0-9]//g' || echo "0")"
            log_info "  OST${ost_idx}: read ${ost_bytes_r} bytes (${ost_read_ops} ops), wrote ${ost_bytes_w} bytes (${ost_write_ops} ops)"
        done

        # Verify stats show non-zero operations.
        if [ "${total_write_ops:-0}" = "0" ] || [ "${total_write_ops:-0}" = "?" ]; then
            log_warn "Stats show zero write operations -- unexpected"
        else
            log_ok "Stats confirm filesystem operations were recorded"
        fi
    else
        log_warn "Stats file exists but returned empty content"
    fi
else
    log_warn "Stats file not found at ${STATS_FILE}"
fi

# ---------------------------------------------------------------------------
# Final summary
# ---------------------------------------------------------------------------
log_hdr "Summary"

ALL_ERRORS=$((TOTAL_ERRORS + VERIFY_ERRORS))

echo ""
echo "  Workers:               ${NUM_WORKERS}"
echo "  Files per worker:      ${FILES_PER_WORKER}"
echo "  Total files written:   ${TOTAL_WRITTEN}"
echo "  Total files read:      ${TOTAL_READ}"
echo "  Total bytes xferred:   ${TOTAL_BYTES} ($(( TOTAL_BYTES / 1024 / 1024 )) MB)"
echo "  Worker-phase errors:   ${TOTAL_ERRORS}"
echo "  Verify-phase errors:   ${VERIFY_ERRORS}"
echo "  Consistency violations: ${ALL_ERRORS}"
echo ""

if [ "$ALL_ERRORS" -eq 0 ]; then
    log_ok "STRESS TEST PASSED -- 0 consistency violations"
    EXIT_CODE=0
else
    log_fail "STRESS TEST FAILED -- ${ALL_ERRORS} consistency violations detected"
    EXIT_CODE=1
fi

# ---------------------------------------------------------------------------
# Unmount and exit
# ---------------------------------------------------------------------------
log_hdr "Unmount"
log_info "Unmounting ${MOUNT}..."
do_unmount "$MOUNT"
sleep 1

if kill -0 "$FUSE_PID" 2>/dev/null; then
    log_warn "FUSE process still running after unmount -- killing"
    kill -9 "$FUSE_PID" 2>/dev/null || true
fi

# Clear PID so cleanup handler does not try again.
FUSE_PID=""

log_ok "Unmounted successfully"

exit "$EXIT_CODE"
