#!/usr/bin/env bash
# scripts/check_macho_sync.sh — the vendored Mach-O checker must match RatMode's.
#
# host/src/vendor/ExtMachOCheck.{h,cpp} are RatMode's Source/Ext/ExtMachOCheck.{h,cpp}
# (the load-time "binary permissions" check: Mach-O shape, load commands, dylib and
# import allowlists, raw-syscall scan) with an SDK provenance banner on top. The
# validator's V12 runs them so that "passes V12" means "RatMode will load it". This
# script fails when:
#   1. a vendored file, with its banner removed, does not hash to the value recorded
#      in host/src/vendor/ExtMachOCheck.sha256, or
#   2. (optional) a RatMode checkout path is given and its Source/Ext copy differs.
#
# Usage:
#   scripts/check_macho_sync.sh                      # compare against the recorded hashes
#   scripts/check_macho_sync.sh /path/to/RatMode     # also compare against RatMode's files
#
# Updating is a deliberate act: re-copy both files from RatMode (keep the banner,
# refresh the commit hash in it), update ExtMachOCheck.sha256 in the same commit, and
# say which RatMode commit / allowlist change prompted it.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
vend="$root/host/src/vendor"
rec="$vend/ExtMachOCheck.sha256"
marker='end of SDK provenance banner'

if command -v sha256sum >/dev/null 2>&1; then
    sha() { sha256sum | awk '{print $1}'; }
else
    sha() { shasum -a 256 | awk '{print $1}'; }
fi

# Everything after the banner marker line, byte for byte.
strip_banner() { awk -v m="$marker" 'found { print; next } index($0, m) { found = 1 }' "$1"; }

status=0
for f in ExtMachOCheck.cpp ExtMachOCheck.h; do
    if ! grep -q "$marker" "$vend/$f"; then
        echo "FAIL: $vend/$f has no provenance banner marker ('$marker')" >&2
        status=1
        continue
    fi
    expected="$(awk -v f="$f" '$2 == f { print $1; exit }' "$rec")"
    actual="$(strip_banner "$vend/$f" | sha)"
    echo "$f"
    echo "  expected (ExtMachOCheck.sha256): $expected"
    echo "  actual   (vendored, banner off): $actual"
    if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
        echo "FAIL: $f differs from the recorded hash. Do not edit it; re-vendor it from RatMode." >&2
        status=1
    fi
    if [ $# -ge 1 ]; then
        src="$1/Source/Ext/$f"
        if [ ! -f "$src" ]; then
            echo "FAIL: $src not found" >&2
            exit 2
        fi
        ratmode="$(sha < "$src")"
        echo "  RatMode  ($src): $ratmode"
        if [ "$ratmode" != "$actual" ]; then
            echo "FAIL: the SDK copy of $f differs from RatMode's source of truth (RatMode's allowlist moved?)." >&2
            status=1
        fi
    fi
done

[ $status -eq 0 ] && echo "Mach-O checker in sync"
exit $status
