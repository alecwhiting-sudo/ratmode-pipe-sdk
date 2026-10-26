// FuKit_HH — an FM hi-hat with a built-in multi-mode overdrive (RATPIPE_DRUM).
// The hi-hat half of the FuKit pair (see pipes/FuKit_BD for the kick).
//
// Generator:
//   ring:  sine carrier, phase-modulated by a second sine at a deliberately
//          inharmonic Ratio (1.41x..6.87x) — the metallic cluster of a cymbal.
//          The FM index decays over FmDec, so the ring starts clangorous and
//          sheds partials as it dies
//   noise: seeded white noise (Noise) — the air and sizzle
//   both through a bounded high-pass (HPF): the body of the hat lives above it
//
// Overdrive — identical to FuKit_BD: Drive sets the pre-gain, Sat picks the
// shape. All six shapes are hard-bounded for any input (contract M4 — no
// polynomial clippers), each trimmed so loudness stays roughly flat as Drive
// rises:
//
//   Tanh   tanh                      smooth, symmetric, odd harmonics
//   Tape   kit::sat::softTape        softer knee, level-compensated
//   Phat   kit::sat::asymTanh        biased tanh, even harmonics, warm
//   Fold   sine wavefolder           sin(pre*x): tops fold back, extra sizzle
//   Hard   hard clip                 flat-top slam, harshest of the six
//   Diode  asymmetric tanh gains     hot positive / cool negative half, fuzzy
//
// A DC block follows the shaper (Phat and Diode are asymmetric), then Tone, a
// post-drive low-pass, rounds off the very top. Amp envelope, velocity and
// Level finish the chain. Decay is the open/closed control: 15 ms is a tight
// closed tick, 900 ms an open hat.
//
// Each TRIGGER freezes the hit from the parameters and the event: the step's
// semitone_offset transposes the ring (and the HPF with it), velocity scales
// level, FM index and noise. Drive, Sat and Tone are read live so the
// overdrive performs mid-hit.
//
// The noise is reseeded from the host seed on every hit, so the pipe is
// deterministic (RATPIPE_FLAG_DETERMINISTIC_WITH_SEED): same seed, same
// events, bit-identical output, and a dirtied instance equals a fresh one
// after FLUSH (validator V9/V11). ALL_OFF / FLUSH silence the hit at once.
#include <ratpipe/pipe_base.hpp>
#include <ratpipe/kit/kit.hpp>

namespace {

namespace kit = ratpipe::kit;

constexpr double kTwoPi = 6.283185307179586;

// Appending is safe; reordering or renaming an id orphans saved songs (M8).
enum
{
    kDecay = 0,    // amplitude decay: closed tick .. open hat               Perform 1
    kDrive,        // overdrive pre-gain                                      Perform 2
    kSat,          // saturation type (discrete)                              Perform 3
    kHpf,          // high-pass: where the body of the hat starts             Perform 4
    kPitch,        // ring carrier pitch
    kFm,           // FM ring amount
    kRatio,        // modulator ratio (discrete, inharmonic)
    kFmDec,        // FM ring decay time (sheds partials as it dies)
    kNoise,        // noise sizzle level
    kTone,         // post-drive low-pass
    kLevel,        // output gain 0..1.5, Mutate never touches it
    kParamCount
};

const char* const kSatNames[6]   = { "Tanh", "Tape", "Phat", "Fold", "Hard", "Diode" };
const char* const kRatioNames[7] = { "1.41x", "2.24x", "2.76x", "3.17x", "4.16x", "5.43x", "6.87x" };
const float       kRatios[7]     = { 1.41f, 2.24f, 2.76f, 3.17f, 4.16f, 5.43f, 6.87f };

class FuKitHhPipe : public ratpipe::Pipe<FuKitHhPipe, kParamCount>
{
public:
    static constexpr ratpipe::PipeInfo kInfo {
        "com.alecwhiting.fukit_hh", "FuKit_HH", "alecwhiting", "1.0.0",
        RATPIPE_DRUM, "fukh",
        { kDecay, kDrive, kSat, kHpf },
        RATPIPE_FLAG_DETERMINISTIC_WITH_SEED
    };

    // id, label (<= 8), group, default, then readout / mutate band / flags.
    static constexpr ratpipe::ParamSpec kParams[kParamCount] = {
        ratpipe::ParamSpec("fukh.decay", "Decay", "Env",   0.25f).ms(15.0f, 900.0f).mutate(0.05f, 0.85f),
        ratpipe::ParamSpec("fukh.drive", "Drive", "Drive", 0.35f).pct().mutate(0.00f, 0.90f),
        ratpipe::ParamSpec("fukh.sat",   "Sat",   "Drive", 0.00f).discrete(6, kSatNames),
        ratpipe::ParamSpec("fukh.hpf",   "HPF",   "Tone",  0.50f).hz(1000.0f, 12000.0f).mutate(0.15f, 0.90f),
        ratpipe::ParamSpec("fukh.pitch", "Pitch", "Tone",  0.40f).hz(200.0f, 2000.0f).mutate(0.10f, 0.80f),
        ratpipe::ParamSpec("fukh.fm",    "FM",    "Tone",  0.60f).pct().mutate(0.10f, 0.95f),
        ratpipe::ParamSpec("fukh.ratio", "Ratio", "Tone",  0.50f).discrete(7, kRatioNames),   // 3.17x
        ratpipe::ParamSpec("fukh.fmdec", "FmDec", "Env",   0.50f).ms(30.0f, 1500.0f).mutate(0.10f, 0.85f),
        ratpipe::ParamSpec("fukh.noise", "Noise", "Tone",  0.50f).pct().mutate(0.10f, 0.95f),
        ratpipe::ParamSpec("fukh.tone",  "Tone",  "Drive", 0.85f).hz(2000.0f, 18000.0f).mutate(0.30f, 1.00f),
        ratpipe::ParamSpec("fukh.level", "Level", "Out",   1.00f).gain().notMutable(),
    };

    explicit FuKitHhPipe(const ratpipe::CreateArgs& a) : Pipe(a) { onPrepare(a.sampleRate, a.maxBlock); }

    void onPrepare(double sr, int /*maxBlock*/)
    {
        sr_ = sr > 1000.0 ? sr : 44100.0;
        bodyHp_.prepare(sr_);
        bodyHp_.setBounded(true);                                 // tanh-bounded states: cannot ring away
        toneLp_.prepare(sr_);
        dc_.prepare(sr_, 10.0f);
        onAllOff();
    }

    void onTrigger(const ratpipe::TriggerEvent& e)
    {
        const float vel = e.velocity;
        const float semis = static_cast<float>(e.semitoneOffset);
        const float pitchHz = 200.0f + 1800.0f * param(kPitch);
        carHz_ = pitchHz * kit::semitonesToRatio(semis);                                 // the step's pitch offset
        ratio_ = kRatios[paramStep(kRatio)];
        const float d = param(kDecay);
        const float decayS = 0.015f + 0.885f * d * d;
        ampK_   = std::exp(-1.0f / (decayS * 0.3f * static_cast<float>(sr_)));           // amp_ is applied twice below: ~-60 dB after decayS
        const float fmS = 0.030f + 1.470f * param(kFmDec);
        fmEnvK_  = std::exp(-1.0f / (fmS * static_cast<float>(sr_)));
        fmIndex_ = 8.0f * param(kFm) * (0.5f + 0.5f * vel);                              // radians at t = 0: clangorous
        noiseAmp_ = 0.8f * param(kNoise) * (0.4f + 0.6f * vel);
        bodyHp_.set(kit::clamp(1000.0f * kit::semitonesToRatio(param(kHpf) * 43.0f + 0.5f * semis),
                               1000.0f, 14000.0f), 0.35f);         // 1 kHz .. 12 kHz, nudged by the step
        bodyHp_.reset();
        noise_.seed(hostSeed() ^ 0x66754b685548ull);                                     // same sizzle every hit: deterministic

        amp_   = 1.0f;
        fmEnv_ = 1.0f;
        vel_   = 0.35f + 0.65f * vel;
        carPhase_ = modPhase_ = 0.0;
    }

    void onAllOff()
    {
        amp_ = fmEnv_ = 0.0f;
        carPhase_ = modPhase_ = 0.0;
        bodyHp_.reset();
        toneLp_.reset();
        dc_.reset();
    }

    void render(float* L, float* R, int n)
    {
        if (amp_ <= 1.0e-6f) { amp_ = 0.0f; return; }           // silent: adding nothing is correct

        // Overdrive settings are read live so Drive / Sat / Tone perform mid-hit.
        const float drive = param(kDrive);
        const int   sat   = paramStep(kSat);
        const float pre   = 1.0f + 9.0f * drive;
        const float level = param(kLevel);

        // Per-type makeup so loudness stays roughly flat as Drive rises (same
        // stage as FuKit_BD); the hard output bound is the tanh ceiling below.
        const float tanhNorm = 1.0f / std::tanh(pre);
        const float phatTrim = 1.0f / (1.0f + 0.9f * drive);
        const float foldTrim = 1.0f - 0.20f * drive;
        const float hardTrim = 1.0f - 0.15f * drive;
        const float bias     = 0.1f + 0.5f * drive;              // Phat operating-point shift
        const float outNorm  = 1.0f / std::tanh(1.2f);           // output ceiling makeup

        toneLp_.set(2000.0f * kit::semitonesToRatio(param(kTone) * 38.0f), 0.0f);   // 2 kHz .. 18 kHz

        const double inc    = static_cast<double>(carHz_) / sr_;
        const double carInc = inc < 0.49 ? inc : 0.49;
        const double modInc = (inc * ratio_ < 0.49) ? inc * ratio_ : 0.49;

        for (int i = 0; i < n; ++i)
        {
            // Ring: 2-op FM at an inharmonic ratio; the index dies over FmDec,
            // so the cluster sheds partials as the hit decays.
            const float mod  = static_cast<float>(std::sin(kTwoPi * modPhase_));
            const float ring = static_cast<float>(std::sin(kTwoPi * carPhase_ + static_cast<double>(fmIndex_ * fmEnv_ * mod)));
            carPhase_ = wrap01(carPhase_ + carInc);
            modPhase_ = wrap01(modPhase_ + modInc);

            // Ring + seeded sizzle, high-passed: the body of the hat (bounded Svf).
            const float x = bodyHp_.processHp((0.6f * ring + noiseAmp_ * noise_.process()) * amp_);

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

            // DC block (Phat / Diode are asymmetric), then the Tone low-pass.
            y = toneLp_.processLp(dc_.process(y));

            // Envelope, velocity, then a tanh ceiling: the filters can
            // transiently overshoot the shaper's bound, so the output bound is
            // taken here. tanh < 1, outNorm ~= 1.2, 0.75 * level <= 1.125:
            // peak <= 1.35, under 2.0 (M3).
            const float out = std::tanh(y * amp_ * vel_ * 1.2f) * outNorm * 0.75f * level;
            L[i] = out;
            R[i] = out;

            amp_   *= ampK_;
            fmEnv_ *= fmEnvK_;
        }
    }

private:
    static double wrap01(double p) noexcept { return p - std::floor(p); }

    double sr_ = 48000.0;
    double carPhase_ = 0.0, modPhase_ = 0.0;
    float  carHz_ = 600.0f, ratio_ = 3.17f;
    float  amp_ = 0.0f, ampK_ = 0.999f;
    float  fmEnv_ = 0.0f, fmEnvK_ = 0.999f, fmIndex_ = 0.0f;
    float  noiseAmp_ = 0.0f;
    float  vel_ = 1.0f;
    kit::Svf        bodyHp_;
    kit::Svf        toneLp_;
    kit::DcBlock    dc_;
    kit::WhiteNoise noise_;
};

} // namespace

RATPIPE_DEFINE_ENTRY(FuKitHhPipe)
