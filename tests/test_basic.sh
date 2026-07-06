#!/usr/bin/env bash
# test_basic.sh — smoke tests for core lifecycle commands

set -euo pipefail

BIN="./container-sim"
PASS=0
FAIL=0
ROOTFS="./rootfs/test-basic"

pass() { echo "  [PASS] $1"; ((++PASS)); }
fail() { echo "  [FAIL] $1"; ((++FAIL)); }

cleanup() {
    rm -f containers.meta containers.meta.tmp container.log
}
trap cleanup EXIT

echo "=== test_basic.sh ==="
echo ""

rm -f containers.meta containers.meta.tmp container.log

echo "--- create ---"
out=$(printf 'create basic-test myhost %s\nexit\n' "$ROOTFS" | "$BIN" 2>&1)
if echo "$out" | grep -q "created container-"; then
    pass "create returns a container id"
else
    fail "create did not return a container id"
    echo "$out"
fi

CID=$(echo "$out" | grep -oE 'container-[0-9]+' | head -1)
if [ -z "$CID" ]; then
    echo "Could not determine container ID; aborting."
    exit 1
fi

echo ""
echo "--- list ---"
out=$(printf 'list\nexit\n' | "$BIN" 2>&1)
if echo "$out" | grep -q "$CID"; then
    pass "list shows the created container"
else
    fail "list does not show the created container"
fi
if echo "$out" | grep -q "CREATED"; then
    pass "list shows CREATED state"
else
    fail "list missing CREATED state"
fi

echo ""
echo "--- delete ---"
out=$(printf 'delete %s\nexit\n' "$CID" | "$BIN" 2>&1)
if echo "$out" | grep -q "deleted $CID"; then
    pass "delete succeeds for a created container"
else
    fail "delete failed"
    echo "$out"
fi

echo ""
echo "--- metadata file ---"
if [ -f containers.meta ]; then
    pass "containers.meta is written"
else
    fail "containers.meta was not created"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
