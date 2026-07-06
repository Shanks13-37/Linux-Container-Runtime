#!/usr/bin/env bash
# setup-rootfs.sh — bootstrap minimal rootfs directories for testing
#
# Creates:
#   rootfs/test-basic
#   rootfs/test-isolation
#   rootfs/test-mon
#   rootfs/test-resources
#   rootfs/test-sched
#
# Must be run on Linux (or WSL2 Ubuntu). Copies a small set of host binaries
# plus their shared libraries into each rootfs, then adds the compiled demo
# workloads from bin/.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

ROOTFS_NAMES=(
    test-basic
    test-isolation
    test-mon
    test-resources
    test-sched
)

SYSTEM_BINS=(
    /usr/bin/dash
    /usr/bin/sh
    /usr/bin/hostname
    /usr/bin/ps
    /usr/bin/sleep
    /usr/bin/echo
    /usr/bin/true
    /usr/bin/false
    /usr/bin/cat
    /usr/bin/find
    /usr/bin/grep
)

log()  { echo "  [setup] $*"; }
warn() { echo "  [warn]  $*"; }

copy_with_libs() {
    local src="$1"
    local destdir="$2"
    local bname

    bname="$(basename "$src")"
    cp -p "$src" "$destdir/bin/$bname"

    ldd "$src" 2>/dev/null | awk '{ print $3 }' | grep -E '^/' | while read -r lib; do
        local libname
        local libdir
        local rel
        local dest_libdir

        libname="$(basename "$lib")"
        libdir="$(dirname "$lib")"
        rel="${libdir#/}"
        dest_libdir="$destdir/$rel"

        mkdir -p "$dest_libdir"
        cp -p "$lib" "$dest_libdir/$libname" 2>/dev/null || true

        if [ -L "$lib" ]; then
            local target
            target="$(readlink -f "$lib")"
            cp -p "$target" "$dest_libdir/$(basename "$target")" 2>/dev/null || true
        fi
    done

    {
        local ld
        ld="$(ldd "$src" 2>/dev/null | grep 'ld-linux\|ld\.so' | awk '{ print $1 }' | head -1)"
        if [ -n "$ld" ] && [ -f "$ld" ]; then
            local ld_libdir
            local rel_ld

            mkdir -p "$destdir/lib64"
            cp -p "$ld" "$destdir/lib64/$(basename "$ld")" 2>/dev/null || true

            ld_libdir="$(dirname "$ld")"
            rel_ld="${ld_libdir#/}"
            if [ -n "$rel_ld" ]; then
                mkdir -p "$destdir/$rel_ld"
                cp -p "$ld" "$destdir/$rel_ld/$(basename "$ld")" 2>/dev/null || true
            fi
        fi
    }
}

if [ ! -f bin/workload-cpu ]; then
    log "building workloads..."
    make workloads -s
fi

for name in "${ROOTFS_NAMES[@]}"; do
    dir="rootfs/$name"

    if [ -f "$dir/.bootstrapped" ]; then
        log "$name: already bootstrapped, skipping"
        continue
    fi

    log "bootstrapping $name..."

    mkdir -p \
        "$dir/bin" \
        "$dir/dev" \
        "$dir/proc" \
        "$dir/sys" \
        "$dir/tmp" \
        "$dir/usr/bin" \
        "$dir/usr/lib" \
        "$dir/lib" \
        "$dir/lib64"

    for bin in "${SYSTEM_BINS[@]}"; do
        if [ -f "$bin" ]; then
            copy_with_libs "$bin" "$dir"
        else
            warn "$bin not found on host, skipping"
        fi
    done

    if [ ! -f "$dir/bin/sh" ] && [ -f "$dir/bin/dash" ]; then
        ln -sf dash "$dir/bin/sh"
    fi

    for wb in bin/workload-*; do
        [ -f "$wb" ] && cp -p "$wb" "$dir/bin/" && log "  copied $(basename "$wb")"
    done

    touch "$dir/.bootstrapped"
    log "$name: done"
done

echo ""
echo "Rootfs setup complete. Run './container-sim' or 'sudo ./container-sim' to start."
