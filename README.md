# ratmode-pipe-sdk

> **Developer preview (RatMode 0.1.9+):** build, validate and load your own instrument pipes in Developer mode. The ABI may still gain additive features; the catalogue for reviewed, signed extensions isn't open yet.

Build your own **pipes** (drum and melodic voices) for [RatMode](https://divurj.com) as
native extension bundles. A pipe is a small C-ABI plug-in with no JUCE and no runtime
dependencies: you write the DSP, the SDK checks it against the same contract RatMode
enforces, and RatMode runs it in a slot with everything built-in pipes get for free
(Mix, Sculpt, stems, pattern mutate, MIDI learn, Perform knobs).

Use this repository as a GitHub template (or clone it), add your pipe under `pipes/`,
and iterate with the command line. MIT licence.

> **Status: milestone 1.** The ABI header, build system, offline host / validator,
> CLI, C++ helper base (`pipe_base.hpp`), DSP kit, starter templates and two worked
> examples (`examples/mono_fm_wobble`, `examples/drum_fm_kick`) are in place.

## What works today / Coming later

**Requirements:** RatMode 0.1.9 or later, macOS 11+, Xcode Command Line Tools, CMake 3.22+, Python 3.

**What works today**

* Mono and drum instrument pipes, with `pipe_base` and the DSP kit.
* The validator.
* `ratpipe new`, `build`, `validate`, `render`, `install` and `package`.
* Loading in Developer mode.
* In RatMode: the Edit window, Perform knobs and mutate.

**Coming later**

* Catalogue submission and signing (loading without Developer mode).
* Poly and chord pipes.
* Colour themes from the SDK.
* Recipes and a Claude skill for generating pipes from a description.
* Preview images.

## Quickstart

macOS 11+ with Xcode Command Line Tools, CMake 3.22+ and Python 3. Hear a finished
pipe first — the gritty FM bass with a wobble macro from `examples/`:

```
git clone https://github.com/alecwhiting-sudo/ratmode-pipe-sdk
cd ratmode-pipe-sdk

tools/ratpipe build                                                     # host + every example
tools/ratpipe validate mono_fm_wobble                                   # V1-V13, all green
tools/ratpipe render mono_fm_wobble --pattern acid16 --out out/wobble.wav   # 8 s at 130 bpm
tools/ratpipe install mono_fm_wobble                                    # try it in RatMode (Developer mode)
```

Then make your own:

```
tools/ratpipe new MyBass --kind mono        # drum | mono; creates pipes/MyBass/
tools/ratpipe build                         # build/MyBass.ratpipe (universal, ad-hoc signed)
tools/ratpipe validate MyBass               # V1-V13: has to be green
tools/ratpipe render MyBass --pattern acid16 --out out/MyBass.wav    # hear it, no RatMode needed
tools/ratpipe install MyBass                # -> ~/Library/Application Support/RatMode/Extensions/
```

Then in RatMode: turn on **Developer mode**, open CONFIGURE, pick your pipe under
**Extension**, read the warning and press **Allow** ([docs/INSTALL.md](docs/INSTALL.md)).
Your pipe appears with an "Unverified" badge and four Perform knobs. Edit
`pipes/MyBass/src/Pipe.cpp` (a commented `pipe_base.hpp` starter: parameter table,
event hooks, `render()`), run `tools/ratpipe build && tools/ratpipe install MyBass`
again, and Allow the rebuilt binary. When the starter feels small, copy an example
from `examples/` into `pipes/` instead and change its id.

On Linux everything except `install` works (CI uses this), so the loop also runs in
Claude Code on the web or any CI box.

## The rules, and what is yours

A pipe must keep a short list of rules (no allocation or locks in `render`, finite and
bounded output, all sound-affecting state in declared parameters, one exported symbol,
...): [docs/CONTRACT.md](docs/CONTRACT.md). Everything else is yours: the DSP, how many
parameters, their names and order, which four are the Perform knobs, how velocity,
accent and slide are interpreted. `ratpipe validate` is the arbiter
([docs/VALIDATOR.md](docs/VALIDATOR.md)).

## Layout

```
include/ratpipe/ratpipe_abi.h    the C ABI, byte-identical to RatMode's copy (never edit; CI checks its hash)
include/ratpipe/pipe_base.hpp    C++17 helper base class (parameter atomics, event dispatch, guards)
include/ratpipe/kit/             header-only DSP kit: PolyBLEP oscillators, tanh saturators, filters, envelopes ...
cmake/RatPipe.cmake              add_ratpipe(): the one way to build a bundle
host/                            ratpipe-host: offline host, renderer and validator (pure C++17, macOS + Linux)
tools/ratpipe                    the command line (Python 3, no dependencies)
template/                        what `ratpipe new` copies
pipes/                           your pipes (created by `ratpipe new`; built automatically)
examples/                        worked example pipes (see below)
docs/                            CONTRACT, MANIFEST, VALIDATOR, INSTALL, SIGNING
scripts/check_abi_hash.sh        the ABI-header integrity check CI runs
```

## Examples

Both are complete pipes built from `pipe_base.hpp` and the kit, validated at 200
seeds on every push, and laid out exactly as `ratpipe new` lays out yours. Each has
a README explaining its knobs.

| pipe | kind | what it shows |
|---|---|---|
| [`examples/mono_fm_wobble`](examples/mono_fm_wobble) | mono | two-operator FM + sub, asymmetric tanh grit, ladder filter, a wobble LFO synced to the host transport (Rate 1/4, 1/8, 1/16, 1/8T), 303-style slide and accent, discrete parameters, mutate bands, `WANTS_TRANSPORT` |
| [`examples/drum_fm_kick`](examples/drum_fm_kick) | drum | FM kick with pitch envelope, seeded click through a bounded band-pass, tanh drive; honours `semitone_offset` and velocity; per-hit reseeding keeps it `DETERMINISTIC_WITH_SEED` |

```
tools/ratpipe render drum_fm_kick --pattern drum16 --out out/kick.wav
tools/ratpipe info   mono_fm_wobble            # the parameter table RatMode will see
```

## `add_ratpipe`

```cmake
add_ratpipe(MyBass
    ID       com.me.mybass         # lowercase reverse-DNS; must equal the id in your source
    SOURCES  src/Pipe.cpp)
```

This produces `MyBass.ratpipe/Contents/{Info.plist, MacOS/com.me.mybass.dylib,
Resources/manifest.json}` with the link hygiene RatMode's loader requires (a real
dylib, universal x86_64 + arm64, macOS 11 minimum, hidden visibility with only
`ratpipe_entry` exported, no rpath, no `-ffast-math`), generates the manifest and
plist from your descriptor ([docs/MANIFEST.md](docs/MANIFEST.md)) and ad-hoc signs it.

## Distribution

Bundles you build are for your own testing. RatMode's normal mode loads only bundles
signed with the RatMode author's Team ID; shipping a pipe means submitting its source
to the `ratmode-pipes` catalogue, whose CI builds and signs it
([docs/SIGNING.md](docs/SIGNING.md)).

## Checks that run on every push

`.github/workflows/ci.yml`: ABI-header hash; Linux build, the kit tests, the
validator's own fixtures, both examples at 200 seeds (`ctest`), their demo renders
(uploaded as `audition-wavs`) and the `ratpipe new` smoke test for both starters; a
macOS universal build with `lipo`, `otool`, export and `codesign` checks on every
bundle.

## Licence

MIT, see [LICENSE](LICENSE). The ABI header, kit and examples are MIT; RatMode itself
is not part of this repository.
