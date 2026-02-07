#!/bin/bash
#
# run_tests.sh — Test runner for stripefs
#
# Runs unit tests (test_cache, test_stripe) and integration tests, then
# prints an overall summary and exits non-zero if anything failed.
#

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PASSED=0
FAILED=0
FAILURES=()

run_test() {
    local name="$1"
    shift
    local cmd=("$@")

    echo "==========================================="
    echo " Running: $name"
    echo "==========================================="

    if "${cmd[@]}"; then
        echo "  -> PASSED"
        ((PASSED++))
    else
        echo "  -> FAILED"
        ((FAILED++))
        FAILURES+=("$name")
    fi
    echo ""
}

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------

# Build unit test binaries if not already present
if [ ! -x "$PROJECT_DIR/test_cache" ] || [ ! -x "$PROJECT_DIR/test_stripe" ]; then
    echo "Building unit test binaries..."
    make -C "$PROJECT_DIR" test_cache test_stripe
    echo ""
fi

run_test "Unit: cache" "$PROJECT_DIR/test_cache"
run_test "Unit: stripe" "$PROJECT_DIR/test_stripe"

# ---------------------------------------------------------------------------
# Integration tests
# ---------------------------------------------------------------------------

if [ -x "$PROJECT_DIR/tests/test_integration.sh" ]; then
    run_test "Integration tests" "$PROJECT_DIR/tests/test_integration.sh"
elif [ -f "$PROJECT_DIR/tests/test_integration.sh" ]; then
    chmod +x "$PROJECT_DIR/tests/test_integration.sh"
    run_test "Integration tests" "$PROJECT_DIR/tests/test_integration.sh"
else
    echo "Warning: tests/test_integration.sh not found, skipping integration tests"
    echo ""
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

TOTAL=$((PASSED + FAILED))

echo "==========================================="
echo " Test Summary"
echo "==========================================="
echo "  Total:  $TOTAL"
echo "  Passed: $PASSED"
echo "  Failed: $FAILED"

if [ "$FAILED" -gt 0 ]; then
    echo ""
    echo "  Failed tests:"
    for name in "${FAILURES[@]}"; do
        echo "    - $name"
    done
    echo "==========================================="
    exit 1
fi

echo "==========================================="
echo "  All tests passed."
exit 0
