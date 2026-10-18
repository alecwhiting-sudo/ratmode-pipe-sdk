# Deep Drift

A deep, unstable sub bass (mono pipe, `com.alecwhiting.deepdrift`). Sine
fundamental with a sub sine an octave down, a PolyBLEP saw "growl" layer,
asymmetric tanh overdrive into a ladder low-pass, and a transport-synced
feedback delay with a variable high-pass **inside the loop**, so every echo
repeat loses more low end and the tail never turns to mud under the dry sub.

Instability comes from a bank of four seeded LFOs at mutually detuned rates
(random pitch wander, slow sine wow, random filter wobble, triangle amp
tremble), all scaled by one knob. The bank only moves while a note sounds, so
patterns re-derive identically on every song switch (contract M7).

## Perform knobs

| knob | what it does |
|---|---|
| **Unstab** | depth of all four instability LFOs at once: pitch wander (to ~0.6 st), wow, filter wobble (to ~1.2 oct), amp tremble |
| **Drive** | asymmetric tanh drive before the filter — even harmonics, loudness-compensated |
| **Echo** | level of the delay return |
| **EchoHP** | high-pass frequency inside the echo feedback loop, 30 Hz – 1.5 kHz: low keeps full-weight dub echoes, high thins each repeat |

## Other parameters

- **Rate** — master LFO rate, 0.1–8 Hz; the four LFOs sit at detuned ratios of it.
- **Time** — echo time, synced: 1/16, 1/8T, 1/8, 3/16, 1/4 (120 bpm fallback).
  The delay time is smoothed (~80 ms), so changing Time bends the repeats like
  a tape echo.
- **Feedb** — echo regeneration (loop gain capped at 0.92, tanh-bounded).
- **Sub / Growl** — mix of the octave-down sine and the saw layer.
- **Cutoff / Res** — ladder low-pass; the amp envelope and accent open it.
- **Decay / Glide** — envelope decay and 303-style slide time.
- **Level** — output gain (Mutate never touches it).

303-style phrasing: slide glides without retriggering, accent opens the filter
and adds drive and level. Declares `WANTS_TRANSPORT`, `HAS_TAILS` and
`DETERMINISTIC_WITH_SEED`.

```
tools/ratpipe build
tools/ratpipe validate DeepDrift
tools/ratpipe render DeepDrift --pattern acid16 --out out/deepdrift.wav
tools/ratpipe install DeepDrift
```
