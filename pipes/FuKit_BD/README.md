# FuKit_BD

An FM kick with a built-in multi-mode overdrive (drum pipe,
`com.alecwhiting.fukit_bd`). The bass-drum half of the FuKit pair — see
`pipes/FuKit_HH` for the hi-hat. The generator is a sine body pitch-swept down from
Punch octaves above Pitch, phase-modulated by a second sine at a selectable
Ratio (0.5x–5x) whose index dies over FmDec — the "knock" — plus a short
seeded click burst. The hit then runs into the drive stage: Drive sets the
pre-gain, **Sat** picks the shape, and a post-drive Tone low-pass tames the
fizz the dirtier shapes make.

## Saturation types

| Sat | shape | character |
|---|---|---|
| **Tanh** | tanh | smooth, symmetric, odd harmonics — the classic |
| **Tape** | soft tanh, level-compensated knee | softest of the six, glues rather than distorts |
| **Phat** | biased (asymmetric) tanh | even harmonics, warm and thick |
| **Fold** | sine wavefolder | tops fold back on themselves — FM-ish fizz that tracks the sweep |
| **Hard** | hard clip | flat-top slam, the harshest |
| **Diode** | asymmetric tanh gains per half | hot top half / cool bottom — fuzzy, even harmonics |

Every shape is hard-bounded for any input (contract M4) and trimmed so
loudness stays roughly flat as Drive rises; a DC block after the shaper keeps
the asymmetric types (Phat, Diode) from leaving an offset on the swept body.

## Perform knobs

| knob | what it does |
|---|---|
| **Pitch** | resting body pitch, 30–110 Hz |
| **Decay** | amplitude decay, 60 ms – 1.5 s |
| **Drive** | overdrive pre-gain (read live — performs mid-hit) |
| **Sat** | saturation type (also live) |

## Other parameters

- **Punch / Sweep** — pitch-envelope depth (octaves above Pitch at the hit)
  and time.
- **FM / Ratio / FmDec** — knock amount, modulator ratio (0.5x, 1x, 1.5x, 2x,
  2.5x, 3.5x, 5x; the non-integer ratios go metallic), knock decay.
- **Click** — seeded noise transient through a bounded band-pass.
- **Tone** — post-drive low-pass, 200 Hz – 18 kHz.
- **Level** — output gain (Mutate never touches it).

Each trigger freezes the hit from the parameters and the event (velocity
scales level, FM and punch; the step's semitone offset transposes the body) —
only Drive, Sat and Tone are read live. The click is reseeded from the host
seed every hit, so the pipe declares `DETERMINISTIC_WITH_SEED`.

```
tools/ratpipe build
tools/ratpipe validate FuKit_BD
tools/ratpipe render FuKit_BD --pattern drum16 --out out/fukit_bd.wav
tools/ratpipe install FuKit_BD
```
