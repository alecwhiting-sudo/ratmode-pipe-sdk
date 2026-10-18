#!/usr/bin/env bash
# Standalone build + run of the kit / pipe_base unit tests, no CMake needed.
#
#   tests/kit/build.sh            # clang++ and g++ (whichever exist), ASan+UBSan, -Werror
#   CXX=g++ tests/kit/build.sh    # one compiler only
#   SAN=0 tests/kit/build.sh      # without sanitizers
#
# Also builds smoke_pipe.cpp as a shared library with the SDK's visibility
# flags and checks that `ratpipe_entry` is the only exported symbol (the
# validator's V1 rule), using nm when available.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${OUT:-$HERE/build}"
mkdir -p "$OUT"

SAN="${SAN:-1}"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror"
STD="-std=c++17"
SRCS="$HERE/main.cpp $HERE/test_kit.cpp $HERE/test_pipe_base.cpp $HERE/smoke_pipe.cpp"

if [ -n "${CXX:-}" ]; then
    COMPILERS="$CXX"
else
    COMPILERS=""
    command -v clang++ >/dev/null 2>&1 && COMPILERS="$COMPILERS clang++"
    command -v g++     >/dev/null 2>&1 && COMPILERS="$COMPILERS g++"
fi
[ -n "$COMPILERS" ] || { echo "no C++ compiler found" >&2; exit 1; }

status=0
for cxx in $COMPILERS; do
    name="$(basename "$cxx")"
    flags="$STD $WARN -O1 -g -fno-omit-frame-pointer -I$ROOT/include"
    if [ "$SAN" != "0" ]; then
        flags="$flags -fsanitize=address,undefined -fno-sanitize-recover=undefined"
    fi
    echo "== $name: unit tests"
    # shellcheck disable=SC2086
    "$cxx" $flags -o "$OUT/kit_tests_$name" $SRCS -lm
    if ! "$OUT/kit_tests_$name"; then status=1; fi

    echo "== $name: smoke pipe as a shared library (hidden visibility)"
    # shellcheck disable=SC2086
    "$cxx" $STD $WARN -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden \
        -I"$ROOT/include" -o "$OUT/smoke_pipe_$name.so" "$HERE/smoke_pipe.cpp" -lm
    if command -v nm >/dev/null 2>&1; then
        exports="$(nm -D --defined-only "$OUT/smoke_pipe_$name.so" | awk '{print $NF}' | grep -v -E '^(_init|_fini|__bss_start|_edata|_end)$' || true)"
        echo "   exports: $(echo $exports)"
        if [ "$exports" != "ratpipe_entry" ]; then
            echo "   FAIL: expected exactly one exported symbol, ratpipe_entry" >&2
            status=1
        fi
        echo "   imports: $(nm -D --undefined-only "$OUT/smoke_pipe_$name.so" | awk '{print $NF}' | tr '\n' ' ')"
    fi
done
exit $status
