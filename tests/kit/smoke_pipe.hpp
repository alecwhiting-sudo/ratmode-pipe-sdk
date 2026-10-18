// A small but real pipe written against pipe_base + the kit. It is what the
// tests load through the C ABI, what build.sh turns into a shared library to
// prove only `ratpipe_entry` is exported, and what the Mach-O cross-build probe
// feeds to RatMode's ExtMachOCheck. Mono PolyBLEP saw -> ladder -> tanh.
//
// It declares DETERMINISTIC_WITH_SEED: it has no noise source, and onAllOff
// (also run for FLUSH) returns every block — oscillator phase, glide, filter,
// envelope, DC blocker — to the constructed state, so after ALL_OFF + FLUSH a
// re-used instance is sample-identical to a fresh one (validator V9/V11).
#pragma once
#include "ratpipe/pipe_base.hpp"
#include "ratpipe/kit/kit.hpp"

enum { SP_CUT = 0, SP_RES, SP_DECAY, SP_DRIVE, SP_WAVE, SP_LEVEL, SP_COUNT };

static const char* const kSmokeWaves[2] = { "Saw", "Square" };

class SmokePipe : public ratpipe::Pipe<SmokePipe, SP_COUNT>
{
public:
    static constexpr ratpipe::PipeInfo kInfo {
        "dev.ratmode.sdk_smoke", "SDK Smoke", "RatMode SDK", "0.1.0", RATPIPE_MONO, "smk",
        { SP_CUT, SP_RES, SP_DECAY, SP_DRIVE }, RATPIPE_FLAG_DETERMINISTIC_WITH_SEED };

    static constexpr ratpipe::ParamSpec kParams[SP_COUNT] = {
        ratpipe::ParamSpec("smk.cut",   "Cut",   "Filter", 0.55f).hz(60.0f, 11000.0f).mutate(0.15f, 0.95f),
        ratpipe::ParamSpec("smk.res",   "Res",   "Filter", 0.35f).pct().mutate(0.0f, 0.85f),
        ratpipe::ParamSpec("smk.decay", "Decay", "Env",    0.40f).ms(20.0f, 2000.0f),
        ratpipe::ParamSpec("smk.drive", "Drive", "Tone",   0.30f).pct(),
        ratpipe::ParamSpec("smk.wave",  "Wave",  "Tone",   0.0f).discrete(2, kSmokeWaves),
        ratpipe::ParamSpec("smk.level", "Level", "Out",    1.0f).gain().notMutable(),
    };

    explicit SmokePipe(const ratpipe::CreateArgs& a) : Pipe(a) { onPrepare(a.sampleRate, a.maxBlock); }

    void onPrepare(double sr, int)
    {
        osc_.prepare(sr);
        filt_.prepare(sr);
        env_.prepare(sr);
        glide_.prepare(sr);
        glide_.setTime(60.0f);
        dc_.prepare(sr);
    }

    void onNoteOn(const ratpipe::NoteEvent& e)
    {
        const float decayMs = 20.0f + 1980.0f * param(SP_DECAY) * param(SP_DECAY);
        env_.set(1.0f, decayMs, 0.0f, 30.0f);
        glide_.noteOn(static_cast<float>(e.note), e.slide);
        if (!e.slide || !env_.isActive())
        {
            env_.gateOn();
            amp_ = e.velocity * (e.accent ? 1.3f : 1.0f);
        }
        held_ = e.note;
    }

    void onNoteOff(const ratpipe::NoteEvent& e)
    {
        if (e.note == held_) env_.gateOff();
    }

    void onReleaseHeld() { env_.gateOff(); }
    void onAllOff()
    {
        // Everything back to the constructed state (M7): phase and glide are
        // not parameters, so a song switch must not inherit them.
        env_.kill(); filt_.reset(); dc_.reset(); osc_.reset(); glide_.reset();
        amp_ = 0.0f; held_ = -1;
    }

    void render(float* L, float* R, int n)
    {
        if (!env_.isActive()) return;                      // additive: nothing to add
        osc_.setWave(paramStep(SP_WAVE) == 1 ? ratpipe::kit::Osc::Pulse : ratpipe::kit::Osc::Saw);
        const float cut   = param(SP_CUT);
        const float res   = param(SP_RES);
        const float drive = 1.0f + 7.0f * param(SP_DRIVE);
        const float level = param(SP_LEVEL);
        const float baseHz = 60.0f * ratpipe::kit::semitonesToRatio(cut * 90.0f);

        for (int i = 0; i < n; ++i)
        {
            const float a = env_.process();
            osc_.setFrequency(glide_.tickHz());
            if ((i & 31) == 0) filt_.set(baseHz * (1.0f + 2.0f * a), res, drive);
            const float s = dc_.process(filt_.process(osc_.process() * amp_ * a)) * 0.2f * level;   // ladder <= 3, dc block <= 2x, level <= 1.5 -> peak < 1.8
            L[i] = s;
            R[i] = s;
        }
    }

private:
    ratpipe::kit::Osc        osc_;
    ratpipe::kit::LadderLite filt_;
    ratpipe::kit::Adsr       env_;
    ratpipe::kit::Glide      glide_;
    ratpipe::kit::DcBlock    dc_;
    float amp_ = 0.0f;
    int   held_ = -1;
};
