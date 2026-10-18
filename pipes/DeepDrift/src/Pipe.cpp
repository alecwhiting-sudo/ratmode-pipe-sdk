// Deep Drift — a deep, unstable sub bass (RATPIPE_MONO).
//
// Signal path, all from the kit plus two sine phase accumulators of its own:
//
//   sine fundamental + sub sine one octave down     (Sub)
//   + PolyBLEP saw layer                            (Growl)
//   -> asymmetric tanh "phat" drive + DC block      (Drive)
//   -> LadderLite four-pole low-pass                (Cutoff, Res)
//   -> amp envelope, velocity, accent               (Decay)
//   -> feedback delay with a variable high-pass     (Echo, Time, Feedb, EchoHP)
//      in the loop, so every repeat gets thinner
//   -> tanh output ceiling, Level
//
// Instability is a bank of four seeded LFOs at mutually detuned rates, all
// scaled by one Unstab knob and a master Rate:
//
//   random-smooth  -> pitch wander (up to ~0.6 st)
//   slow sine      -> pitch "wow"  (tape-style, up to ~0.25 st)
//   random-smooth  -> filter cutoff wobble (up to ~1.2 oct)
//   triangle       -> amp tremble
//
// The echo Time is synced to the host transport (1/16, 1/8T, 1/8, 3/16, 1/4;
// 120 bpm when the host gives none); the delay time is smoothed, so moving
// Time bends the pitch of the repeats like a tape echo. The high-pass sits
// inside the feedback path: every pass through the loop loses more low end,
// which keeps long feedback tails from turning to mud under the dry sub.
//
// 303-style phrasing: a note slid into (SLIDE_FROM_PREV) glides and does not
// retrigger the envelope; ACCENT opens the filter and adds drive and level.
//
// Everything that sounds is a declared parameter (contract M7): ALL_OFF puts
// the voice and the LFO bank back to their seeded rest state, FLUSH also
// clears the delay line, and the pipe declares DETERMINISTIC_WITH_SEED so the
// validator compares two instances sample for sample (V9/V11).
#include <ratpipe/pipe_base.hpp>
#include <ratpipe/kit/kit.hpp>

namespace {

namespace kit = ratpipe::kit;

constexpr double kTwoPi = 6.283185307179586;

// Appending is safe; reordering or renaming an id orphans saved songs (M8).
enum
{
    kUnstab = 0,   // global instability depth (all four LFOs)          Perform 1
    kDrive,        // asymmetric tanh drive before the filter           Perform 2
    kEcho,         // echo level                                        Perform 3
    kEchoHp,       // high-pass frequency inside the echo loop          Perform 4
    kRate,         // master LFO rate
    kTime,         // echo time: 1/16, 1/8T, 1/8, 3/16, 1/4 (discrete)
    kFeedb,        // echo feedback
    kSub,          // sub sine (one octave down) mix
    kGrowl,        // PolyBLEP saw layer mix
    kCutoff,       // filter cutoff at rest
    kRes,          // filter resonance
    kDecay,        // amp envelope decay
    kGlide,        // slide time
    kLevel,        // output gain 0..1.5, Mutate never touches it
    kParamCount
};

const char* const kTimeNames[5] = { "1/16", "1/8T", "1/8", "3/16", "1/4" };
const double      kTimeBeats[5] = { 0.25, 1.0 / 3.0, 0.5, 0.75, 1.0 };

class DeepDriftPipe : public ratpipe::Pipe<DeepDriftPipe, kParamCount>
{
public:
    static constexpr ratpipe::PipeInfo kInfo {
        "com.alecwhiting.deepdrift", "Deep Drift", "alecwhiting", "1.0.0",
        RATPIPE_MONO, "ddr",
        { kUnstab, kDrive, kEcho, kEchoHp },
        RATPIPE_FLAG_WANTS_TRANSPORT | RATPIPE_FLAG_HAS_TAILS | RATPIPE_FLAG_DETERMINISTIC_WITH_SEED
    };

    // id, label (<= 8), group, default, then readout / mutate band / flags.
    static constexpr ratpipe::ParamSpec kParams[kParamCount] = {
        ratpipe::ParamSpec("ddr.unstab", "Unstab", "Drift",  0.45f).pct().mutate(0.0f, 0.95f),
        ratpipe::ParamSpec("ddr.drive",  "Drive",  "Tone",   0.50f).pct().mutate(0.1f, 0.9f),
        ratpipe::ParamSpec("ddr.echo",   "Echo",   "Echo",   0.35f).pct().mutate(0.0f, 0.85f),
        ratpipe::ParamSpec("ddr.echohp", "EchoHP", "Echo",   0.40f).hz(30.0f, 1500.0f).mutate(0.1f, 0.9f),
        ratpipe::ParamSpec("ddr.rate",   "Rate",   "Drift",  0.40f).hz(0.1f, 8.0f).mutate(0.1f, 0.8f),
        ratpipe::ParamSpec("ddr.time",   "Time",   "Echo",   0.50f).discrete(5, kTimeNames),       // 1/8
        ratpipe::ParamSpec("ddr.feedb",  "Feedb",  "Echo",   0.45f).pct().mutate(0.0f, 0.8f),
        ratpipe::ParamSpec("ddr.sub",    "Sub",    "Osc",    0.60f).pct().mutate(0.2f, 1.0f),
        ratpipe::ParamSpec("ddr.growl",  "Growl",  "Osc",    0.30f).pct().mutate(0.0f, 0.8f),
        ratpipe::ParamSpec("ddr.cutoff", "Cutoff", "Filter", 0.55f).hz(40.0f, 6000.0f).mutate(0.2f, 0.9f),
        ratpipe::ParamSpec("ddr.res",    "Res",    "Filter", 0.25f).pct().mutate(0.0f, 0.7f),
        ratpipe::ParamSpec("ddr.decay",  "Decay",  "Env",    0.50f).ms(60.0f, 2500.0f).mutate(0.2f, 0.9f),
        ratpipe::ParamSpec("ddr.glide",  "Glide",  "Tone",   0.25f).ms(5.0f, 300.0f).mutate(0.0f, 0.7f),
        ratpipe::ParamSpec("ddr.level",  "Level",  "Out",    1.00f).gain().notMutable(),
    };

    explicit DeepDriftPipe(const ratpipe::CreateArgs& a) : Pipe(a) { onPrepare(a.sampleRate, a.maxBlock); }

    void onPrepare(double sr, int /*maxBlock*/)
    {
        sr_ = sr > 1000.0 ? sr : 44100.0;
        sub_.prepare(sr_);
        sub_.setOctavesDown(1);
        saw_.prepare(sr_);
        saw_.setWave(kit::Osc::Saw);
        dc_.prepare(sr_, 10.0f);
        filt_.prepare(sr_);
        ampEnv_.prepare(sr_);
        glide_.prepare(sr_);
        for (kit::Lfo& l : lfos_) l.prepare(sr_);
        lfos_[0].setWave(kit::Lfo::RandomSmooth);   // pitch wander
        lfos_[1].setWave(kit::Lfo::Sine);           // pitch wow
        lfos_[2].setWave(kit::Lfo::RandomSmooth);   // filter wobble
        lfos_[3].setWave(kit::Lfo::Tri);            // amp tremble
        line_.prepare(static_cast<int>(sr_ * 2.2)); // 1/4 note down to ~27 bpm
        hpf_.prepare(sr_);
        // ~80 ms one-pole on the delay time: Time changes bend like a tape echo.
        dCoef_ = 1.0 - std::exp(-1.0 / (0.08 * sr_));
        onFlush();
    }

    // ── events ──────────────────────────────────────────────────────────────

    void onNoteOn(const ratpipe::NoteEvent& e)
    {
        const float d = param(kDecay);
        const float decayMs = 60.0f + 2440.0f * d * d;
        ampEnv_.set(1.5f, decayMs, 0.55f, 30.0f);                 // holds at 55 % while the key is down
        glide_.setTime(5.0f + 295.0f * param(kGlide));

        const bool legato = e.slide && gate_;                     // 303: slide glides, does not retrigger
        glide_.noteOn(static_cast<float>(e.note), legato);
        if (! legato)
        {
            ampEnv_.gateOn();
            sinePhase_ = 0.0;                                     // every hit starts the same (determinism)
            sub_.reset();
            saw_.reset();
        }
        accent_ = e.accent ? 1.0f : 0.0f;
        vel_    = 0.5f + 0.5f * e.velocity;
        gate_   = true;
        note_   = e.note;
        quiet_  = false;                                          // echoes may ring from here on
    }

    void onNoteOff(const ratpipe::NoteEvent& e)
    {
        if (e.slideSource || e.note != note_) return;             // the delayed off of a slide source: ignore
        gate_ = false;
        ampEnv_.gateOff();
    }

    void onReleaseHeld() { gate_ = false; ampEnv_.gateOff(); }

    // ALL_OFF silences the voice and re-seeds the LFO bank; the echo tail is
    // allowed to ring (HAS_TAILS). FLUSH also clears the delay line.
    void onAllOff()
    {
        ampEnv_.kill();
        filt_.reset();
        dc_.reset();
        sub_.reset();
        saw_.reset();
        glide_.reset();
        sinePhase_ = 0.0;
        gate_   = false;
        note_   = -1;
        accent_ = 0.0f;
        vel_    = 1.0f;
        for (int i = 0; i < 4; ++i)
        {
            lfos_[i].reset();
            lfos_[i].seed(hostSeed() ^ (0x9e3779b97f4a7c15ull * static_cast<uint64_t>(i + 1)));
        }
    }

    void onFlush()
    {
        onAllOff();
        line_.clear();
        hpf_.reset();
        quiet_ = true;    // nothing audible until the next note: delay time may jump, no tape bend
    }

    // Once per host render call: echo time from the transport, LFO rates.
    void onBlockStart(int /*n*/)
    {
        const int step = paramStep(kTime);
        const double beats = kTimeBeats[step < 0 ? 0 : (step > 4 ? 4 : step)];
        const RatPipeTransport& t = transport();
        double bpm = 120.0;                                       // fallback when the host has no transport
        if (hasTransport() && t.playing != 0 && t.bpm > 20.0 && t.bpm < 1000.0)
            bpm = t.bpm;
        dTarget_ = kit::clamp(static_cast<float>(beats * 60.0 / bpm * sr_),
                              1.0f, static_cast<float>(line_.maxDelay()));
        if (quiet_) dSm_ = dTarget_;    // keeps snapping until the first note after a flush (M7:
                                        // a dirtied and a fresh instance must converge identically)

        // Master rate 0.1..8 Hz; the bank sits at detuned ratios so the
        // combined motion never repeats.
        const float v = param(kRate);
        const float hz = 0.1f * std::exp2(v * 6.3219f);
        lfos_[0].setRate(hz);
        lfos_[1].setRate(hz * 0.23f);
        lfos_[2].setRate(hz * 1.71f);
        lfos_[3].setRate(hz * 2.39f);
    }

    // ── audio ───────────────────────────────────────────────────────────────

    void render(float* L, float* R, int n)
    {
        const float unstab = param(kUnstab);
        const float drive  = param(kDrive);
        const float echo   = param(kEcho);
        const float subMix = param(kSub);
        const float growl  = param(kGrowl);
        const float res    = param(kRes) * (1.0f - 0.15f * accent_);
        const float level  = param(kLevel);
        const float fb     = 0.92f * param(kFeedb);
        const float baseHz = 40.0f * kit::semitonesToRatio(param(kCutoff) * 86.8f);    // 40 Hz .. 6 kHz
        const float hpHz   = 30.0f * kit::semitonesToRatio(param(kEchoHp) * 67.7f);    // 30 Hz .. 1.5 kHz
        const float preG   = 1.0f + (11.0f + 3.0f * accent_) * drive;
        const float preB   = 0.4f * drive;
        const float preTrim = 1.0f / (1.0f + 0.7f * drive);       // loudness roughly flat as Drive rises
        const float accGain = 1.0f + 0.3f * accent_;
        const float outDrive = 1.2f;
        const float outNorm  = 1.0f / std::tanh(outDrive);

        float driftSt = 0.0f, wobOct = 0.0f, trem = 1.0f;
        for (int i = 0; i < n; ++i)
        {
            // Control rate (32 samples): the LFO bank, cutoff exp(), echo HPF.
            // The bank only runs while the envelope is audible, so after
            // ALL_OFF + FLUSH its phases match a fresh instance exactly (M7).
            if ((i & 31) == 0)
            {
                if (ampEnv_.isActive())
                {
                    const float wander = lfos_[0].tick(32);
                    const float wow    = lfos_[1].tick(32);
                    const float wob    = lfos_[2].tick(32);
                    const float tr     = lfos_[3].tick(32);
                    driftSt = unstab * (0.6f * wander + 0.25f * wow);
                    wobOct  = unstab * 1.2f * wob;
                    trem    = 1.0f - 0.3f * unstab * (0.5f + 0.5f * tr);
                }
                const float aEnvNow = ampEnv_.value();
                const float cutHz = baseHz * kit::semitonesToRatio(12.0f * (1.0f * aEnvNow * (1.0f + 1.2f * accent_) + wobOct));
                filt_.set(cutHz, res, 1.0f);
                hpf_.set(hpHz, 0.05f);
            }

            float v = 0.0f;
            if (ampEnv_.isActive())
            {
                const float aEnv = ampEnv_.process();
                const float hz   = kit::midiToHz(glide_.tickNote() + driftSt);

                // Sine fundamental + sub an octave down + saw growl layer.
                double inc = static_cast<double>(hz) / sr_;
                if (inc > 0.49) inc = 0.49;
                const float sine = static_cast<float>(std::sin(kTwoPi * sinePhase_));
                sinePhase_ = wrap01(sinePhase_ + inc);
                saw_.setFrequency(hz);
                float x = 0.55f * sine + 0.75f * subMix * sub_.process(hz) + 0.5f * growl * saw_.process();

                // Phat drive: asymmetric tanh (even harmonics), DC removed. Bounded < 2.
                x = dc_.process(kit::sat::asymTanh(x, preG, preB)) * preTrim;

                // Ladder: tanh-bounded loop, |y| <= 3.
                x = filt_.process(x);

                v = x * aEnv * vel_ * accGain * trem;
            }

            // Echo: smoothed delay time (tape bend), variable high-pass inside
            // the loop so every repeat loses more low end, tanh bound on the
            // feedback (M4/M6: loop gain < 1, bounded non-linearity only).
            dSm_ += dCoef_ * (static_cast<double>(dTarget_) - dSm_);
            const float wet = hpf_.processHp(line_.read(static_cast<float>(dSm_)));
            line_.write(v + fb * kit::sat::fastTanh(wet));

            // Output: soft tanh ceiling, Level. tanh < 1, 0.62 * level <= 0.93
            // and outNorm ~= 1.2: the peak stays under the 2.0 bound (M3).
            const float y = std::tanh((v + echo * wet) * outDrive) * outNorm * 0.62f * level;
            L[i] = y;
            R[i] = y;
        }
    }

private:
    static double wrap01(double p) noexcept { return p - std::floor(p); }

    double sr_ = 48000.0;
    double sinePhase_ = 0.0;
    double dSm_ = 0.0, dCoef_ = 0.0;
    float  dTarget_ = 1.0f;
    bool   quiet_ = true;
    kit::SubOsc    sub_;
    kit::Osc       saw_;
    kit::DcBlock   dc_;
    kit::LadderLite filt_;
    kit::Adsr      ampEnv_;
    kit::Glide     glide_;
    kit::Lfo       lfos_[4];
    kit::DelayLine line_;
    kit::Svf       hpf_;
    float accent_ = 0.0f, vel_ = 1.0f;
    bool  gate_   = false;
    int   note_   = -1;
};

} // namespace

RATPIPE_DEFINE_ENTRY(DeepDriftPipe)
