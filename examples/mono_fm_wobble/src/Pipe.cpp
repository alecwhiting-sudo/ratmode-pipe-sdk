// FM Wobble — "a gritty FM bass with a wobble macro" (RATPIPE_MONO).
//
// The README's demo pipe. Signal path, all from the kit plus two sine phase
// accumulators of its own:
//
//   modulator sine (ratio x carrier) --> phase-modulates the carrier sine
//   + sub sine one octave down                  (Sub)
//   -> asymmetric tanh "grit" + DC block        (Grit)
//   -> LadderLite four-pole low-pass            (Cutoff, Res, Wobble, env)
//   -> tanh output stage, Level
//
// Wobble is an LFO on the filter cutoff, synced to the host transport: the
// phase is taken from the transport tick every block (so it lands on the bar
// no matter where playback started) and only free-runs at 120 bpm when the
// host gives no transport or is stopped. Rate picks the cycle length in note
// values (1/4, 1/8, 1/16, 1/8T); Wobble is the depth in octaves below Cutoff.
//
// 303-style phrasing: a note slid into (SLIDE_FROM_PREV) glides and does not
// retrigger the envelopes; ACCENT opens the filter further, adds FM index and
// level. Velocity scales level and the mod envelope.
//
// Everything that sounds is a declared parameter (contract M7): FLUSH/ALL_OFF
// put every oscillator, envelope and filter back to rest, and the pipe
// declares DETERMINISTIC_WITH_SEED so the validator compares two instances
// sample for sample (V9/V11).
#include <ratpipe/pipe_base.hpp>
#include <ratpipe/kit/kit.hpp>

namespace {

namespace kit = ratpipe::kit;

constexpr double kTwoPi = 6.283185307179586;

// Appending is safe; reordering or renaming an id orphans saved songs (M8).
enum
{
    kWobble = 0,   // LFO depth on the cutoff (octaves below Cutoff)       Perform 1
    kCutoff,       // filter cutoff at rest                                Perform 2
    kFm,           // FM index                                             Perform 3
    kGrit,         // asymmetric tanh drive before the filter              Perform 4
    kRate,         // wobble cycle: 1/4, 1/8, 1/16, 1/8T (discrete)
    kRatio,        // modulator : carrier ratio (discrete)
    kSub,          // sub sine mix
    kDecay,        // mod + amp envelope decay
    kRes,          // filter resonance
    kGlide,        // slide time
    kLevel,        // output gain 0..1.5, Mutate never touches it
    kParamCount
};

const char* const kRateNames[4]  = { "1/4", "1/8", "1/16", "1/8T" };
const double      kRateBeats[4]  = { 1.0, 0.5, 0.25, 1.0 / 3.0 };          // beats per wobble cycle
const char* const kRatioNames[5] = { "1/2", "1", "2", "3", "4" };
const float       kRatioValue[5] = { 0.5f, 1.0f, 2.0f, 3.0f, 4.0f };

class FmWobblePipe : public ratpipe::Pipe<FmWobblePipe, kParamCount>
{
public:
    static constexpr ratpipe::PipeInfo kInfo {
        "dev.ratmode.examples.fm_wobble", "FM Wobble", "RatMode SDK examples", "1.0.0",
        RATPIPE_MONO, "fmw",
        { kWobble, kCutoff, kFm, kGrit },
        RATPIPE_FLAG_WANTS_TRANSPORT | RATPIPE_FLAG_DETERMINISTIC_WITH_SEED
    };

    // id, label (<= 8), group, default, then readout / mutate band / flags.
    static constexpr ratpipe::ParamSpec kParams[kParamCount] = {
        ratpipe::ParamSpec("fmw.wobble", "Wobble", "Wobble", 0.55f).pct().mutate(0.0f, 0.9f),
        ratpipe::ParamSpec("fmw.cutoff", "Cutoff", "Filter", 0.60f).hz(60.0f, 8000.0f).mutate(0.25f, 0.9f),
        ratpipe::ParamSpec("fmw.fm",     "FM",     "Osc",    0.40f).pct().mutate(0.0f, 0.8f),
        ratpipe::ParamSpec("fmw.grit",   "Grit",   "Tone",   0.35f).pct().mutate(0.0f, 0.8f),
        ratpipe::ParamSpec("fmw.rate",   "Rate",   "Wobble", 1.0f / 3.0f).discrete(4, kRateNames),   // 1/8
        ratpipe::ParamSpec("fmw.ratio",  "Ratio",  "Osc",    0.50f).discrete(5, kRatioNames),       // 2
        ratpipe::ParamSpec("fmw.sub",    "Sub",    "Osc",    0.40f).pct(),
        ratpipe::ParamSpec("fmw.decay",  "Decay",  "Env",    0.45f).ms(40.0f, 2000.0f).mutate(0.15f, 0.85f),
        ratpipe::ParamSpec("fmw.res",    "Res",    "Filter", 0.35f).pct().mutate(0.0f, 0.8f),
        ratpipe::ParamSpec("fmw.glide",  "Glide",  "Tone",   0.30f).ms(5.0f, 300.0f).mutate(0.0f, 0.7f),
        ratpipe::ParamSpec("fmw.level",  "Level",  "Out",    1.00f).gain().notMutable(),
    };

    explicit FmWobblePipe(const ratpipe::CreateArgs& a) : Pipe(a) { onPrepare(a.sampleRate, a.maxBlock); }

    void onPrepare(double sr, int /*maxBlock*/)
    {
        sr_ = sr > 1000.0 ? sr : 44100.0;
        sub_.prepare(sr_);
        sub_.setOctavesDown(1);
        dc_.prepare(sr_, 10.0f);
        filt_.prepare(sr_);
        ampEnv_.prepare(sr_);
        modEnv_.prepare(sr_);
        glide_.prepare(sr_);
        onAllOff();
    }

    // ── events ──────────────────────────────────────────────────────────────

    void onNoteOn(const ratpipe::NoteEvent& e)
    {
        const float d = param(kDecay);
        const float decayMs = 40.0f + 1960.0f * d * d;
        ampEnv_.set(1.0f, decayMs, 0.55f, 25.0f);                 // holds at 55 % while the key is down
        modEnv_.set(1.0f, decayMs, 0.0f, 25.0f);                  // sweeps to nothing
        glide_.setTime(5.0f + 295.0f * param(kGlide));

        const bool legato = e.slide && gate_;                     // 303: slide glides, does not retrigger
        glide_.noteOn(static_cast<float>(e.note), legato);
        if (! legato)
        {
            ampEnv_.gateOn();
            modEnv_.gateOn();
            carPhase_ = modPhase_ = 0.0;                          // every hit starts the same (determinism)
            sub_.reset();
        }
        accent_ = e.accent ? 1.0f : 0.0f;
        vel_    = 0.5f + 0.5f * e.velocity;
        gate_   = true;
        note_   = e.note;
    }

    void onNoteOff(const ratpipe::NoteEvent& e)
    {
        if (e.slideSource || e.note != note_) return;             // the delayed off of a slide source: ignore
        gate_ = false;
        ampEnv_.gateOff();
        modEnv_.gateOff();
    }

    void onReleaseHeld() { gate_ = false; ampEnv_.gateOff(); modEnv_.gateOff(); }

    void onAllOff()
    {
        ampEnv_.kill();
        modEnv_.kill();
        filt_.reset();
        dc_.reset();
        sub_.reset();
        carPhase_ = modPhase_ = 0.0;
        freePhase_ = 0.0;
        gate_   = false;
        note_   = -1;
        accent_ = 0.0f;
    }

    // Once per host render call, before the block is split at events: derive
    // the wobble phase for sample 0 of the block from the transport.
    void onBlockStart(int /*n*/)
    {
        blockPos_ = 0;
        const int rate = paramStep(kRate);
        const double beatsPerCycle = kRateBeats[rate < 0 ? 0 : (rate > 3 ? 3 : rate)];
        const RatPipeTransport& t = transport();
        double bpm = 120.0;                                       // fallback: free-run as if at 120 bpm
        if (hasTransport() && t.playing != 0 && t.bpm > 20.0 && t.bpm < 1000.0 && t.ticks_per_beat > 0)
        {
            bpm = t.bpm;
            const double beats = static_cast<double>(t.tick) / static_cast<double>(t.ticks_per_beat);
            const double cyc = beats / beatsPerCycle;
            freePhase_ = cyc - std::floor(cyc);                   // lands on the bar wherever playback started
        }
        lfoInc_ = bpm / 60.0 / sr_ / beatsPerCycle;               // cycles per sample
        lfoPhase0_ = freePhase_;
    }

    // ── audio ───────────────────────────────────────────────────────────────

    void render(float* L, float* R, int n)
    {
        const int pos0 = blockPos_;
        blockPos_ += n;
        // Keep the free-running phase moving even while silent, so the wobble
        // does not restart on every note when the host has no transport.
        freePhase_ = wrap01(lfoPhase0_ + lfoInc_ * static_cast<double>(blockPos_));
        if (! ampEnv_.isActive()) return;                         // nothing to add

        const float wobble = param(kWobble);
        const float fmIdx  = 6.0f * param(kFm) * param(kFm);      // radians at full envelope (squared: fine control low down)
        const float grit   = param(kGrit);
        const float ratio  = kRatioValue[clampStep(paramStep(kRatio), 5)];
        const float subMix = param(kSub);
        const float res    = param(kRes) * (1.0f - 0.15f * accent_);   // accent: a touch less ring, more bite
        const float level  = param(kLevel);
        const float baseHz = 60.0f * kit::semitonesToRatio(param(kCutoff) * 84.6f);   // 60 Hz .. 8 kHz
        const float gritDrive = 1.0f + 9.0f * grit;
        const float gritBias  = 0.35f * grit;
        const float gritTrim  = 1.0f / (1.0f + 0.6f * grit);          // loudness roughly flat as Grit rises
        const float accGain   = 1.0f + 0.3f * accent_;
        const float outDrive  = 1.4f;
        const float outNorm   = 1.0f / std::tanh(outDrive);

        float cutHz = filt_.cutoff();
        for (int i = 0; i < n; ++i)
        {
            const float aEnv = ampEnv_.process();
            const float mEnv = modEnv_.process();
            const float hz   = glide_.tickHz();

            // Control rate (32 samples): wobble LFO + envelope -> cutoff, exp() lives here.
            if ((i & 31) == 0)
            {
                const double ph  = wrap01(lfoPhase0_ + lfoInc_ * static_cast<double>(pos0 + i));
                const float  tri = static_cast<float>(ph < 0.5 ? 1.0 - 4.0 * ph : 4.0 * ph - 3.0);   // +1 at the beat, -1 half way
                const float  lfo = 0.5f * (1.0f - tri);                                             // 0 on the beat .. 1 half way
                const float  octDown = 4.0f * wobble * lfo;                                        // up to 4 octaves below Cutoff
                const float  octUp   = (1.5f + 2.0f * accent_) * mEnv * vel_;                      // the envelope opens it
                cutHz = baseHz * kit::semitonesToRatio(12.0f * (octUp - octDown));
                filt_.set(cutHz, res, 1.0f);
            }

            // Two-operator FM (phase modulation): modulator -> carrier.
            const double carInc = static_cast<double>(hz) / sr_;
            const double modInc = carInc * static_cast<double>(ratio);
            const float  mod    = static_cast<float>(std::sin(kTwoPi * modPhase_));
            const float  index  = fmIdx * (0.25f + 0.75f * mEnv) * (1.0f + 0.5f * accent_) * vel_;
            const float  car    = static_cast<float>(std::sin(kTwoPi * carPhase_ + static_cast<double>(index * mod)));
            carPhase_ = wrap01(carPhase_ + (carInc < 0.49 ? carInc : 0.49));
            modPhase_ = wrap01(modPhase_ + (modInc < 0.49 ? modInc : 0.49));

            float x = 0.7f * car + 0.6f * subMix * sub_.process(hz);

            // Grit: asymmetric tanh (even harmonics), DC removed. Bounded < 2 by construction.
            x = dc_.process(kit::sat::asymTanh(x, gritDrive, gritBias)) * gritTrim;

            // Ladder: tanh-bounded loop, |y| <= 3.
            x = filt_.process(x);

            // Output: amp envelope, velocity, accent, soft tanh ceiling, Level.
            // tanh < 1, 0.6 * level <= 0.9: the peak stays under the 2.0 bound (M3).
            const float y = std::tanh(x * aEnv * vel_ * accGain * outDrive) * outNorm * 0.6f * level;
            L[i] = y;
            R[i] = y;
        }
    }

private:
    static double wrap01(double p) noexcept { return p - std::floor(p); }
    static int clampStep(int s, int count) noexcept { return s < 0 ? 0 : (s >= count ? count - 1 : s); }

    double sr_ = 48000.0;
    double carPhase_ = 0.0, modPhase_ = 0.0;
    double freePhase_ = 0.0, lfoPhase0_ = 0.0, lfoInc_ = 0.0;
    int    blockPos_ = 0;
    kit::SubOsc     sub_;
    kit::DcBlock    dc_;
    kit::LadderLite filt_;
    kit::Adsr       ampEnv_, modEnv_;
    kit::Glide      glide_;
    float accent_ = 0.0f, vel_ = 1.0f;
    bool  gate_   = false;
    int   note_   = -1;
};

} // namespace

RATPIPE_DEFINE_ENTRY(FmWobblePipe)
