# The RatMode pipe contract

Every pipe built with this SDK is untrusted native code running inside the RatMode
process. The rules below are what keeps a third-party pipe from glitching, crashing
or corrupting the host. `ratpipe validate` (see [VALIDATOR.md](VALIDATOR.md)) checks
as many of them as can be checked mechanically; RatMode re-checks the cheap ones at
load and sanitises the output at run time. The validator is the gate, not a hint.

The sections below are the contract as specified in the RatMode repository
(the RatMode extension contract), reproduced here. "M" numbers are what the validator's
messages refer to.


### 8.1 MUST-NOT-BREAK (and how each is enforced)

| # | Rule | Enforcement |
|---|---|---|
| M1 | No allocation, locks, blocking syscalls or file IO in `render`. All buffers sized in `create/prepare` for `max_block`. | **Validator** V5/V6; **runtime** none possible — the base class makes it easy (pre-sized `std::array`/`std::vector` in ctor) |
| M2 | Params are written by `set_param` on the message thread and read in `render` via atomics; no other shared state. | **SDK base** generates the atomics from the table; validator V8; code review in the skill |
| M3 | Output is finite and bounded: never NaN/Inf; peak ≤ 2.0 under any param combination. Guard every feedback path. | **Validator** V4; **runtime** `ExtPipe` sanitiser (non-finite → 0 + strike; clamp ±4) |
| M4 | Bounded saturation only (tanh family). No cubic/polynomial soft-clip anywhere a signal feeds back. | **Kit** ships only tanh-family shapers; validator V13 (static warn); V4 catches the blow-up |
| M5 | Band-limited oscillators (PolyBLEP or equivalent) for saw/square/tri. | **Kit** default; validator spectral check on a 2 kHz saw (alias floor warn-only) |
| M6 | Delay/feedback coefficients clamped (|fb| ≤ 0.95), DC-blocked where appropriate. | Kit `delay_line` clamps; V4 |
| M7 | **All sound-affecting state is a declared parameter.** After `all_notes_off + flush_tails` and setting every param, two instances are indistinguishable. No hidden persisted state, no reading files at render, no clock/time dependence except via `RatPipeTransport`. | **Validator** V9/V10/V11; **host** replays defaults then the song's macros on every song switch |
| M8 | Param ids: `<prefix>.<path>`, lowercase, dotted; prefix 2–6 letters, not reserved; stable across versions (renaming an id orphans saved songs — add new ids, keep old ones accepted). | **Validator** V2; **host** load-time lint refuses the bundle |
| M9 | Values are normalised 0..1 (or 0..1.5 `GAIN_STYLE`) across the ABI; engineering units are mapped inside the pipe. | V3/V8; `ExtEditPanel` assumes it |
| M10 | `render` is additive into L/R; never clears, never writes outside `n`. | Validator (guard samples around the buffer) |
| M11 | `create/destroy/prepare/set_param` may be called while `render` runs on another thread; `destroy` is only called after the host has quiesced render. The extension must not spawn threads that touch audio state. | Host handshake; validator thread-sanitizer run (optional CI job) |
| M12 | No JUCE, no Objective-C runtime, no exceptions across the ABI, only `ratpipe_entry` exported. | `add_ratpipe` flags; V1 symbol check; base `try/catch(...)` |
| M13 | UI: logical units only, no display-scale reads, no native views, no timers; the host owns the window. | By construction (M1/M2 have no extension UI code; M3 canvas API exposes no scale or window) |
| M14 | Universal binary, macOS 11 deployment target, Info.plist id == manifest id == descriptor id. | V12; host load-time check |
| M15 | Descriptor and manifest are static and cheap: `ratpipe_entry` must not allocate large state, open files or start threads (discovery may call it in a sandboxed `ratpipe-host`, RatMode never calls it at scan time). | Validator timing on `ratpipe_entry` (< 5 ms) |

### 8.2 FREE-TO-CHANGE (conventions, not rules)

- The DSP itself: any synthesis/sampling approach, any voice count, any internal sample-rate handling, oversampling, your own blocks instead of the kit (kit use is recommended, not required — the validator is the gate).
- Parameter count, grouping, labels, ordering, readout kinds, mutate bands, which four are the Perform knobs.
- Display name, author, version scheme, bundle name (the id is what the song stores).
- Part kind (drum / mono / poly) and how velocity, accent, slide and `semitone_offset` are interpreted (document it in the manifest's `notes`; the SDK recommends 303-style accent/slide semantics for melodic pipes).
- Theme tokens and badge (M2); custom paint region (M3).
- Build system details inside the bundle (any compiler, any C++ standard ≥ 17, static libc++ or not), project layout, tests beyond the validator, licence of the extension.
- Whether the pipe is deterministic (declare `DETERMINISTIC_WITH_SEED` and use the host seed) or free-running noise.
- Default values (within the declared bounds) and how the pipe sounds at defaults — though the SDK's house rule is "audible and useful at defaults, never silent, never clipping" and V4 checks the clipping half.

---

## Notes for pipe authors (SDK, not part of the RatMode text above)

### Runtime-length `memset` / `std::fill` / `vector::assign` and older RatMode builds

RatMode checks every import of a bundle against an allowlist when it loads it
(`Source/Ext/ExtMachOCheck.cpp`; the SDK validator's V12 runs the same check on a
Mach-O binary). Apple clang lowers a *runtime-length* zeroing loop — `memset(p, 0, n)`,
`std::fill(p, p + n, 0.0f)`, `vec.assign(n, 0.0f)`, even a hand-written
`for (i < n) p[i] = 0` — to `___bzero` on x86_64 and a constant non-zero fill loop to
`_memset_pattern16`. RatMode builds released before the allowlist learnt those names
refuse such a bundle at load, whether or not the code ever runs: the import alone is
enough.

Use `ratpipe::kit::zeroBuffer(float*, size_t)` (in `kit/finite_guard.hpp`) for every
runtime-length clear, on any thread; it stores a zero loaded through `volatile`, which
the compiler cannot turn into a library call. The base and the kit already do
(`pipe_base` scratch, `DelayLine::clear`). Constant-size `memset` of a small struct is
fine (it is inlined). Prefer writing every output sample in `render()` over clearing
first; `L`/`R` arrive zeroed anyway.
