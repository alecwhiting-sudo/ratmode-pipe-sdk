# The validator (`ratpipe validate`)

`ratpipe validate [Name|bundle]` runs `ratpipe-host validate`. It loads the bundle
(or a raw `.so` / `.dylib`), runs checks V1 to V13 and exits non-zero if any fails.
`--quick` uses 20 fuzz seeds instead of 200; `--json` prints a machine-readable
report; `--only V4,V9` runs a subset. The validation itself runs in a child process,
so a pipe that crashes is reported as a failure (`result: crash`) instead of taking
the host with it.

| # | Check | Fails when |
|---|---|---|
| V1 ABI | `ratpipe_entry` exported and **only** that symbol exported; `abi_major` matches; descriptor and every `params[]` element `struct_size` correct; required function pointers set | any |
| V2 Identity | id regex, name at most 24 characters, macro prefix valid and not reserved, every param id `prefix.`-rooted, lowercase, unique, label at most 8 characters | any |
| V3 Bounds | `min < max`, default inside, mutate band inside, discrete steps 2 to 64, perform knob indices valid | any |
| V4 Render fuzz | 44.1k / 48k / 96k Hz x block sizes 32 to 1024 and ragged x patterns (drum16, acid16 with slide and accent, chords, silence) x parameters at default / min / max / random x 200 seeds with live parameter jitter | any non-finite sample; peak above 2.0; DC above -40 dB after 1 s without events; a write outside `[0, n)`; `render` not additive; an exception escaping the ABI; `create` returning NULL |
| V5 RT allocation | global `malloc` / `free` / `operator new` are interposed during `render` only | any allocation or free inside `render` |
| V6 RT IO / locks | `open`, `fopen`, `read`, `write`, `printf`, mutex / rwlock / condition-variable / semaphore waits, `sleep`, dispatch calls are interposed during `render` only | any call |
| V7 Timing | worst-case `render` time at 64 samples (best of three passes); `ratpipe_entry` duration | above 50 % of the block's real-time budget (warning above 25 %); entry above 5 ms. Scale for slow machines with `--timing-slack 2` or `RATPIPE_TIMING_SLACK` |
| V8 Param round-trip | `set_param` then `get_param` returns the same value for min / max / mid / default; the manifest matches the descriptor | any difference |
| V9 Song-switch | instance A: random parameters, play 4 bars, `ALL_OFF + RELEASE_HELD + FLUSH`, every parameter back to default, play the pattern again; fresh instance B plays the same pattern | A differs from B by more than -60 dBFS (sample-exact, for pipes declaring `DETERMINISTIC_WITH_SEED`) or more than 2 dB in 100 ms RMS (other pipes) after the first 100 ms |
| V10 Silence | `ALL_OFF + FLUSH` while a note is held, at default and maximum parameters | output above -80 dBFS between 2.0 s and 2.5 s afterwards |
| V11 Determinism | two instances, same host seed, parameters and events | not bit-identical (only checked when `DETERMINISTIC_WITH_SEED` is declared) |
| V12 Bundle | `Info.plist` id and executable, `manifest.json` rules (the same ones RatMode applies), manifest id; on macOS also `lipo -archs` has x86_64 and arm64, `otool -L` shows only libSystem / libc++ / libc++abi, no `LC_RPATH` | any |
| V13 Saturation audit | static grep of the sources (`--src DIR`; the CLI passes the pipe's folder) for `x*x*x`, `pow(x, 3)` and cubic soft-clip idioms | warning only: a polynomial clipper in a feedback path has crashed RatMode before (contract M4) |

## What it cannot prove

* **V5 / V6 are interposition, not proof.** On Linux the host replaces the C and C++
  allocators and the common IO / lock / sleep entry points, and counts calls made
  while the pipe is inside `render`. It does not see inlined system calls, a private
  allocator that maps memory itself, futex-based locks that bypass pthread, or
  spinning on atomics. Passing means "no obvious violation", not "real-time safe".
  Sanitizer builds of the host and non-glibc systems skip V5 / V6 and say so.
* **macOS interposition is unverified.** On macOS the same counting is done by
  `libratpipe_rtguard.dylib` (`DYLD_INTERPOSE`); `ratpipe-host validate` re-executes
  itself with it inserted. That path was written without access to a Mac and has not
  been run; if the dylib is missing V5 and V6 report SKIP rather than a false pass.
* **Races.** V4 also calls `set_param` from a second thread during `render`. A data
  race shows only under ThreadSanitizer; build your pipe and the host with
  `-fsanitize=thread` for that (V5 / V6 skip in that configuration).
* **Timing** depends on the machine. V7 is a guard against gross overruns, not a
  profile.

## Reading a failure

Each failure names the contract rule it breaks (M1 to M15, see
[CONTRACT.md](CONTRACT.md)) and where it happened: sample rate, block size, pattern,
parameter set and, for the fuzz runs, the seed. Reproduce one with
`ratpipe render Name --params max --pattern acid16 --block 64` and listen, or run
`ratpipe-host validate build/Name.ratpipe --only V4`.
