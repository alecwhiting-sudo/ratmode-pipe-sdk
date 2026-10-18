# manifest.json

Every `.ratpipe` bundle carries `Contents/Resources/manifest.json`. RatMode reads
**only** this file when it scans the Extensions folders (it never loads your code
to list it), so the manifest is what the Extensions dialog and the CONFIGURE menu
show. You do not write it: `add_ratpipe()` generates it from the built descriptor
by running `ratpipe-host --dump-manifest`, so the manifest and the descriptor can
never disagree (the validator's V8 and V12 re-check that they do not).

## Bundle layout

```
MyBass.ratpipe/
  Contents/
    Info.plist                    generated; CFBundleIdentifier == id
    MacOS/com.me.mybass.dylib     MH_DYLIB, universal (x86_64 + arm64); ".so" in Linux CI builds
    Resources/manifest.json       generated
```

The binary is `Contents/MacOS/<id>.dylib` by convention. An explicit
`"binary": "file.dylib"` key (a plain file name, no `/`, not starting with `.`)
overrides it; RatMode never accepts a path.

## Keys RatMode reads

| key | rule |
|---|---|
| `id` | reverse-DNS, lowercase: `^[a-z0-9]+(\.[a-z0-9_]+)+$`, at least one dot, at most 128 characters. Songs store this; never change it once released. Must equal the descriptor id and `CFBundleIdentifier`. |
| `name` | display name, 1 to 24 characters (the Perform row identity zone is narrow). |
| `author` | free text. |
| `version` | semver of the pipe (`1.2.3`). |
| `part_kind` | `"drum"` (receives TRIGGER events), `"mono"` (NOTE_ON/OFF, one voice expected) or `"poly"` (NOTE_ON/OFF; chord expansion arrives in a later milestone, so a poly pipe receives the root note only for now). |
| `macro_prefix` | 2 to 6 lowercase letters. Every parameter id is `<prefix>.<path>`. Must not be one of the reserved prefixes below. |
| `abi_major` | must equal the host's ABI major (currently `1`). A mismatch shows as "Built for ABI N, this RatMode speaks ABI M". |
| `min_ratmode_version` | oldest RatMode that may load it. Default `0.1.8`; override per target with `MIN_RATMODE_VERSION` in `add_ratpipe()`. |
| `perform_knobs` | four parameter indices for the pre-assigned Perform knobs, or `-1` for a placeholder. |
| `binary` | optional, see above. |

Anything else in the file (the SDK adds `sdk_version`, `abi_minor`, `flags` and the
full `params` table for tools) is ignored by RatMode and safe to keep.

### Reserved macro prefixes

`fx sculpt synth gain sample dry ra rb an sv sn sub0 sub1 sub2 sub3 au_param __`
(and anything starting with `__` or `au_param`). RatMode already routes these ids;
a pipe using one is refused by the validator (V2) and by RatMode at load.

## Parameters

Parameters live in the descriptor (see `include/ratpipe/ratpipe_abi.h`, struct
`RatPipeParamDesc`), and are mirrored into the manifest for tools:

| field | rule |
|---|---|
| `id` | `<prefix>.<path>`, lowercase letters, digits, `_` and `.`; unique per pipe. **Stable across versions**: renaming an id orphans saved songs. Add new ids; keep old ones accepted. |
| `label` | knob label, at most 8 characters. |
| `group` | Edit-panel row / section label (may be empty). |
| `default`, `min`, `max` | `min < max`, default inside. Range 0..1, or 0..1.5 with `GAIN_STYLE`. Values cross the ABI normalised; map to engineering units inside the pipe. |
| `mutate_lo`, `mutate_hi` | safe band for Mutate. If equal, the whole `min..max` range is used. Must lie inside `min..max`. |
| `flags` | `discrete` (needs `steps` 2 to 64, evenly spaced positions), `bipolar` (arc drawn from the centre), `gain_style`, `not_mutable`. |
| `readout` | `hz`, `ms`, `pct`, `st` or `none`, with `readout_min` and `readout_max` giving the linear mapping the host prints. |
| `choices` | labels for `discrete` parameters. |

Defaults matter: RatMode replays every parameter's descriptor default and then the
song's stored values on each song switch, so a song that never stored a value gets
your default, never the previous song's live value. Every sound-affecting value must
therefore be a declared parameter (contract M7).
