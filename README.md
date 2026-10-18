# ratmode-pipe-sdk

Build your own **pipes** (drum and melodic voices) for [RatMode](https://divurj.com) as
native extension bundles. A pipe is a small C-ABI plug-in with no JUCE and no runtime
dependencies: you write the DSP, the SDK checks it against the same contract RatMode
enforces, and RatMode runs it in a slot with everything built-in pipes get for free
(Mix, Sculpt, stems, pattern mutate, MIDI learn, Perform knobs).

Use this repository as a GitHub template (or clone it), add your pipe under `pipes/`,
and iterate with the command line. MIT licence.

> **Status: milestone 1.** The ABI header, build system, offline host / validator and
> CLI are in place. The C++ helper base (`pipe_base.hpp`), the DSP kit and the example
> pipes are being added alongside; the starter templates are written against the raw
> C ABI so they build with nothing else.

## Quickstart

macOS 11+ with Xcode Command Line Tools, CMake 3.22+ and Python 3:

```
git clone https://github.com/alecwhiting-sudo/ratmode-pipe-sdk
cd ratmode-pipe-sdk

tools/ratpipe new MyBass --kind mono        # drum | mono; creates pipes/MyBass/
tools/ratpipe build                         # build/MyBass.ratpipe (universal, ad-hoc signed)
tools/ratpipe validate MyBass               # V1-V13: has to be green
tools/ratpipe render MyBass --pattern acid16 --out out/MyBass.wav    # hear it, no RatMode needed
tools/ratpipe install MyBass                # -> ~/Library/Application Support/RatMode/Extensions/
```

Then in RatMode: turn on **Developer mode**, open CONFIGURE, pick your pipe under
**Extension**, read the warning and press **Allow** ([docs/INSTALL.md](docs/INSTALL.md)).
Your pipe appears with an "Unverified" badge and four Perform knobs. Edit
`pipes/MyBass/src/Pipe.cpp`, run `tools/ratpipe build && tools/ratpipe install MyBass`
again, and Allow the rebuilt binary.

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
examples/                        worked example pipes
docs/                            CONTRACT, MANIFEST, VALIDATOR, INSTALL, SIGNING
scripts/check_abi_hash.sh        the ABI-header integrity check CI runs
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

`.github/workflows/ci.yml`: ABI-header hash, Linux build + validation of the host
fixtures, the template starters and (when present) the kit tests, and a macOS
universal build with `lipo` checks.

## Licence

MIT, see [LICENSE](LICENSE). The ABI header, kit and examples are MIT; RatMode itself
is not part of this repository.
