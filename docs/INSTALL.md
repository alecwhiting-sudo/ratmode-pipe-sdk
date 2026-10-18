# Installing a pipe into RatMode

> **Developer preview (RatMode 0.1.9+):** loading your own pipes works in Developer mode today; the signed catalogue isn't open yet.

Pipes you build yourself are **local, unsigned builds**. RatMode's normal mode loads
only bundles signed with the RatMode author's Team ID (see [SIGNING.md](SIGNING.md)),
so to try your own pipe you turn on **Developer mode**, which exists for exactly this.
A bundle that works this way shows an **Unverified** badge and is not distributable.

## What you need

* macOS 11 or later (Apple silicon or Intel) with the Xcode Command Line Tools
  (`xcode-select --install`), CMake 3.22 or later and Python 3.
* RatMode, with a licence that reaches CONFIGURE (extensions are picked in CONFIGURE,
  which needs the paid tier). SDK developers can get a free developer key from the
  RatMode author; ask when you start.
* On Linux you can build, validate and render (`ratpipe validate`, `ratpipe render`),
  which is how CI and Claude Code on the web work. You cannot install: RatMode is a
  macOS app.

## Clone to installed bundle

```
git clone https://github.com/alecwhiting-sudo/ratmode-pipe-sdk
cd ratmode-pipe-sdk
tools/ratpipe new MyBass --kind mono        # or --kind drum; creates pipes/MyBass
tools/ratpipe build                         # universal .ratpipe in build/
tools/ratpipe validate MyBass               # must be green before you go further
tools/ratpipe render MyBass --pattern acid16 --out out/MyBass.wav   # listen first
tools/ratpipe install MyBass                # copies to the Extensions folder
```

`install` runs a quick validation first and refuses a pipe that fails it. It copies
the bundle to

```
~/Library/Application Support/RatMode/Extensions/MyBass.ratpipe
```

(RatMode also scans `/Library/Application Support/RatMode/Extensions/` for all users.)
`tools/ratpipe uninstall MyBass` removes it again (by bundle name or by id).

## Turning on Developer mode and allowing your pipe

The one-click flow:

1. Launch RatMode. Open **CONFIGURE** on a slot of the right kind (a drum slot for a
   drum pipe, a melodic slot for mono / poly).
2. Pick **Extension**. Your pipe is listed as *"MyBass (unsigned - needs Developer mode)"*.
   Choose it.
3. RatMode shows the **Developer mode warning**: extensions run inside RatMode with the
   same access as RatMode itself. Read it and choose **Turn on**.
4. A **consent dialog** shows the pipe's name, author, version, bundle path and
   "Unsigned - Developer mode". Choose **Allow**.
5. The slot now shows your pipe's name with an **Unverified** mark and its four
   Perform knobs. Sequence it, save, switch songs, bounce stems: it behaves like any
   other pipe.

Cancelling at step 3 or 4 leaves Developer mode off and the slot unchanged.

The longer route, if you prefer to do it up front: **CONFIGURE > Extension > Manage
extensions...**, tick **Developer mode** (warning first), then press **Allow** on your
pipe's row.

### Rebuilding

RatMode remembers consent per bundle **binary hash**. After you rebuild, the row reads
*"Binary changed since it was allowed (Developer mode)"*; the slot goes to Missing
with its knob values kept, and **Allow** brings it back. That is intended, not a bug.

## When something is off

| what RatMode shows | meaning and fix |
|---|---|
| Not listed at all | wrong folder, or the bundle has no `Contents/Resources/manifest.json`. Rebuild with `ratpipe build` (the manifest is generated), then **Rescan**. |
| *Unsigned - turn on Developer mode to use* | expected in normal mode. Follow the steps above. |
| *Rejected: Not signed by RatMode* | you are in normal mode. Local builds can only load with Developer mode. |
| *Blocked: Binary uses disallowed APIs: ...* | RatMode's load-time binary check refused the build (extra dylib, `LC_RPATH`, a file / network / exec import, a raw syscall). `ratpipe validate` V12 and the notes in [VALIDATOR.md](VALIDATOR.md) catch most of it; do not link libraries beyond libSystem and libc++. |
| *Built for ABI N, this RatMode speaks ABI M* | the SDK checkout and the RatMode build disagree on `RATPIPE_ABI_MAJOR`. Update whichever is older. |
| *Binary missing / Invalid id / macro_prefix invalid or reserved* | a manifest rule, see [MANIFEST.md](MANIFEST.md); the validator reports the same text. |
| *Quarantined after a crash* | RatMode crashed while loading or first rendering the pipe. Find the fault (run it under a debugger or an ASan build of `ratpipe-host`), then **Clear quarantine** in the Extensions dialog. |
| Faulted ("!") | the pipe produced three non-finite blocks and was silenced. V4 shows how; the song's settings are kept. |
| Missing | the bundle was removed or failed to load; the slot keeps the song's record, so installing it again brings the sound back. |

"Reset extension settings" in the Extensions dialog clears consents, quarantine and
strike counts (and turns Developer mode off); it never touches your songs.

## Uninstalling

```
tools/ratpipe uninstall MyBass
```

then **Rescan** (or relaunch). Slots that used it become Missing and silent; the song
file is untouched and the sound returns when you reinstall.
