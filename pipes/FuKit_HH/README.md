# FuKit_HH

An FM hi-hat with a built-in multi-mode overdrive (drum pipe,
`com.alecwhiting.fukit_hh`). The hi-hat half of the FuKit pair — see
`pipes/FuKit_BD` for the kick. The ring is a two-operator FM pair at a
deliberately **inharmonic** Ratio (1.41x–6.87x), the metallic cluster of a
cymbal, whose index dies over FmDec so the hit sheds partials as it decays. A
seeded Noise layer adds the air, and a high-pass (HPF, 1–12 kHz) sets where
the body of the hat starts. The hit then runs into the same drive stage as
FuKit_BD: Drive sets the pre-gain, **Sat** picks the shape, Tone rounds off
the very top.

## Saturation types

Identical to FuKit_BD: **Tanh** (smooth, odd harmonics), **Tape** (softest
knee), **Phat** (asymmetric, even harmonics), **Fold** (sine wavefolder —
extra sizzle on hats), **Hard** (flat-top slam), **Diode** (hot top half,
fuzzy). Every shape is hard-bounded (contract M4) and trimmed so loudness
stays roughly flat as Drive rises; a DC block keeps the asymmetric types from
leaving an offset.

## Perform knobs

| knob | what it does |
|---|---|
| **Decay** | the open/closed control: 15 ms closed tick – 900 ms open hat |
| **Drive** | overdrive pre-gain (read live — performs mid-hit) |
| **Sat** | saturation type (also live) |
| **HPF** | high-pass, 1–12 kHz: where the body of the hat starts |

## Other parameters

- **Pitch** — ring carrier, 200 Hz – 2 kHz.
- **FM / Ratio / FmDec** — ring amount, inharmonic modulator ratio (1.41x,
  2.24x, 2.76x, 3.17x, 4.16x, 5.43x, 6.87x), and how fast the cluster sheds
  partials.
- **Noise** — seeded sizzle level.
- **Tone** — post-drive low-pass, 2–18 kHz.
- **Level** — output gain (Mutate never touches it).

Each trigger freezes the hit from the parameters and the event (velocity
scales level, FM and noise; the step's semitone offset transposes the ring and
nudges the HPF) — only Drive, Sat and Tone are read live. The noise is
reseeded from the host seed every hit, so the pipe declares
`DETERMINISTIC_WITH_SEED`.

```
tools/ratpipe build
tools/ratpipe validate FuKit_HH
tools/ratpipe render FuKit_HH --pattern drum16 --out out/fukit_hh.wav
tools/ratpipe install FuKit_HH
```
