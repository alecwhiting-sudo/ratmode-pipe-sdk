# Mach-O fixtures

`sdk_smoke_fat.dylib` is `tests/kit/smoke_pipe.cpp` (pipe_base + kit) cross-built
from Linux as a universal (arm64 + x86_64) `MH_DYLIB` with
`clang++ -target <arch>-apple-macos11 -O2 -fvisibility=hidden -fstack-protector-strong`
and `ld64.lld -dylib -dead_strip -install_name @loader_path/dev.ratmode.sdk_smoke.dylib`
against stub `libSystem` / `libc++` tbds, then joined with a fat header. It links
nothing but libSystem and libc++ and imports only libm, `memcpy`, operator new/delete
and the C++ EH runtime.

`host/tests/test_validator.py` feeds it to `ratpipe-host --check-macho` — RatMode's
own load-time check (`host/src/vendor/ExtMachOCheck.cpp`) — and expects OK with two
slices; a truncated copy must be refused. This is what lets the Linux CI job prove the
vendored checker runs and agrees with RatMode on a real pipe binary, not just that it
compiles. It is not loaded or executed anywhere.
