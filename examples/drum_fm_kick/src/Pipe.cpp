// FM Kick — an FM kick drum (RATPIPE_DRUM).
//
//   body:  sine carrier, pitch-swept down from Punch octaves above Pitch,
//          phase-modulated by a second sine (FM) whose index dies in ~40 ms —
//          the "knock" at the front of the hit
//   click: a short seeded noise burst through a bounded band-pass (Click)
//   -> tanh drive (Drive), amplitude envelope (Decay), velocity, Level
//
// Each TRIGGER freezes everything about the hit from the parameters and the
// event: the step's semitone_offset transposes the body (and the click a
// little), velocity scales level, FM index and punch, so soft hits are
// rounder and loud ones snap. Turning a knob mid-hit changes the next hit,
// not the one playing.
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
    kPitch = 0,    // resting pitch 35..120 Hz                               Perform 1
    kDecay,        // amplitude decay                                         Perform 2
    kPunch,        // pitch-envelope depth (octaves above Pitch at t = 0)     Perform 3
    kDrive,        // tanh drive                                              Perform 4
    kFm,           // FM knock amount
    kClick,        // transient level
    kSweep,        // pitch-envelope time
    kLevel,        // output gain 0..1.5, Mutate never touches it
    kParamCount
};

class FmKickPipe : public ratpipe::Pipe<FmKickPipe, kParamCount>
{
public:
    static constexpr ratpipe::PipeInfo kInfo {
        "dev.ratmode.examples.fm_kick", "FM Kick", "RatMode SDK examples", "1.0.0",
        RATPIPE_DRUM, "fmk",
        { kPitch, kDecay, kPunch, kDrive },
        RATPIPE_FLAG_DETERMINISTIC_WITH_SEED
    };

    // id, label (<= 8), group, default, then readout / mutate band / flags.
    static constexpr ratpipe::ParamSpec kParams[kParamCount] = {
        ratpipe::ParamSpec("fmk.pitch", "Pitch", "Tone", 0.30f).hz(35.0f, 120.0f).mutate(0.10f, 0.70f),
        ratpipe::ParamSpec("fmk.decay", "Decay", "Env",  0.45f).ms(60.0f, 1500.0f).mutate(0.15f, 0.85f),
        ratpipe::ParamSpec("fmk.punch", "Punch", "Env",  0.50f).pct().mutate(0.10f, 0.90f),
        ratpipe::ParamSpec("fmk.drive", "Drive", "Tone", 0.30f).pct().mutate(0.00f, 0.85f),
        ratpipe::ParamSpec("fmk.fm",    "FM",    "Tone", 0.35f).pct().mutate(0.00f, 0.80f),
        ratpipe::ParamSpec("fmk.click", "Click", "Tone", 0.40f).pct().mutate(0.00f, 0.90f),
        ratpipe::ParamSpec("fmk.sweep", "Sweep", "Env",  0.40f).ms(10.0f, 150.0f).mutate(0.10f, 0.80f),
        ratpipe::ParamSpec("fmk.level", "Level", "Out",  1.00f).gain().notMutable(),
    };

    explicit FmKickPipe(const ratpipe::CreateArgs& a) : Pipe(a) { onPrepare(a.sampleRate, a.maxBlock); }

    void onPrepare(double sr, int /*maxBlock*/)
    {
        sr_ = sr > 1000.0 ? sr : 44100.0;
        clickBp_.prepare(sr_);
        clickBp_.setBounded(true);                                // tanh-bounded states: cannot ring away
        fmEnvK_ = std::exp(-1.0f / (0.040f * static_cast<float>(sr_)));   // FM knock: 40 ms
        onAllOff();
    }

    void onTrigger(const ratpipe::TriggerEvent& e)
    {
        const float vel = e.velocity;
        const float semis = static_cast<float>(e.semitoneOffset);
        const float pitchHz = 35.0f + 85.0f * param(kPitch);
        baseHz_   = pitchHz * kit::semitonesToRatio(semis);                              // the step's pitch offset
        sweepOct_ = (0.5f + 3.0f * param(kPunch)) * (0.7f + 0.3f * vel);                 // octaves above rest at t = 0
        const float sweepS = 0.010f + 0.140f * param(kSweep);
        pitchK_ = std::exp(-1.0f / (sweepS * 0.35f * static_cast<float>(sr_)));          // reaches rest in ~sweepS
        const float d = param(kDecay);
        const float decayS = 0.06f + 1.44f * d * d;
        ampK_   = std::exp(-1.0f / (decayS * 0.3f * static_cast<float>(sr_)));           // amp_ is applied twice below: ~-60 dB after decayS
        fmIndex_ = 5.0f * param(kFm) * (0.5f + 0.5f * vel);                              // radians at t = 0
        clickAmp_ = 0.9f * param(kClick) * (0.4f + 0.6f * vel);
        clickK_   = std::exp(-1.0f / (0.006f * static_cast<float>(sr_)));                // 6 ms burst
        clickBp_.set(2500.0f * kit::semitonesToRatio(0.5f * semis), 0.55f);
        clickBp_.reset();
        noise_.seed(hostSeed() ^ 0x6b69636bull);                                         // same burst every hit: deterministic

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
    }

    void render(float* L, float* R, int n)
    {
        if (amp_ <= 1.0e-6f) { amp_ = 0.0f; return; }           // silent: adding nothing is correct

        const float drive = 1.0f + 7.0f * param(kDrive);
        const float norm  = 1.0f / std::tanh(drive);              // loudness roughly flat as Drive rises
        const float level = param(kLevel);

        for (int i = 0; i < n; ++i)
        {
            // Body: pitch envelope (exp2 per sample keeps the sweep zipper-free), 2-op FM.
            const float hz     = baseHz_ * kit::semitonesToRatio(12.0f * sweepOct_ * pitchEnv_);
            const double inc   = static_cast<double>(hz) / sr_;
            const double carInc = inc < 0.49 ? inc : 0.49;
            const double modInc = (inc * 2.5 < 0.49) ? inc * 2.5 : 0.49;   // modulator 2.5x the body: metallic knock
            const float  mod   = static_cast<float>(std::sin(kTwoPi * modPhase_));
            const float  body  = static_cast<float>(std::sin(kTwoPi * carPhase_ + static_cast<double>(fmIndex_ * fmEnv_ * mod)));
            carPhase_ = wrap01(carPhase_ + carInc);
            modPhase_ = wrap01(modPhase_ + modInc);

            // Click: seeded noise burst through a bounded band-pass (|bp| <= 8 for any input).
            const float click = clickBp_.processBp(noise_.process() * clickEnv_) * clickAmp_ * 0.25f;

            // Drive, envelope, velocity. tanh < 1, 0.9 * level <= 1.35: under the 2.0 bound (M3).
            const float y = std::tanh((body * amp_ + click) * drive) * norm * amp_ * vel_ * 0.9f * level;
            L[i] = y;
            R[i] = y;

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
    float  baseHz_ = 50.0f, sweepOct_ = 1.0f;
    float  amp_ = 0.0f, ampK_ = 0.9995f;
    float  pitchEnv_ = 0.0f, pitchK_ = 0.998f;
    float  fmEnv_ = 0.0f, fmEnvK_ = 0.999f, fmIndex_ = 0.0f;
    float  clickEnv_ = 0.0f, clickK_ = 0.99f, clickAmp_ = 0.0f;
    float  vel_ = 1.0f;
    kit::Svf        clickBp_;
    kit::WhiteNoise noise_;
};

} // namespace

RATPIPE_DEFINE_ENTRY(FmKickPipe)
