// FuKit_BD — an FM kick with a built-in multi-mode overdrive (RATPIPE_DRUM).
//
// Generator:
//   body:  sine carrier, pitch-swept down from Punch octaves above Pitch,
//          phase-modulated by a second sine at a selectable Ratio of the body
//          (0.5x..5x) whose index dies over FmDec — the "knock" of the hit
//   click: a short seeded noise burst through a bounded band-pass (Click)
//
// Overdrive (the point of this pipe): Drive sets the pre-gain, Sat picks the
// shape. All six shapes are hard-bounded for any input (contract M4 — no
// polynomial clippers), and each is trimmed so loudness stays roughly flat as
// Drive rises:
//
//   Tanh   tanh                      smooth, symmetric, odd harmonics
//   Tape   kit::sat::softTape        softer knee, level-compensated
//   Phat   kit::sat::asymTanh        biased tanh, even harmonics, warm
//   Fold   sine wavefolder           sin(pre*x): tops fold back, FM-ish fizz
//   Hard   hard clip                 flat-top slam, harshest of the six
//   Diode  asymmetric tanh gains     hot positive / cool negative half, fuzzy
//
// A DC block follows the shaper (Phat and Diode are asymmetric and would
// otherwise leave an offset on the swept body), then Tone, a gentle post-drive
// low-pass, tames the fizz the dirtier shapes make. Amp envelope, velocity and
// Level finish the chain.
//
// Each TRIGGER freezes the hit from the parameters and the event: the step's
// semitone_offset transposes the body (and the click a little), velocity
// scales level, FM index and punch. Turning a knob mid-hit changes the next
// hit, not the one playing — except Drive, Sat and Tone, which are read live
// so the overdrive performs.
//
// The click noise is reseeded from the host seed on every hit, so the pipe is
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
    kPitch = 0,    // resting pitch 30..110 Hz                              Perform 1
    kDecay,        // amplitude decay                                        Perform 2
    kDrive,        // overdrive pre-gain                                     Perform 3
    kSat,          // saturation type (discrete)                             Perform 4
    kPunch,        // pitch-envelope depth (octaves above Pitch at t = 0)
    kSweep,        // pitch-envelope time
    kFm,           // FM knock amount
    kRatio,        // modulator ratio (discrete)
    kFmDec,        // FM knock decay time
    kClick,        // transient level
    kTone,         // post-drive low-pass
    kLevel,        // output gain 0..1.5, Mutate never touches it
    kParamCount
};

const char* const kSatNames[6]   = { "Tanh", "Tape", "Phat", "Fold", "Hard", "Diode" };
const char* const kRatioNames[7] = { "0.5x", "1x", "1.5x", "2x", "2.5x", "3.5x", "5x" };
const float       kRatios[7]     = { 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.5f, 5.0f };

class FuKitPipe : public ratpipe::Pipe<FuKitPipe, kParamCount>
{
public:
    static constexpr ratpipe::PipeInfo kInfo {
        "com.alecwhiting.fukit_bd", "FuKit_BD", "alecwhiting", "1.0.0",
        RATPIPE_DRUM, "fuk",
        { kPitch, kDecay, kDrive, kSat },
        RATPIPE_FLAG_DETERMINISTIC_WITH_SEED
    };

    // id, label (<= 8), group, default, then readout / mutate band / flags.
    static constexpr ratpipe::ParamSpec kParams[kParamCount] = {
        ratpipe::ParamSpec("fuk.pitch", "Pitch", "Tone",  0.30f).hz(30.0f, 110.0f).mutate(0.10f, 0.70f),
        ratpipe::ParamSpec("fuk.decay", "Decay", "Env",   0.45f).ms(60.0f, 1500.0f).mutate(0.15f, 0.85f),
        ratpipe::ParamSpec("fuk.drive", "Drive", "Drive", 0.35f).pct().mutate(0.00f, 0.90f),
        ratpipe::ParamSpec("fuk.sat",   "Sat",   "Drive", 0.00f).discrete(6, kSatNames),
        ratpipe::ParamSpec("fuk.punch", "Punch", "Env",   0.50f).pct().mutate(0.10f, 0.90f),
        ratpipe::ParamSpec("fuk.sweep", "Sweep", "Env",   0.40f).ms(8.0f, 160.0f).mutate(0.10f, 0.80f),
        ratpipe::ParamSpec("fuk.fm",    "FM",    "Tone",  0.35f).pct().mutate(0.00f, 0.80f),
        ratpipe::ParamSpec("fuk.ratio", "Ratio", "Tone",  0.6667f).discrete(7, kRatioNames),   // 2.5x
        ratpipe::ParamSpec("fuk.fmdec", "FmDec", "Env",   0.30f).ms(5.0f, 200.0f).mutate(0.05f, 0.80f),
        ratpipe::ParamSpec("fuk.click", "Click", "Tone",  0.40f).pct().mutate(0.00f, 0.90f),
        ratpipe::ParamSpec("fuk.tone",  "Tone",  "Drive", 0.80f).hz(200.0f, 18000.0f).mutate(0.25f, 1.00f),
        ratpipe::ParamSpec("fuk.level", "Level", "Out",   1.00f).gain().notMutable(),
    };

    explicit FuKitPipe(const ratpipe::CreateArgs& a) : Pipe(a) { onPrepare(a.sampleRate, a.maxBlock); }

    void onPrepare(double sr, int /*maxBlock*/)
    {
        sr_ = sr > 1000.0 ? sr : 44100.0;
        clickBp_.prepare(sr_);
        clickBp_.setBounded(true);                                // tanh-bounded states: cannot ring away
        toneLp_.prepare(sr_);
        dc_.prepare(sr_, 10.0f);                                  // well under the 30 Hz body: no low-end loss
        onAllOff();
    }

    void onTrigger(const ratpipe::TriggerEvent& e)
    {
        const float vel = e.velocity;
        const float semis = static_cast<float>(e.semitoneOffset);
        const float pitchHz = 30.0f + 80.0f * param(kPitch);
        baseHz_   = pitchHz * kit::semitonesToRatio(semis);                              // the step's pitch offset
        sweepOct_ = (0.5f + 3.0f * param(kPunch)) * (0.7f + 0.3f * vel);                 // octaves above rest at t = 0
        const float sweepS = 0.008f + 0.152f * param(kSweep);
        pitchK_ = std::exp(-1.0f / (sweepS * 0.35f * static_cast<float>(sr_)));          // reaches rest in ~sweepS
        const float d = param(kDecay);
        const float decayS = 0.06f + 1.44f * d * d;
        ampK_   = std::exp(-1.0f / (decayS * 0.3f * static_cast<float>(sr_)));           // amp_ is applied twice below: ~-60 dB after decayS
        ratio_  = kRatios[paramStep(kRatio)];
        const float fmS = 0.005f + 0.195f * param(kFmDec);
        fmEnvK_  = std::exp(-1.0f / (fmS * static_cast<float>(sr_)));
        fmIndex_ = 5.5f * param(kFm) * (0.5f + 0.5f * vel);                              // radians at t = 0
        clickAmp_ = 0.9f * param(kClick) * (0.4f + 0.6f * vel);
        clickK_   = std::exp(-1.0f / (0.006f * static_cast<float>(sr_)));                // 6 ms burst
        clickBp_.set(2500.0f * kit::semitonesToRatio(0.5f * semis), 0.55f);
        clickBp_.reset();
        noise_.seed(hostSeed() ^ 0x66754b6974ull);                                       // same burst every hit: deterministic

        amp_      = 1.0f;
        pitchEnv_ = 1.0f;
        fmEnv_    = 1.0f;
        clickEnv_ = 1.0f;
        vel_      = 0.35f + 0.65f * vel;
        carPhase_ = modPhase_ = 0.0;
    }

    void onAllOff()
    {
        amp_ = pitchEnv_ = fmEnv_ = clickEnv_ = 0.0f;
        carPhase_ = modPhase_ = 0.0;
        clickBp_.reset();
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

        // Per-type makeup so loudness stays roughly flat as Drive rises. Every
        // shape is bounded by 1 (Phat by 2, trimmed below 1.1): with amp, vel
        // and 0.8 * level <= 1.2 the peak stays under the 2.0 bound (M3).
        const float tanhNorm = 1.0f / std::tanh(pre);
        const float phatTrim = 1.0f / (1.0f + 0.9f * drive);
        const float foldTrim = 1.0f - 0.20f * drive;
        const float hardTrim = 1.0f - 0.15f * drive;
        const float bias     = 0.1f + 0.5f * drive;              // Phat operating-point shift
        const float outNorm  = 1.0f / std::tanh(1.2f);           // output ceiling makeup

        toneLp_.set(200.0f * kit::semitonesToRatio(param(kTone) * 77.9f), 0.0f);   // 200 Hz .. 18 kHz

        for (int i = 0; i < n; ++i)
        {
            // Body: pitch envelope (exp2 per sample keeps the sweep zipper-free), 2-op FM.
            const float hz     = baseHz_ * kit::semitonesToRatio(12.0f * sweepOct_ * pitchEnv_);
            const double inc   = static_cast<double>(hz) / sr_;
            const double carInc = inc < 0.49 ? inc : 0.49;
            const double modInc = (inc * ratio_ < 0.49) ? inc * ratio_ : 0.49;
            const float  mod   = static_cast<float>(std::sin(kTwoPi * modPhase_));
            const float  body  = static_cast<float>(std::sin(kTwoPi * carPhase_ + static_cast<double>(fmIndex_ * fmEnv_ * mod)));
            carPhase_ = wrap01(carPhase_ + carInc);
            modPhase_ = wrap01(modPhase_ + modInc);

            // Click: seeded noise burst through a bounded band-pass.
            const float click = clickBp_.processBp(noise_.process() * clickEnv_) * clickAmp_ * 0.25f;

            // Overdrive: pre-gain, one of six bounded shapes, per-type makeup.
            const float x = body * amp_ + click;
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

            // Envelope, velocity, then a tanh ceiling: the DC block and Tone
            // filter can transiently overshoot the shaper's bound, so the
            // output bound is taken here instead. tanh < 1, outNorm ~= 1.2,
            // 0.75 * level <= 1.125: peak <= 1.35, under 2.0 (M3).
            const float out = std::tanh(y * amp_ * vel_ * 1.2f) * outNorm * 0.75f * level;
            L[i] = out;
            R[i] = out;

            amp_      *= ampK_;
            pitchEnv_ *= pitchK_;
            fmEnv_    *= fmEnvK_;
            clickEnv_ *= clickK_;
        }
    }

private:
    static double wrap01(double p) noexcept { return p - std::floor(p); }

    double sr_ = 48000.0;
    double carPhase_ = 0.0, modPhase_ = 0.0;
    float  baseHz_ = 50.0f, sweepOct_ = 1.0f, ratio_ = 2.5f;
    float  amp_ = 0.0f, ampK_ = 0.9995f;
    float  pitchEnv_ = 0.0f, pitchK_ = 0.998f;
    float  fmEnv_ = 0.0f, fmEnvK_ = 0.999f, fmIndex_ = 0.0f;
    float  clickEnv_ = 0.0f, clickK_ = 0.99f, clickAmp_ = 0.0f;
    float  vel_ = 1.0f;
    kit::Svf        clickBp_;
    kit::Svf        toneLp_;
    kit::DcBlock    dc_;
    kit::WhiteNoise noise_;
};

} // namespace

RATPIPE_DEFINE_ENTRY(FuKitPipe)
