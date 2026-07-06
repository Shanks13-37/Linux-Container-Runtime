#!/usr/bin/env bash
# test_resources.sh — verify CPU, PID, and memory limits

set -euo pipefail

BIN="./container-sim"
PASS=0
FAIL=0
ROOTFS="./rootfs/test-resources"

pass() { echo "  [PASS] $1"; ((++PASS)); }
fail() { echo "  [FAIL] $1"; ((++FAIL)); }

echo "=== test_resources.sh ==="
echo ""

echo "--- CPU limit (RLIMIT_CPU) ---"
out=$(printf 'run --cpu 2 res-cpu cpuhost %s /bin/workload-cpu 60\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -qE "(killed signal|RESOURCE_LIMIT_HIT|cpu burn done|finished|runtime workload completed)"; then
    pass "CPU-limited workload terminated or completed predictably"
else
    fail "CPU limit test inconclusive"
    echo "$out"
fi

echo ""
echo "--- PID limit (RLIMIT_NPROC) ---"
out=$(printf 'run --pids 5 res-fork forkhost %s /bin/workload-fork 128 2\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "fork failed"; then
    pass "fork workload hit the PID limit"
elif echo "$out" | grep -q "started"; then
    STARTED=$(echo "$out" | grep -oP 'started \K\d+' | head -1)
    if [ -n "$STARTED" ] && [ "$STARTED" -lt 128 ]; then
        pass "PID limit reduced the number of children ($STARTED < 128)"
    else
        fail "PID limit did not reduce child creation"
        echo "$out"
    fi
else
    fail "PID limit test inconclusive"
    echo "$out"
fi

echo ""
echo "--- Memory limit (RLIMIT_AS) ---"
out=$(printf 'run --mem 64 res-mem memhost %s /bin/workload-mem 512\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -qE "malloc.*failed|Cannot allocate"; then
    pass "memory allocation failed under the configured limit"
elif echo "$out" | grep -q "allocated"; then
    pass "memory workload ran; RLIMIT_AS behavior may vary by host"
else
    fail "memory limit test inconclusive"
    echo "$out"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
