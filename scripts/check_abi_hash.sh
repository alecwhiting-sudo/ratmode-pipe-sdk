#!/usr/bin/env bash
# scripts/check_abi_hash.sh — the vendored ABI header must never change here.
#
# ratpipe_abi.h is OWNED by RatMode (Source/Ext/ratpipe_abi.h); this repo
# carries a byte-identical copy (the app repo's header is the source of truth). This script fails when:
#   1. include/ratpipe/ratpipe_abi.h does not hash to the value recorded in
#      include/ratpipe/ratpipe_abi.sha256, or
#   2. (optional) a RatMode checkout path is given and its copy differs.
#
# Usage:
#   scripts/check_abi_hash.sh                      # compare against the recorded hash
#   scripts/check_abi_hash.sh /path/to/RatMode     # also compare against Source/Ext/ratpipe_abi.h there
#
# Updating the recorded hash is a deliberate act: copy the new header from
# RatMode, update ratpipe_abi.sha256 in the same commit, and say why.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
hdr="$root/include/ratpipe/ratpipe_abi.h"
rec="$root/include/ratpipe/ratpipe_abi.sha256"

if command -v sha256sum >/dev/null 2>&1; then
    sha() { sha256sum "$1" | awk '{print $1}'; }
else
    sha() { shasum -a 256 "$1" | awk '{print $1}'; }
fi

expected="$(awk '{print $1; exit}' "$rec")"
actual="$(sha "$hdr")"
status=0

echo "expected (ratpipe_abi.sha256): $expected"
echo "actual   (ratpipe_abi.h)     : $actual"
if [ "$expected" != "$actual" ]; then
    echo "FAIL: ratpipe_abi.h differs from the recorded hash. Do not edit it; re-vendor it from RatMode." >&2
    status=1
fi

if [ $# -ge 1 ]; then
    src="$1/Source/Ext/ratpipe_abi.h"
    if [ ! -f "$src" ]; then
        echo "FAIL: $src not found" >&2
        exit 2
    fi
    ratmode="$(sha "$src")"
    echo "RatMode  ($src): $ratmode"
    if [ "$ratmode" != "$actual" ]; then
        echo "FAIL: the SDK copy differs from RatMode's source of truth." >&2
        status=1
    fi
fi

[ $status -eq 0 ] && echo "ABI header OK"
exit $status
