#!/usr/bin/env bash
# test_isolation.sh — verify namespace and rootfs isolation

set -euo pipefail

BIN="./container-sim"
PASS=0
FAIL=0
ROOTFS="./rootfs/test-isolation"

pass() { echo "  [PASS] $1"; ((++PASS)); }
fail() { echo "  [FAIL] $1"; ((++FAIL)); }

cleanup() {
    rm -f containers.meta containers.meta.tmp
}
trap cleanup EXIT

echo "=== test_isolation.sh ==="
echo ""

echo "--- hostname isolation ---"
out=$(printf 'run iso-hostname mycontainer %s /bin/hostname\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "mycontainer"; then
    pass "container sees its own hostname"
else
    fail "hostname isolation not working"
    echo "$out"
fi

HOST_HOSTNAME=$(hostname)
if echo "$out" | grep -q "$HOST_HOSTNAME"; then
    fail "container leaked the host hostname"
else
    pass "host hostname is not visible inside the container"
fi

echo ""
echo "--- PID namespace summary ---"
out=$(printf 'run iso-pid pidhost %s /bin/echo pid-check\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "ns pid"; then
    pass "startup summary includes namespace PID"
else
    fail "startup summary missing namespace PID"
    echo "$out"
fi
if echo "$out" | grep -Eq 'ns pid[[:space:]]+1'; then
    pass "container process starts as PID 1 inside the namespace"
else
    fail "namespace PID was not reported as 1"
    echo "$out"
fi

echo ""
echo "--- filesystem isolation ---"
out=$(printf 'run iso-fs fstest %s /bin/echo rootfs-ok\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "rootfs-ok"; then
    pass "container executes successfully in isolated rootfs"
else
    fail "container failed to execute in isolated rootfs"
    echo "$out"
fi

echo ""
echo "--- network namespace setup ---"
out=$(printf 'run iso-net nettest %s /bin/workload-netcheck\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "netns:"; then
    pass "workload reports network namespace identity"
else
    fail "network namespace information missing"
    echo "$out"
fi
if echo "$out" | grep -q "lo:"; then
    pass "loopback interface status is visible inside the container"
else
    fail "loopback status missing from network workload"
    echo "$out"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
