# FuKit-CP

A noise clap with a built-in multi-mode overdrive, two assignable LFOs and a
tempo-synced tape delay (drum pipe, `com.alecwhiting.fukit_cp`). The clap of
the FuKit family — see `pipes/FuKit_BD` (kick) and `pipes/FuKit_HH` (hat).

The generator is a classic layered clap, all seeded noise: three pre-echo
spikes **Spread** ms apart (each ~2 ms, falling in level), then a tail whose
length is **Decay**. The noise runs through a high-pass then a low-pass —
the two cutoffs (**HPF** / **LPF**) are the clap's whole tone shape — and
into the same drive stage as the rest of the family.

## Saturation types

Identical to FuKit_BD / FuKit_HH: **Tanh**, **Tape**, **Phat**, **Fold**,
**Hard**, **Diode**. Every shape is hard-bounded (contract M4) and trimmed so
loudness stays roughly flat as Drive rises; a DC block keeps the asymmetric
types from leaving an offset.

## LFOs

Two of them, each with **Rate** (0.05–16 Hz), **Amt**, **Wave** (sine, tri,
saw, square, sample & hold, smoothed random) and a **Dest**:

| Dest | what it moves |
|---|---|
| **Off** | nothing |
| **LPF** | low-pass cutoff, ±2 octaves at full Amt |
| **HPF** | high-pass cutoff, ±2 octaves |
| **Drive** | overdrive pre-gain, ± half the knob |
| **Spread** | pre-echo spacing, ±50% — sampled at each trigger, so every hit lands with a different flam |
| **Send** | delay send, 0–2x — rhythmic throws |

Both LFOs can point at the same Dest (they sum). They are seeded and only run
while the clap is audible, so patterns re-derive identically on every song
switch (contract M7).

## Tape delay

Mid-panned (dead centre). The send-level signal is tape-saturated into a
feedback loop with a fixed one-pole damp, so every repeat gets darker and
rounder; the delay time is smoothed (~80 ms), so changing **Time** bends the
repeats like a real tape echo.

- **Time** — synced: 1/32, 1/16, 1/8T, 1/8, 3/16, 1/4, 1/2 (120 bpm fallback).
- **Send** — what goes into the loop.
- **Feedb** — regeneration (loop gain capped at 0.92, tanh-bounded).

## Perform knobs

**Decay**, **Drive**, **LPF**, **HPF**.

## Other parameters

- **Sat** — saturation type.
- **Spread** — pre-echo spacing, 4–20 ms.
- **Level** — output gain (Mutate never touches it).

Each trigger freezes the hit (spread, spike times, tail decay, velocity; the
step's semitone offset nudges both filters) — Drive, Sat, the cutoffs, the
LFOs and the delay are read live. Declares `WANTS_TRANSPORT`, `HAS_TAILS` and
`DETERMINISTIC_WITH_SEED`.

```
tools/ratpipe build
tools/ratpipe validate FuKit-CP
tools/ratpipe render FuKit-CP --pattern drum16 --out out/fukit_cp.wav
tools/ratpipe install FuKit-CP
```
