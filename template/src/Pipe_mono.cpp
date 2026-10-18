// @DISPLAY@ — melodic mono pipe (starter).
//
// This is the smallest useful melodic pipe: a PolyBLEP saw into a four-pole
// ladder low-pass, a decaying envelope that also opens the filter, tanh drive,
// 303-style slide (glide, no retrigger) and accent. It is meant to be copied
// and changed — every number below is yours.
//
// How a pipe is put together (include/ratpipe/pipe_base.hpp does the ABI work):
//
//   1. an enum naming the parameters (their ORDER is the index RatMode stores);
//   2. kInfo    — identity: id, display name, author, version, part kind,
//                 macro prefix, the four Perform knobs, flags;
//   3. kParams  — one ParamSpec per parameter: id "<prefix>.<path>", label
//                 (<= 8 chars), group, default, then readout / range / mutate
//                 band / flags via the chained setters;
//   4. hooks    — onPrepare / onNoteOn / onNoteOff / onReleaseHeld / onAllOff
//                 run between the samples they belong to (sample-accurate);
//   5. render(L, R, n) — writes up to 256 samples into zeroed scratch buffers;
//                 the base adds them into the host's buffers, replaces NaN/Inf
//                 and clamps to +/- 2.0 (contract M3, M10);
//   6. RATPIPE_DEFINE_ENTRY — the one exported symbol.
//
// Parameters arrive normalised 0..1 (Level 0..1.5) and are already atomics:
// read them with param(i) in render() and map to engineering units yourself.
//
// The rules that must hold (docs/CONTRACT.md) — `ratpipe validate` checks them:
//   * render() never allocates, locks or does IO; size everything in the
//     constructor / onPrepare (M1).
//   * every sound-affecting value is a declared parameter; ALL_OFF / FLUSH
//     return the voice to silence so two instances end up identical (M7).
//   * only tanh-family saturation (kit::sat, kit::LadderLite), never a cubic
//     soft-clipper (M4); band-limited oscillators (kit::Osc, M5).
#include <ratpipe/pipe_base.hpp>
#include <ratpipe/kit/kit.hpp>

namespace {

// Parameter indices. Appending is safe; reordering or renaming an id orphans
// the values saved in songs (contract M8).
enum { kCutoff = 0, kDecay, kDrive, kGlide, kLevel, kParamCount };

class @NAME@Pipe : public ratpipe::Pipe<@NAME@Pipe, kParamCount>
{
public:
    static constexpr ratpipe::PipeInfo kInfo {
        "@ID@",                      // id: lowercase reverse-DNS, stored by songs — never change it
        "@DISPLAY@",                 // display name, <= 24 characters
        "@AUTHOR@",
        "0.1.0",                     // your version (semver)
        RATPIPE_MONO,                // melodic: NOTE_ON / NOTE_OFF, one voice
        "@PREFIX@",                  // macro prefix: every parameter id is "@PREFIX@.<path>"
        { kCutoff, kDecay, kDrive, kGlide },   // the four Perform knobs (parameter indices)
        RATPIPE_FLAG_DETERMINISTIC_WITH_SEED   // no noise: same input -> same output (the validator checks it)
    };

    // id, label (<= 8 chars), group, default — then readout / mutate band / flags.
    // The readout is only how RatMode prints the value; the mapping to sound
    // happens in render(). The mutate band is the range Mutate may wander in.
    static constexpr ratpipe::ParamSpec kParams[kParamCount] = {
        ratpipe::ParamSpec("@PREFIX@.cutoff", "Cut",   "Filter", 0.55f).hz(60.0f, 11000.0f).mutate(0.15f, 0.95f),
        ratpipe::ParamSpec("@PREFIX@.decay",  "Decay", "Env",    0.40f).ms(30.0f, 1500.0f).mutate(0.10f, 0.90f),
        ratpipe::ParamSpec("@PREFIX@.drive",  "Drive", "Tone",   0.30f).pct().mutate(0.00f, 0.90f),
        ratpipe::ParamSpec("@PREFIX@.glide",  "Glide", "Tone",   0.35f).ms(5.0f, 250.0f).mutate(0.00f, 0.80f),
        ratpipe::ParamSpec("@PREFIX@.level",  "Level", "Out",    1.00f).gain().notMutable(),   // 0..1.5; Mutate leaves it alone
    };

    explicit @NAME@Pipe(const ratpipe::CreateArgs& a) : Pipe(a) { onPrepare(a.sampleRate, a.maxBlock); }

    // Sample-rate (re)configuration. Also the place to size anything that
    // depends on maxBlock — this pipe needs nothing beyond the kit objects.
    void onPrepare(double sr, int /*maxBlock*/)
    {
        osc_.prepare(sr);
        osc_.setWave(ratpipe::kit::Osc::Saw);
        filt_.prepare(sr);
        env_.prepare(sr);
        glide_.prepare(sr);
        onAllOff();
    }

    void onNoteOn(const ratpipe::NoteEvent& e)
    {
        // Decay 30 ms .. 1.5 s, squared so the lower half of the knob is usable.
        const float decayMs = 30.0f + 1470.0f * param(kDecay) * param(kDecay);
        env_.set(1.0f, decayMs, 0.0f, 12.0f);            // attack ms, decay ms, sustain 0..1, release ms
        glide_.setTime(5.0f + 245.0f * param(kGlide));

        // 303 semantics: a note slid into glides from the previous pitch and
        // does NOT restart the envelope; anything else is a fresh hit.
        const bool legato = e.slide && gate_;
        glide_.noteOn(static_cast<float>(e.note), legato);
        if (! legato)
        {
            env_.gateOn();
            osc_.reset();
        }
        accent_ = e.accent ? 1.0f : 0.0f;
        amp_    = 0.6f + 0.4f * e.velocity;
        gate_   = true;
        note_   = e.note;
    }

    void onNoteOff(const ratpipe::NoteEvent& e)
    {
        // The delayed off of a slide source belongs to the note we already left.
        if (! e.slideSource && e.note == note_) { gate_ = false; env_.gateOff(); }
    }

    void onReleaseHeld() { gate_ = false; env_.gateOff(); }

    // ALL_OFF and FLUSH (FLUSH calls onAllOff unless you define onFlush):
    // everything that could carry sound into the next song goes back to rest.
    void onAllOff()
    {
        env_.kill();
        filt_.reset();
        osc_.reset();
        gate_   = false;
        note_   = -1;
        accent_ = 0.0f;
    }

    // n <= 256; L and R arrive zeroed. Write every sample you want heard.
    void render(float* L, float* R, int n)
    {
        if (! env_.isActive()) return;                   // silent: adding nothing is correct

        const float cutoff = param(kCutoff);
        const float drive  = 1.0f + 7.0f * param(kDrive);
        const float level  = param(kLevel);
        const float norm   = 1.0f / std::tanh(drive);    // keeps loudness steady as Drive moves
        const float baseHz = 60.0f * ratpipe::kit::semitonesToRatio(cutoff * 90.0f);   // 60 Hz .. 11 kHz

        for (int i = 0; i < n; ++i)
        {
            const float a = env_.process();              // 0..1
            osc_.setFrequency(glide_.tickHz());

            // Coefficients at a 32-sample control rate: exp() is not free.
            if ((i & 31) == 0)
                filt_.set(baseHz * (1.0f + (2.0f + 3.0f * accent_) * a), 0.25f, 1.0f);

            const float x = filt_.process(osc_.process()) * a * amp_ * (1.0f + 0.3f * accent_);
            // ladder <= 3, tanh < 1, level <= 1.5: the peak stays well under the 2.0 bound
            const float y = std::tanh(x * drive) * norm * 0.5f * level;
            L[i] = y;
            R[i] = y;
        }
    }

private:
    ratpipe::kit::Osc        osc_;
    ratpipe::kit::LadderLite filt_;
    ratpipe::kit::Adsr       env_;
    ratpipe::kit::Glide      glide_;
    float amp_    = 0.0f;
    float accent_ = 0.0f;
    bool  gate_   = false;
    int   note_   = -1;
};

} // namespace

RATPIPE_DEFINE_ENTRY(@NAME@Pipe)
