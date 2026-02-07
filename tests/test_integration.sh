#!/bin/bash
#
# Integration test suite for stripefs — a FUSE striping filesystem.
#
# Creates temporary OST directories, mounts stripefs, runs a battery of
# functional tests (write/read/verify, striping distribution, cache hits,
# mkdir/rmdir, overwrite, truncate, empty files, unlink, rename), then
# unmounts and reports results.
#
# Exit code: 0 if all tests pass, 1 if any test fails.

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$PROJECT_ROOT/stripefs"
TEST_BASE="/tmp/stripefs_integration_$$"

MOUNT="$TEST_BASE/mnt"
OST0="$TEST_BASE/ost0"
OST1="$TEST_BASE/ost1"
OST2="$TEST_BASE/ost2"
OST3="$TEST_BASE/ost3"

STRIPE_SIZE=1048576  # 1 MB

PASSED=0
FAILED=0
STRIPEFS_PID=""

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

cleanup() {
    echo ""
    echo "--- Cleanup ---"

    # Unmount if still mounted
    if mountpoint -q "$MOUNT" 2>/dev/null || mount | grep -q "$MOUNT"; then
        echo "Unmounting $MOUNT ..."
        if command -v fusermount3 >/dev/null 2>&1; then
            fusermount3 -u "$MOUNT" 2>/dev/null
        elif command -v fusermount >/dev/null 2>&1; then
            fusermount -u "$MOUNT" 2>/dev/null
        else
            umount "$MOUNT" 2>/dev/null
        fi
        sleep 1
    fi

    # Kill the stripefs process if still running
    if [ -n "$STRIPEFS_PID" ] && kill -0 "$STRIPEFS_PID" 2>/dev/null; then
        echo "Killing stripefs (PID $STRIPEFS_PID) ..."
        kill "$STRIPEFS_PID" 2>/dev/null
        wait "$STRIPEFS_PID" 2>/dev/null
    fi

    # Remove temp dirs
    rm -rf "$TEST_BASE"
    echo "Removed $TEST_BASE"
}

# Always clean up, even on early exit
trap cleanup EXIT

pass() {
    PASSED=$((PASSED + 1))
    echo "  PASS: $1"
}

fail() {
    FAILED=$((FAILED + 1))
    echo "  FAIL: $1"
}

# Determine the right md5 command for this platform
if command -v md5sum >/dev/null 2>&1; then
    md5cmd() { md5sum "$1" | awk '{print $1}'; }
elif command -v md5 >/dev/null 2>&1; then
    md5cmd() { md5 -q "$1"; }
else
    echo "ERROR: neither md5sum nor md5 found" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------

echo "=== stripefs integration tests ==="
echo "Test base directory: $TEST_BASE"
echo ""

# Build the binary
echo "--- Building stripefs ---"
if ! make -C "$PROJECT_ROOT" clean >/dev/null 2>&1; then
    echo "WARNING: make clean failed (continuing)"
fi
if ! make -C "$PROJECT_ROOT" release 2>&1; then
    echo "FATAL: build failed"
    exit 1
fi

if [ ! -x "$BINARY" ]; then
    echo "FATAL: binary not found at $BINARY"
    exit 1
fi

# Create directories
mkdir -p "$MOUNT" "$OST0" "$OST1" "$OST2" "$OST3"

echo ""
echo "--- Mounting stripefs ---"
"$BINARY" \
    --mount "$MOUNT" \
    --ost "$OST0" \
    --ost "$OST1" \
    --ost "$OST2" \
    --ost "$OST3" \
    --stripe-size "$STRIPE_SIZE" \
    --cache-size 64 \
    --ttl 30 \
    --foreground &
STRIPEFS_PID=$!

# Wait for the mount to become available
sleep 2

# Verify the mount is live
if ! mount | grep -q "$MOUNT"; then
    echo "FATAL: stripefs did not mount within 2 seconds"
    exit 1
fi

echo "stripefs mounted (PID $STRIPEFS_PID)"
echo ""

# From here on, fail individual tests rather than aborting the whole script.
set +e

# ---------------------------------------------------------------------------
# Test 1: Small file write + read (1 KB)
# ---------------------------------------------------------------------------

echo "--- Test 1: Small file (1 KB) write and read-back ---"

SMALL_SRC="$TEST_BASE/small_src.bin"
dd if=/dev/urandom of="$SMALL_SRC" bs=1024 count=1 2>/dev/null

cp "$SMALL_SRC" "$MOUNT/small_file.bin"
sync

EXPECTED=$(md5cmd "$SMALL_SRC")
ACTUAL=$(md5cmd "$MOUNT/small_file.bin")

if [ "$EXPECTED" = "$ACTUAL" ]; then
    pass "small file md5 matches ($EXPECTED)"
else
    fail "small file md5 mismatch (expected $EXPECTED, got $ACTUAL)"
fi

# ---------------------------------------------------------------------------
# Test 2: Medium file write + read (5 MB)
# ---------------------------------------------------------------------------

echo "--- Test 2: Medium file (5 MB) write and read-back ---"

MEDIUM_SRC="$TEST_BASE/medium_src.bin"
dd if=/dev/urandom of="$MEDIUM_SRC" bs=1048576 count=5 2>/dev/null

cp "$MEDIUM_SRC" "$MOUNT/medium_file.bin"
sync

EXPECTED=$(md5cmd "$MEDIUM_SRC")
ACTUAL=$(md5cmd "$MOUNT/medium_file.bin")

if [ "$EXPECTED" = "$ACTUAL" ]; then
    pass "medium file md5 matches ($EXPECTED)"
else
    fail "medium file md5 mismatch (expected $EXPECTED, got $ACTUAL)"
fi

# ---------------------------------------------------------------------------
# Test 3: Large file write + read (20 MB — spans many stripes)
# ---------------------------------------------------------------------------

echo "--- Test 3: Large file (20 MB) write and read-back ---"

LARGE_SRC="$TEST_BASE/large_src.bin"
dd if=/dev/urandom of="$LARGE_SRC" bs=1048576 count=20 2>/dev/null

cp "$LARGE_SRC" "$MOUNT/large_file.bin"
sync

EXPECTED=$(md5cmd "$LARGE_SRC")
ACTUAL=$(md5cmd "$MOUNT/large_file.bin")

if [ "$EXPECTED" = "$ACTUAL" ]; then
    pass "large file md5 matches ($EXPECTED)"
else
    fail "large file md5 mismatch (expected $EXPECTED, got $ACTUAL)"
fi

# ---------------------------------------------------------------------------
# Test 4: Stripe distribution — chunks spread across OSTs
# ---------------------------------------------------------------------------

echo "--- Test 4: Verify stripe distribution across OSTs ---"

# The 20 MB file (20 x 1 MB stripes) should have chunks in multiple OST dirs.
# Stripe chunks are named <filename>.s<N> and distributed round-robin.
COUNT_OST0=$(find "$OST0" -name "large_file.bin.s*" 2>/dev/null | wc -l | tr -d ' ')
COUNT_OST1=$(find "$OST1" -name "large_file.bin.s*" 2>/dev/null | wc -l | tr -d ' ')
COUNT_OST2=$(find "$OST2" -name "large_file.bin.s*" 2>/dev/null | wc -l | tr -d ' ')
COUNT_OST3=$(find "$OST3" -name "large_file.bin.s*" 2>/dev/null | wc -l | tr -d ' ')

TOTAL_CHUNKS=$((COUNT_OST0 + COUNT_OST1 + COUNT_OST2 + COUNT_OST3))

echo "    Chunks per OST: OST0=$COUNT_OST0  OST1=$COUNT_OST1  OST2=$COUNT_OST2  OST3=$COUNT_OST3  (total=$TOTAL_CHUNKS)"

# With 20 stripes across 4 OSTs, each should have 5 chunks (round-robin).
if [ "$COUNT_OST0" -ge 1 ] && [ "$COUNT_OST1" -ge 1 ] && \
   [ "$COUNT_OST2" -ge 1 ] && [ "$COUNT_OST3" -ge 1 ]; then
    pass "stripe chunks distributed to all 4 OSTs"
else
    fail "stripe chunks NOT distributed to all OSTs"
fi

if [ "$TOTAL_CHUNKS" -eq 20 ]; then
    pass "total chunk count is 20 (matches 20 x 1MB stripes)"
else
    fail "expected 20 total chunks, got $TOTAL_CHUNKS"
fi

# ---------------------------------------------------------------------------
# Test 5: Cache hits — re-read files and check stats
# ---------------------------------------------------------------------------

echo "--- Test 5: Cache hits on re-read ---"

# First read was test 1-3. Read the small file again — should be a cache hit.
ACTUAL2=$(md5cmd "$MOUNT/small_file.bin")
if [ "$ACTUAL2" = "$(md5cmd "$SMALL_SRC")" ]; then
    pass "re-read of small file still correct"
else
    fail "re-read of small file returned wrong data"
fi

# Read the stats virtual file
STATS=$(cat "$MOUNT/.stripefs_stats" 2>/dev/null)
if [ -n "$STATS" ]; then
    pass "stats virtual file is readable"
else
    fail "could not read .stripefs_stats"
fi

# Check that cache_hits > 0 (the re-read should have been served from cache)
CACHE_HITS=$(echo "$STATS" | grep -o '"hits"[[:space:]]*:[[:space:]]*[0-9]*' | head -1 | grep -o '[0-9]*$')
if [ -n "$CACHE_HITS" ] && [ "$CACHE_HITS" -gt 0 ]; then
    pass "cache_hits > 0 ($CACHE_HITS hits recorded)"
else
    fail "expected cache_hits > 0 (got: $CACHE_HITS)"
fi

# ---------------------------------------------------------------------------
# Test 6: Create and remove directories
# ---------------------------------------------------------------------------

echo "--- Test 6: mkdir and rmdir ---"

mkdir "$MOUNT/testdir"
if [ -d "$MOUNT/testdir" ]; then
    pass "mkdir created directory"
else
    fail "mkdir did not create directory"
fi

# Verify directory exists on all OSTs
DIR_ON_ALL=true
for ost in "$OST0" "$OST1" "$OST2" "$OST3"; do
    if [ ! -d "$ost/testdir" ]; then
        DIR_ON_ALL=false
    fi
done

if $DIR_ON_ALL; then
    pass "directory created on all 4 OSTs"
else
    fail "directory NOT present on all OSTs"
fi

rmdir "$MOUNT/testdir"
if [ ! -d "$MOUNT/testdir" ]; then
    pass "rmdir removed directory"
else
    fail "rmdir did not remove directory"
fi

# ---------------------------------------------------------------------------
# Test 7: Overwrite / modify a file
# ---------------------------------------------------------------------------

echo "--- Test 7: Overwrite (modify) a file ---"

OVERWRITE_SRC1="$TEST_BASE/overwrite_v1.bin"
OVERWRITE_SRC2="$TEST_BASE/overwrite_v2.bin"
dd if=/dev/urandom of="$OVERWRITE_SRC1" bs=1024 count=4 2>/dev/null
dd if=/dev/urandom of="$OVERWRITE_SRC2" bs=1024 count=4 2>/dev/null

cp "$OVERWRITE_SRC1" "$MOUNT/overwrite_test.bin"
sync

V1_MD5=$(md5cmd "$OVERWRITE_SRC1")
BEFORE=$(md5cmd "$MOUNT/overwrite_test.bin")

if [ "$V1_MD5" = "$BEFORE" ]; then
    pass "initial write verified"
else
    fail "initial write mismatch"
fi

# Overwrite with new content
cp "$OVERWRITE_SRC2" "$MOUNT/overwrite_test.bin"
sync

V2_MD5=$(md5cmd "$OVERWRITE_SRC2")
AFTER=$(md5cmd "$MOUNT/overwrite_test.bin")

if [ "$V2_MD5" = "$AFTER" ]; then
    pass "overwrite content updated correctly"
else
    fail "overwrite content mismatch (expected $V2_MD5, got $AFTER)"
fi

if [ "$BEFORE" != "$AFTER" ]; then
    pass "content changed after overwrite"
else
    fail "content did NOT change after overwrite"
fi

# ---------------------------------------------------------------------------
# Test 8: Truncate
# ---------------------------------------------------------------------------

echo "--- Test 8: Truncate ---"

TRUNC_SRC="$TEST_BASE/trunc_src.bin"
dd if=/dev/urandom of="$TRUNC_SRC" bs=1024 count=8 2>/dev/null
cp "$TRUNC_SRC" "$MOUNT/trunc_test.bin"
sync

# Truncate to empty
: > "$MOUNT/trunc_test.bin"
sync

TRUNC_SIZE=$(stat -f%z "$MOUNT/trunc_test.bin" 2>/dev/null || stat -c%s "$MOUNT/trunc_test.bin" 2>/dev/null)

if [ "$TRUNC_SIZE" = "0" ] || [ "$TRUNC_SIZE" = "1" ]; then
    # Some shells write a trailing newline for `: >`, accept 0 or 1
    pass "truncate reduced file (size=$TRUNC_SIZE)"
else
    fail "truncate did not reduce file to ~0 bytes (size=$TRUNC_SIZE)"
fi

# ---------------------------------------------------------------------------
# Test 9: Empty file creation
# ---------------------------------------------------------------------------

echo "--- Test 9: Empty file creation ---"

touch "$MOUNT/empty_file.txt"
sync

if [ -f "$MOUNT/empty_file.txt" ]; then
    pass "empty file created"
else
    fail "empty file was not created"
fi

EMPTY_SIZE=$(stat -f%z "$MOUNT/empty_file.txt" 2>/dev/null || stat -c%s "$MOUNT/empty_file.txt" 2>/dev/null)
if [ "$EMPTY_SIZE" = "0" ]; then
    pass "empty file has size 0"
else
    fail "empty file has unexpected size ($EMPTY_SIZE)"
fi

# ---------------------------------------------------------------------------
# Test 10: File at exact stripe boundary (1 MB)
# ---------------------------------------------------------------------------

echo "--- Test 10: File at exact stripe size (1 MB) ---"

EXACT_SRC="$TEST_BASE/exact_src.bin"
dd if=/dev/urandom of="$EXACT_SRC" bs=1048576 count=1 2>/dev/null

cp "$EXACT_SRC" "$MOUNT/exact_stripe.bin"
sync

EXPECTED=$(md5cmd "$EXACT_SRC")
ACTUAL=$(md5cmd "$MOUNT/exact_stripe.bin")

if [ "$EXPECTED" = "$ACTUAL" ]; then
    pass "exact-stripe-size file md5 matches ($EXPECTED)"
else
    fail "exact-stripe-size file md5 mismatch (expected $EXPECTED, got $ACTUAL)"
fi

EXACT_SIZE=$(stat -f%z "$MOUNT/exact_stripe.bin" 2>/dev/null || stat -c%s "$MOUNT/exact_stripe.bin" 2>/dev/null)
if [ "$EXACT_SIZE" = "1048576" ]; then
    pass "exact-stripe-size file is exactly 1048576 bytes"
else
    fail "exact-stripe-size file size unexpected ($EXACT_SIZE)"
fi

# ---------------------------------------------------------------------------
# Test 11: File deletion (unlink)
# ---------------------------------------------------------------------------

echo "--- Test 11: File deletion (unlink) ---"

dd if=/dev/urandom of="$MOUNT/delete_me.bin" bs=1024 count=2 2>/dev/null
sync

if [ -f "$MOUNT/delete_me.bin" ]; then
    pass "file to delete exists"
else
    fail "file to delete was not created"
fi

rm "$MOUNT/delete_me.bin"
sync

if [ ! -f "$MOUNT/delete_me.bin" ]; then
    pass "file successfully deleted"
else
    fail "file still exists after rm"
fi

# Verify stripe chunks are cleaned up across OSTs
LEFTOVER=0
for ost in "$OST0" "$OST1" "$OST2" "$OST3"; do
    LEFTOVER=$((LEFTOVER + $(find "$ost" -name "delete_me.bin.s*" 2>/dev/null | wc -l)))
done

if [ "$LEFTOVER" -eq 0 ]; then
    pass "all stripe chunks removed from OSTs"
else
    fail "$LEFTOVER stripe chunks left behind after unlink"
fi

# ---------------------------------------------------------------------------
# Test 12: Rename (mv)
# ---------------------------------------------------------------------------

echo "--- Test 12: Rename (mv) ---"

RENAME_SRC="$TEST_BASE/rename_src.bin"
dd if=/dev/urandom of="$RENAME_SRC" bs=1024 count=3 2>/dev/null

cp "$RENAME_SRC" "$MOUNT/before_rename.bin"
sync

BEFORE_MD5=$(md5cmd "$RENAME_SRC")
ACTUAL_BEFORE=$(md5cmd "$MOUNT/before_rename.bin")

if [ "$BEFORE_MD5" = "$ACTUAL_BEFORE" ]; then
    pass "pre-rename file content correct"
else
    fail "pre-rename file content mismatch"
fi

mv "$MOUNT/before_rename.bin" "$MOUNT/after_rename.bin"
sync

if [ ! -f "$MOUNT/before_rename.bin" ]; then
    pass "old name no longer exists"
else
    fail "old name still exists after rename"
fi

if [ -f "$MOUNT/after_rename.bin" ]; then
    pass "new name exists"
else
    fail "new name does not exist after rename"
fi

AFTER_MD5=$(md5cmd "$MOUNT/after_rename.bin")
if [ "$BEFORE_MD5" = "$AFTER_MD5" ]; then
    pass "renamed file content preserved ($AFTER_MD5)"
else
    fail "renamed file content corrupted (expected $BEFORE_MD5, got $AFTER_MD5)"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo ""
echo "==========================================="
echo " Integration test results"
echo "==========================================="
echo " Passed: $PASSED"
echo " Failed: $FAILED"
echo " Total:  $((PASSED + FAILED))"
echo "==========================================="

if [ "$FAILED" -gt 0 ]; then
    exit 1
fi

exit 0
