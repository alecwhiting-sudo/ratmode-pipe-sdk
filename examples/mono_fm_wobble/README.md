# FM Wobble

A gritty two-operator FM bass with a tempo-synced wobble macro — the pipe the
README quickstart renders. `dev.ratmode.examples.fm_wobble`, macro prefix `fmw.`,
mono.

    tools/ratpipe build
    tools/ratpipe validate mono_fm_wobble
    tools/ratpipe render   mono_fm_wobble --pattern acid16 --bpm 130 --out out/wobble.wav

## Perform knobs

| knob | id | what it does |
|---|---|---|
| Wobble | `fmw.wobble` | depth of the cutoff LFO: 0 = none, 1 = four octaves below Cutoff at the bottom of each cycle |
| Cutoff | `fmw.cutoff` | ladder cutoff at rest, 60 Hz – 8 kHz; the envelope and accent open it from there |
| FM | `fmw.fm` | modulation index (squared, so the lower half is subtle); the mod envelope and accent add to it |
| Grit | `fmw.grit` | asymmetric tanh drive before the filter, DC-blocked, loudness-compensated |

Other parameters: `fmw.rate` (wobble cycle 1/4, 1/8, 1/16, 1/8T), `fmw.ratio`
(modulator : carrier 1/2, 1, 2, 3, 4), `fmw.sub` (sub sine one octave down),
`fmw.decay`, `fmw.res`, `fmw.glide`, `fmw.level` (0..1.5, not mutated).

## How the wobble stays on the beat

`onBlockStart` reads `transport().tick / ticks_per_beat`, divides by the beats per
cycle for the chosen Rate and takes the fraction as the LFO phase for sample 0 of
the block. The phase is therefore a pure function of the song position (contract
M7: nothing hidden carries over between songs), and it lands on the bar wherever
playback starts. Without a transport, or when it is stopped, the LFO free-runs at
120 bpm from the last known phase.

## Phrasing

Slide (`SLIDE_FROM_PREV`) glides the pitch and keeps both envelopes running;
accent opens the filter two more octaves, adds FM index and 30 % level; velocity
scales level and the mod envelope.
