// FuKit-CP — a noise clap with a built-in multi-mode overdrive, two
// assignable LFOs and a synced tape delay (RATPIPE_DRUM). The clap of the
// FuKit family (pipes/FuKit_BD, pipes/FuKit_HH).
//
// Generator — a classic layered clap, all seeded noise:
//   three pre-echo spikes Spread ms apart (each ~2 ms, falling in level),
//   then a tail whose length is Decay — the "crowd" after the slap.
//   The noise runs through a high-pass then a low-pass (HPF / LPF, both
//   bounded SVFs): the two cutoffs are the clap's whole tone shape.
//
// Overdrive — identical to the rest of the family: Drive sets the pre-gain,
// Sat picks the shape. All six shapes are hard-bounded for any input
// (contract M4 — no polynomial clippers), each trimmed so loudness stays
// roughly flat as Drive rises:
//
//   Tanh   tanh                      smooth, symmetric, odd harmonics
//   Tape   kit::sat::softTape        softer knee, level-compensated
//   Phat   kit::sat::asymTanh        biased tanh, even harmonics, warm
//   Fold   sine wavefolder           sin(pre*x): tops fold back
//   Hard   hard clip                 flat-top slam, harshest of the six
//   Diode  asymmetric tanh gains     hot positive / cool negative half
//
// Two LFOs, each with Rate, Amt, Wave (sine / tri / saw / square / S&H /
// smoothed random) and a Dest picked from inside the generator:
//
//   Off     no routing
//   LPF     low-pass cutoff, +/- 2 octaves at full Amt
//   HPF     high-pass cutoff, +/- 2 octaves
//   Drive   overdrive pre-gain, +/- half the knob
//   Spread  pre-echo spacing, +/- 50 % — sampled at each TRIGGER, so every
//           hit lands with a different flam
//   Send    delay send, 0..2x — rhythmic throws
//
// Both LFOs are seeded and only run while the clap is audible, so patterns
// re-derive identically on every song switch (contract M7, validator V9).
//
// Tape delay — mid-panned (dead centre, like everything here): the send-level
// dry signal is tape-saturated into a feedback loop with a fixed one-pole
// damp, so every repeat gets darker and rounder; the delay time is smoothed
// (~80 ms), so changing Time bends the repeats like a real tape echo. Time is
// synced to the host transport (1/32, 1/16, 1/8T, 1/8, 3/16, 1/4, 1/2; 120
// bpm when the host gives none); Send sets what goes in, Feedb the
// regeneration (loop gain capped at 0.92, tanh-bounded).
//
// Each TRIGGER freezes the hit (spread, spike times, tail decay, velocity,
// the step's semitone offset nudging both filters); Drive, Sat, the cutoffs,
// the LFOs and the delay are read live. The noise is reseeded from the host
// seed on every hit (RATPIPE_FLAG_DETERMINISTIC_WITH_SEED): same seed, same
// events, bit-identical output. ALL_OFF silences the clap and reseeds the
// LFOs; the delay tail is allowed to ring (HAS_TAILS); FLUSH also clears the
// line (validator V9/V10/V11).
#include <ratpipe/pipe_base.hpp>
#include <ratpipe/kit/kit.hpp>

namespace {

namespace kit = ratpipe::kit;

// Appending is safe; reordering or renaming an id orphans saved songs (M8).
enum
{
    kDecay = 0,    // tail decay                                             Perform 1
    kDrive,        // overdrive pre-gain                                      Perform 2
    kLpf,          // low-pass cutoff                                         Perform 3
    kHpf,          // high-pass cutoff                                        Perform 4
    kSat,          // saturation type (discrete)
    kSpread,       // pre-echo spacing
    kL1Rate,       // LFO 1 rate
    kL1Amt,        // LFO 1 depth
    kL1Wave,       // LFO 1 waveform (discrete)
    kL1Dest,       // LFO 1 destination (discrete)
    kL2Rate,       // LFO 2 rate
    kL2Amt,        // LFO 2 depth
    kL2Wave,       // LFO 2 waveform (discrete)
    kL2Dest,       // LFO 2 destination (discrete)
    kTime,         // delay time, synced (discrete)
    kSend,         // delay send
    kFeedb,        // delay feedback
    kLevel,        // output gain 0..1.5, Mutate never touches it
    kParamCount
};

const char* const kSatNames[6]  = { "Tanh", "Tape", "Phat", "Fold", "Hard", "Diode" };
const char* const kWaveNames[6] = { "Sine", "Tri", "Saw", "Square", "S&H", "Rand" };
const char* const kDestNames[6] = { "Off", "LPF", "HPF", "Drive", "Spread", "Send" };
enum { kDestOff = 0, kDestLpf, kDestHpf, kDestDrive, kDestSpread, kDestSend };
const char* const kTimeNames[7] = { "1/32", "1/16", "1/8T", "1/8", "3/16", "1/4", "1/2" };
const double      kTimeBeats[7] = { 0.125, 0.25, 1.0 / 3.0, 0.5, 0.75, 1.0, 2.0 };

// The clap: three pre-echo spikes then the tail (index 3), levels falling.
constexpr int   kSpikes = 4;
const float     kSpikeAmp[kSpikes] = { 1.0f, 0.82f, 0.68f, 0.95f };

class FuKitCpPipe : public ratpipe::Pipe<FuKitCpPipe, kParamCount>
{
public:
    static constexpr ratpipe::PipeInfo kInfo {
        "com.alecwhiting.fukit_cp", "FuKit-CP", "alecwhiting", "1.0.0",
        RATPIPE_DRUM, "fukc",
        { kDecay, kDrive, kLpf, kHpf },
        RATPIPE_FLAG_WANTS_TRANSPORT | RATPIPE_FLAG_HAS_TAILS | RATPIPE_FLAG_DETERMINISTIC_WITH_SEED
    };

    // id, label (<= 8), group, default, then readout / mutate band / flags.
    static constexpr ratpipe::ParamSpec kParams[kParamCount] = {
        ratpipe::ParamSpec("fukc.decay",  "Decay",   "Env",    0.45f).ms(40.0f, 1200.0f).mutate(0.10f, 0.85f),
        ratpipe::ParamSpec("fukc.drive",  "Drive",   "Drive",  0.35f).pct().mutate(0.00f, 0.90f),
        ratpipe::ParamSpec("fukc.lpf",    "LPF",     "Filter", 0.85f).hz(500.0f, 18000.0f).mutate(0.30f, 1.00f),
        ratpipe::ParamSpec("fukc.hpf",    "HPF",     "Filter", 0.35f).hz(100.0f, 8000.0f).mutate(0.10f, 0.80f),
        ratpipe::ParamSpec("fukc.sat",    "Sat",     "Drive",  0.00f).discrete(6, kSatNames),
        ratpipe::ParamSpec("fukc.spread", "Spread",  "Env",    0.50f).ms(4.0f, 20.0f).mutate(0.10f, 0.90f),
        ratpipe::ParamSpec("fukc.l1rate", "L1 Rate", "LFO 1",  0.40f).hz(0.05f, 16.0f).mutate(0.10f, 0.80f),
        ratpipe::ParamSpec("fukc.l1amt",  "L1 Amt",  "LFO 1",  0.00f).pct().mutate(0.00f, 0.80f),
        ratpipe::ParamSpec("fukc.l1wave", "L1 Wave", "LFO 1",  0.00f).discrete(6, kWaveNames),
        ratpipe::ParamSpec("fukc.l1dest", "L1 Dest", "LFO 1",  0.20f).discrete(6, kDestNames),   // LPF
        ratpipe::ParamSpec("fukc.l2rate", "L2 Rate", "LFO 2",  0.55f).hz(0.05f, 16.0f).mutate(0.10f, 0.80f),
        ratpipe::ParamSpec("fukc.l2amt",  "L2 Amt",  "LFO 2",  0.00f).pct().mutate(0.00f, 0.80f),
        ratpipe::ParamSpec("fukc.l2wave", "L2 Wave", "LFO 2",  0.20f).discrete(6, kWaveNames),   // Tri
        ratpipe::ParamSpec("fukc.l2dest", "L2 Dest", "LFO 2",  1.00f).discrete(6, kDestNames),   // Send
        ratpipe::ParamSpec("fukc.time",   "Time",    "Tape",   0.50f).discrete(7, kTimeNames),   // 1/8
        ratpipe::ParamSpec("fukc.send",   "Send",    "Tape",   0.25f).pct().mutate(0.00f, 0.85f),
        ratpipe::ParamSpec("fukc.feedb",  "Feedb",   "Tape",   0.40f).pct().mutate(0.00f, 0.80f),
        ratpipe::ParamSpec("fukc.level",  "Level",   "Out",    1.00f).gain().notMutable(),
    };

    explicit FuKitCpPipe(const ratpipe::CreateArgs& a) : Pipe(a) { onPrepare(a.sampleRate, a.maxBlock); }

    void onPrepare(double sr, int /*maxBlock*/)
    {
        sr_ = sr > 1000.0 ? sr : 44100.0;
        hpf_.prepare(sr_);
        hpf_.setBounded(true);                                    // tanh-bounded states: cannot ring away
        lpf_.prepare(sr_);
        lpf_.setBounded(true);
        dc_.prepare(sr_, 10.0f);
        lfo1_.prepare(sr_);
        lfo2_.prepare(sr_);
        line_.prepare(static_cast<int>(sr_ * 2.2));               // 1/2 note down to ~55 bpm (clamped below that)
        dcLoop_.prepare(sr_, 10.0f);                              // in the loop: DC cannot recirculate (M6)
        dampC_ = 1.0f - std::exp(-2.0f * 3.14159265f * 3500.0f / static_cast<float>(sr_));  // tape darkening
        dCoef_ = 1.0 - std::exp(-1.0 / (0.08 * sr_));             // ~80 ms on the delay time: tape bend
        onFlush();
    }

    // ── events ──────────────────────────────────────────────────────────────

    void onTrigger(const ratpipe::TriggerEvent& e)
    {
        const float vel = e.velocity;
        const float semis = static_cast<float>(e.semitoneOffset);

        // Spread, warped by any LFO routed to it — sampled now, so each hit
        // lands with a different flam while the LFO moves.
        float spreadS = 0.004f + 0.016f * param(kSpread);
        spreadS *= 1.0f + 0.5f * lfoTo(kDestSpread);
        spreadS = kit::clamp(spreadS, 0.002f, 0.040f);
        for (int s = 0; s < kSpikes; ++s)
            spikeAt_[s] = static_cast<int>(static_cast<float>(s) * spreadS * static_cast<float>(sr_));

        const float d = param(kDecay);
        const float decayS = 0.04f + 1.16f * d * d;
        tailK_ = std::exp(-1.0f / (decayS * 0.15f * static_cast<float>(sr_)));   // ~-60 dB after decayS
        preK_  = std::exp(-1.0f / (0.002f * static_cast<float>(sr_)));           // each pre-echo: ~2 ms
        filtTrans_ = kit::semitonesToRatio(0.5f * semis);                        // the step's offset nudges both filters
        noise_.seed(hostSeed() ^ 0x66754b634350ull);                             // same clap every hit: deterministic

        t_        = 0;
        spikeIdx_ = 0;
        burst_    = 0.0f;
        envK_     = preK_;
        vel_      = 0.35f + 0.65f * vel;
        quiet_    = false;                                        // repeats may ring from here on
    }

    // ALL_OFF silences the clap and reseeds the LFOs; the delay tail is
    // allowed to ring (HAS_TAILS). FLUSH also clears the line.
    void onAllOff()
    {
        burst_ = 0.0f;
        spikeIdx_ = kSpikes;
        t_ = 1 << 30;
        hpf_.reset();
        lpf_.reset();
        dc_.reset();
        lfo1_.reset();
        lfo1_.seed(hostSeed() ^ 0x9e3779b97f4a7c15ull);
        lfo2_.reset();
        lfo2_.seed(hostSeed() ^ 0x3c6ef372fe94f82aull);
        l1_ = l2_ = 0.0f;
        vel_ = 1.0f;
    }

    void onFlush()
    {
        onAllOff();
        line_.clear();
        dcLoop_.reset();
        damp_ = 0.0f;
        quiet_ = true;    // nothing audible until the next hit: delay time may jump, no tape bend
    }

    // Once per host render call: delay time from the transport, LFO setup.
    void onBlockStart(int /*n*/)
    {
        const int step = paramStep(kTime);
        const double beats = kTimeBeats[step < 0 ? 0 : (step > 6 ? 6 : step)];
        const RatPipeTransport& t = transport();
        double bpm = 120.0;                                       // fallback when the host has no transport
        if (hasTransport() && t.playing != 0 && t.bpm > 20.0 && t.bpm < 1000.0)
            bpm = t.bpm;
        dTarget_ = kit::clamp(static_cast<float>(beats * 60.0 / bpm * sr_),
                              1.0f, static_cast<float>(line_.maxDelay()));
        if (quiet_) dSm_ = dTarget_;    // keeps snapping until the first hit after a flush (M7:
                                        // a dirtied and a fresh instance must converge identically)

        lfo1_.setWave(static_cast<kit::Lfo::Wave>(paramStep(kL1Wave)));
        lfo2_.setWave(static_cast<kit::Lfo::Wave>(paramStep(kL2Wave)));
        lfo1_.setRate(0.05f * std::exp2(param(kL1Rate) * 8.3219f));   // 0.05 .. 16 Hz
        lfo2_.setRate(0.05f * std::exp2(param(kL2Rate) * 8.3219f));
    }

    // ── audio ───────────────────────────────────────────────────────────────

    void render(float* L, float* R, int n)
    {
        if (quiet_) return;                                       // flushed and untriggered: exactly silent

        const float driveKnob = param(kDrive);
        const int   sat   = paramStep(kSat);
        const float level = param(kLevel);
        const float send  = param(kSend);
        const float fb    = 0.92f * param(kFeedb);
        const float lpfBase = 500.0f * kit::semitonesToRatio(param(kLpf) * 62.0f) * filtTrans_;   // 500 Hz .. 18 kHz
        const float hpfBase = 100.0f * kit::semitonesToRatio(param(kHpf) * 75.9f) * filtTrans_;   // 100 Hz .. 8 kHz
        const float outNorm = 1.0f / std::tanh(1.2f);             // output ceiling makeup

        float drive = driveKnob, sendEff = send;
        float pre = 1.0f + 9.0f * drive;
        float tanhNorm = 1.0f, phatTrim = 1.0f, foldTrim = 1.0f, hardTrim = 1.0f, bias = 0.1f;

        for (int i = 0; i < n; ++i)
        {
            // Control rate (32 samples): the LFOs, their routings, both
            // filters, the drive trims. The LFOs only run while the clap is
            // audible, so after ALL_OFF + FLUSH their phases match a fresh
            // instance exactly (M7).
            if ((i & 31) == 0)
            {
                const bool active = burst_ > 1.0e-6f || spikeIdx_ < kSpikes;
                if (active)
                {
                    l1_ = lfo1_.tick(32) * param(kL1Amt);
                    l2_ = lfo2_.tick(32) * param(kL2Amt);
                }
                drive   = kit::clamp01(driveKnob + 0.5f * lfoTo(kDestDrive));
                sendEff = kit::clamp01(send * (1.0f + lfoTo(kDestSend)));
                pre      = 1.0f + 9.0f * drive;
                tanhNorm = 1.0f / std::tanh(pre);
                phatTrim = 1.0f / (1.0f + 0.9f * drive);
                foldTrim = 1.0f - 0.20f * drive;
                hardTrim = 1.0f - 0.15f * drive;
                bias     = 0.1f + 0.5f * drive;
                lpf_.set(kit::clamp(lpfBase * std::exp2(2.0f * lfoTo(kDestLpf)), 200.0f, 18000.0f), 0.1f);
                hpf_.set(kit::clamp(hpfBase * std::exp2(2.0f * lfoTo(kDestHpf)), 40.0f, 12000.0f), 0.1f);
            }

            // Generator: the spike train (three pre-echoes, then the tail).
            float v = 0.0f;
            if (burst_ > 1.0e-6f || spikeIdx_ < kSpikes)
            {
                while (spikeIdx_ < kSpikes && t_ >= spikeAt_[spikeIdx_])
                {
                    burst_ = kSpikeAmp[spikeIdx_];
                    envK_  = spikeIdx_ == kSpikes - 1 ? tailK_ : preK_;   // the last spike is the tail
                    ++spikeIdx_;
                }
                float x = noise_.process() * burst_;
                burst_ *= envK_;
                ++t_;

                // Tone: high-pass then low-pass (both bounded SVFs).
                x = lpf_.processLp(hpf_.processHp(x));

                // Overdrive: pre-gain, one of six bounded shapes, per-type makeup.
                float y;
                switch (sat)
                {
                    default:
                    case 0: y = std::tanh(x * pre) * tanhNorm;                  break;  // Tanh
                    case 1: y = kit::sat::softTape(x, drive);                   break;  // Tape (trim built in)
                    case 2: y = kit::sat::asymTanh(x, pre, bias) * phatTrim;    break;  // Phat
                    case 3: y = std::sin(1.5707963f * x * pre) * foldTrim;      break;  // Fold (sin: bounded)
                    case 4: y = kit::clamp(x * pre, -1.0f, 1.0f) * hardTrim;    break;  // Hard
                    case 5: y = std::tanh(x * pre * (x > 0.0f ? 1.6f : 0.6f))   // Diode: hot top half
                                * tanhNorm;                                     break;
                }

                v = dc_.process(y) * vel_;                        // DC block: Phat / Diode are asymmetric
            }

            // Tape delay, mid-panned: smoothed time (tape bend), tape-saturated
            // send, a fixed one-pole damp and a DC block inside the loop — the
            // damp makes every repeat darker, the DC block keeps the onset
            // transients' offset from recirculating (at fb 0.92 the loop would
            // multiply any DC by ~12). Tanh bound on the regeneration (M4/M6:
            // loop gain < 1, bounded non-linearity only).
            dSm_ += dCoef_ * (static_cast<double>(dTarget_) - dSm_);
            const float wet = line_.read(static_cast<float>(dSm_));
            damp_ += dampC_ * (wet - damp_);
            line_.write(kit::sat::fastTanh(v) * sendEff + fb * kit::sat::fastTanh(dcLoop_.process(damp_)));

            // Output: soft tanh ceiling (the filters and DC block can overshoot
            // the shaper's bound, so the bound is taken here). tanh < 1,
            // outNorm ~= 1.2, 0.7 * level <= 1.05: peak <= 1.27, under 2.0 (M3).
            const float out = std::tanh((v + wet) * 1.2f) * outNorm * 0.7f * level;
            L[i] = out;
            R[i] = out;
        }
    }

private:
    /// Summed bipolar modulation from whichever LFOs point at `dest`.
    float lfoTo(int dest) const noexcept
    {
        float m = 0.0f;
        if (paramStep(kL1Dest) == dest) m += l1_;
        if (paramStep(kL2Dest) == dest) m += l2_;
        return kit::clamp(m, -1.0f, 1.0f);
    }

    double sr_ = 48000.0;
    double dSm_ = 0.0, dCoef_ = 0.0;
    float  dTarget_ = 1.0f, dampC_ = 0.3f, damp_ = 0.0f;
    bool   quiet_ = true;
    int    t_ = 0, spikeIdx_ = kSpikes;
    int    spikeAt_[kSpikes] = { 0, 0, 0, 0 };
    float  burst_ = 0.0f, envK_ = 0.99f, preK_ = 0.99f, tailK_ = 0.999f;
    float  filtTrans_ = 1.0f;
    float  vel_ = 1.0f;
    float  l1_ = 0.0f, l2_ = 0.0f;
    kit::Svf        hpf_;
    kit::Svf        lpf_;
    kit::DcBlock    dc_;
    kit::DcBlock    dcLoop_;
    kit::WhiteNoise noise_;
    kit::Lfo        lfo1_;
    kit::Lfo        lfo2_;
    kit::DelayLine  line_;
};

} // namespace

RATPIPE_DEFINE_ENTRY(FuKitCpPipe)
