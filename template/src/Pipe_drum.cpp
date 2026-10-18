// @DISPLAY@ — drum pipe (starter).
//
// A sine kick: a decaying amplitude envelope, a pitch envelope that sweeps
// down from "punch" octaves above the resting pitch, and tanh drive. It is
// meant to be copied and changed — every number below is yours.
//
// How a pipe is put together (include/ratpipe/pipe_base.hpp does the ABI work):
//
//   1. an enum naming the parameters (their ORDER is the index RatMode stores);
//   2. kInfo    — identity: id, display name, author, version, part kind,
//                 macro prefix, the four Perform knobs, flags;
//   3. kParams  — one ParamSpec per parameter: id "<prefix>.<path>", label
//                 (<= 8 chars), group, default, then readout / range / mutate
//                 band / flags via the chained setters;
//   4. hooks    — a drum pipe gets onTrigger (velocity, semitone_offset, step)
//                 plus onAllOff; they run between the samples they belong to;
//   5. render(L, R, n) — writes up to 256 samples into zeroed scratch buffers;
//                 the base adds them into the host's buffers, replaces NaN/Inf
//                 and clamps to +/- 2.0 (contract M3, M10);
//   6. RATPIPE_DEFINE_ENTRY — the one exported symbol.
//
// Parameters arrive normalised 0..1 (Level 0..1.5) and are already atomics:
// read them with param(i) and map to engineering units yourself.
//
// The rules that must hold (docs/CONTRACT.md) — `ratpipe validate` checks them:
//   * render() never allocates, locks or does IO; size everything in the
//     constructor / onPrepare (M1).
//   * every sound-affecting value is a declared parameter; ALL_OFF / FLUSH
//     return the voice to silence so two instances end up identical (M7).
//   * only tanh-family saturation, never a cubic soft-clipper (M4).
#include <ratpipe/pipe_base.hpp>
#include <ratpipe/kit/kit.hpp>

namespace {

// Parameter indices. Appending is safe; reordering or renaming an id orphans
// the values saved in songs (contract M8).
enum { kPitch = 0, kDecay, kPunch, kDrive, kLevel, kParamCount };

class @NAME@Pipe : public ratpipe::Pipe<@NAME@Pipe, kParamCount>
{
public:
    static constexpr ratpipe::PipeInfo kInfo {
        "@ID@",                      // id: lowercase reverse-DNS, stored by songs — never change it
        "@DISPLAY@",                 // display name, <= 24 characters
        "@AUTHOR@",
        "0.1.0",                     // your version (semver)
        RATPIPE_DRUM,                // drum lane: TRIGGER events
        "@PREFIX@",                  // macro prefix: every parameter id is "@PREFIX@.<path>"
        { kPitch, kDecay, kPunch, kDrive },    // the four Perform knobs (parameter indices)
        RATPIPE_FLAG_DETERMINISTIC_WITH_SEED   // no noise: same input -> same output (the validator checks it)
    };

    // id, label (<= 8 chars), group, default — then readout / mutate band / flags.
    static constexpr ratpipe::ParamSpec kParams[kParamCount] = {
        ratpipe::ParamSpec("@PREFIX@.pitch", "Pitch", "Tone", 0.35f).hz(35.0f, 160.0f).mutate(0.10f, 0.80f),
        ratpipe::ParamSpec("@PREFIX@.decay", "Decay", "Env",  0.45f).ms(40.0f, 1200.0f).mutate(0.10f, 0.90f),
        ratpipe::ParamSpec("@PREFIX@.punch", "Punch", "Env",  0.50f).pct(),
        ratpipe::ParamSpec("@PREFIX@.drive", "Drive", "Tone", 0.25f).pct().mutate(0.00f, 0.90f),
        ratpipe::ParamSpec("@PREFIX@.level", "Level", "Out",  1.00f).gain().notMutable(),   // 0..1.5; Mutate leaves it alone
    };

    explicit @NAME@Pipe(const ratpipe::CreateArgs& a) : Pipe(a) { onPrepare(a.sampleRate, a.maxBlock); }

    void onPrepare(double sr, int /*maxBlock*/)
    {
        sr_ = sr;
        osc_.prepare(sr);
        osc_.setWave(ratpipe::kit::Osc::Sine);
        onAllOff();
    }

    // One hit. Everything about the hit is frozen here from the parameters
    // and the event, so turning a knob mid-hit does not bend the sound.
    void onTrigger(const ratpipe::TriggerEvent& e)
    {
        const float pitchHz = 35.0f + 125.0f * param(kPitch);
        baseHz_   = pitchHz * ratpipe::kit::semitonesToRatio(static_cast<float>(e.semitoneOffset));   // the step's pitch offset
        sweepOct_ = 3.0f * param(kPunch);                                                             // octaves above rest at t = 0
        const float decayS = 0.04f + 1.16f * param(kDecay) * param(kDecay);
        ampK_   = std::exp(-1.0f / (decayS * 0.3f * static_cast<float>(sr_)));      // amp_ is applied twice in render: ~-60 dB after decayS
        pitchK_ = std::exp(-1.0f / (0.03f * static_cast<float>(sr_)));              // 30 ms pitch sweep
        amp_      = 1.0f;
        pitchEnv_ = 1.0f;
        velocity_ = e.velocity;
        osc_.reset();
    }

    // ALL_OFF and FLUSH: back to silence, no tail left for the next song.
    void onAllOff()
    {
        amp_      = 0.0f;
        pitchEnv_ = 0.0f;
        osc_.reset();
    }

    // n <= 256; L and R arrive zeroed. Write every sample you want heard.
    void render(float* L, float* R, int n)
    {
        if (amp_ <= 1.0e-6f) { amp_ = 0.0f; return; }   // silent: adding nothing is correct

        const float drive = 1.0f + 6.0f * param(kDrive);
        const float norm  = 1.0f / std::tanh(drive);     // keeps loudness steady as Drive moves
        const float level = param(kLevel);

        for (int i = 0; i < n; ++i)
        {
            // Pitch at a 32-sample control rate: exp2() is not free.
            if ((i & 31) == 0)
                osc_.setFrequency(baseHz_ * ratpipe::kit::semitonesToRatio(12.0f * sweepOct_ * pitchEnv_));

            const float s = osc_.process() * amp_;
            // tanh < 1, level <= 1.5: the peak stays well under the 2.0 bound
            const float y = std::tanh(s * drive) * norm * amp_ * velocity_ * 0.9f * level;
            L[i] = y;
            R[i] = y;

            amp_      *= ampK_;
            pitchEnv_ *= pitchK_;
        }
    }

private:
    ratpipe::kit::Osc osc_;
    double sr_        = 48000.0;
    float  baseHz_    = 50.0f;
    float  sweepOct_  = 1.0f;
    float  amp_       = 0.0f, ampK_   = 0.9995f;
    float  pitchEnv_  = 0.0f, pitchK_ = 0.998f;
    float  velocity_  = 1.0f;
};

} // namespace

RATPIPE_DEFINE_ENTRY(@NAME@Pipe)
