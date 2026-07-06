#!/usr/bin/env bash
# test_scheduler.sh — verify scheduler toggles and background containers

set -euo pipefail

BIN="./container-sim"
PASS=0
FAIL=0
ROOTFS="./rootfs/test-sched"

pass() { echo "  [PASS] $1"; ((++PASS)); }
fail() { echo "  [FAIL] $1"; ((++FAIL)); }

cleanup() {
    rm -f containers.meta containers.meta.tmp
}
trap cleanup EXIT

echo "=== test_scheduler.sh ==="
echo ""

echo "--- scheduler status (off by default) ---"
out=$(printf 'sched status\nexit\n' | "$BIN" 2>&1)
if echo "$out" | grep -q "mode=disabled"; then
    pass "scheduler is disabled by default"
else
    fail "scheduler should start disabled"
    echo "$out"
fi

echo ""
echo "--- sched on/off toggle ---"
out=$(printf 'sched on\nsched status\nsched off\nsched status\nexit\n' | "$BIN" 2>&1)
if echo "$out" | grep -q "mode=enabled"; then
    pass "sched on enables the scheduler"
else
    fail "sched on did not enable the scheduler"
    echo "$out"
fi
if echo "$out" | grep -q "mode=disabled"; then
    pass "sched off disables the scheduler"
else
    fail "sched off did not disable the scheduler"
    echo "$out"
fi

echo ""
echo "--- sched slice ---"
out=$(printf 'sched slice 100\nsched status\nexit\n' | "$BIN" 2>&1)
if echo "$out" | grep -q "slice=100ms"; then
    pass "sched slice updates the time slice"
else
    fail "sched slice change not reflected in status"
    echo "$out"
fi

echo ""
echo "--- background containers under scheduler ---"
rm -f containers.meta containers.meta.tmp
out=$(printf \
'sched on
runbg sched-a host-a %s /bin/sleep 20
runbg sched-b host-b %s /bin/sleep 20
list
stop container-0001
stop container-0002
delete container-0001
delete container-0002
exit
' "$ROOTFS" "$ROOTFS" | "$BIN" 2>&1)

RUNNING=$(echo "$out" | grep -c "RUNNING" || true)
if [ "$RUNNING" -ge 2 ]; then
    pass "two containers are visible as RUNNING under the scheduler"
else
    fail "expected two running containers under scheduler"
    echo "$out"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
