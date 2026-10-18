# FM Kick

An FM kick drum: pitch-swept sine body with a 40 ms FM "knock", a seeded click
through a bounded band-pass, tanh drive. `dev.ratmode.examples.fm_kick`, macro
prefix `fmk.`, drum.

    tools/ratpipe build
    tools/ratpipe validate drum_fm_kick
    tools/ratpipe render   drum_fm_kick --pattern drum16 --bpm 130 --out out/kick.wav

## Perform knobs

| knob | id | what it does |
|---|---|---|
| Pitch | `fmk.pitch` | resting pitch, 35 – 120 Hz; the step's `semitone_offset` transposes from here |
| Decay | `fmk.decay` | amplitude decay, 60 ms – 1.5 s (squared, fine control on short settings) |
| Punch | `fmk.punch` | how many octaves above Pitch the hit starts (0.5 – 3.5), scaled a little by velocity |
| Drive | `fmk.drive` | tanh drive on body + click, loudness-compensated |

Other parameters: `fmk.fm` (FM index at the start of the hit, modulator at 2.5x the
body), `fmk.click` (transient level), `fmk.sweep` (pitch-envelope time 10 – 150 ms),
`fmk.level` (0..1.5, not mutated).

## Velocity and pitch offset

Velocity scales the level (35 % – 100 %), the FM index, the click and the sweep
depth, so ghost notes are rounder and accents snap. `semitone_offset` transposes
the body exactly and the click band-pass by half as much.

## Determinism

The click noise is reseeded from the host seed at every trigger, so the pipe
declares `DETERMINISTIC_WITH_SEED`: identical input gives bit-identical output and
the validator compares a dirtied instance with a fresh one sample for sample.
