#!/usr/bin/env bash
# test_monitoring.sh — verify logs and stats commands

set -euo pipefail

BIN="./container-sim"
PASS=0
FAIL=0
ROOTFS="./rootfs/test-mon"

pass() { echo "  [PASS] $1"; ((++PASS)); }
fail() { echo "  [FAIL] $1"; ((++FAIL)); }

cleanup() {
    rm -f containers.meta containers.meta.tmp container.log
}
trap cleanup EXIT

echo "=== test_monitoring.sh ==="
echo ""

echo "--- container-specific logs ---"
rm -f containers.meta containers.meta.tmp container.log
out=$(printf 'runbg mon-log host-log %s /bin/sleep 10\nlogs container-0001\nstop container-0001\ndelete container-0001\nexit\n' \
    "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "CONTAINER_CREATED"; then
    pass "logs shows lifecycle entries for the container"
else
    fail "logs output missing lifecycle entries"
    echo "$out"
fi
if echo "$out" | grep -q "container-0001"; then
    pass "logs are filterable by container id"
else
    fail "container-specific logs missing container id"
    echo "$out"
fi

echo ""
echo "--- logs tail ---"
rm -f containers.meta containers.meta.tmp container.log
out=$(printf 'runbg mon-tail host-tail %s /bin/sleep 10\nlogs -n 2 container-0001\nstop container-0001\ndelete container-0001\nexit\n' \
    "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "CONTAINER_STARTED"; then
    pass "logs -n prints recent lifecycle lines"
else
    fail "logs -n did not show recent lifecycle lines"
    echo "$out"
fi

echo ""
echo "--- stats for one container ---"
rm -f containers.meta containers.meta.tmp
out=$(printf 'runbg mon-stats host-stats %s /bin/sleep 10\nstats container-0001\nstop container-0001\ndelete container-0001\nexit\n' \
    "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "Container Monitoring"; then
    pass "stats prints the monitoring table"
else
    fail "stats table header missing"
    echo "$out"
fi
if echo "$out" | grep -q "container-0001"; then
    pass "stats shows the requested container"
else
    fail "stats missing the requested container row"
    echo "$out"
fi

echo ""
echo "--- stats for all running containers ---"
rm -f containers.meta containers.meta.tmp
out=$(printf 'runbg mon-all host-all %s /bin/sleep 10\nstats\nstop container-0001\ndelete container-0001\nexit\n' \
    "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "Monitor profile"; then
    pass "stats prints overall monitor profile information"
else
    fail "stats all missing monitor profile"
    echo "$out"
fi
if echo "$out" | grep -q "container-0001"; then
    pass "stats all shows the running container"
else
    fail "stats all missing running container"
    echo "$out"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
