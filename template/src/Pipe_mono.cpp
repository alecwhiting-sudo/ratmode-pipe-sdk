// @DISPLAY@ — melodic mono pipe (starter).
//
// A PolyBLEP saw -> one-pole low-pass -> tanh drive, with a decaying envelope,
// 303-style slide (glide, no retrigger) and accent. Written straight against
// the C ABI so it builds with nothing but this repo.
//
// TODO(sdk): port this to ratpipe/pipe_base.hpp (parameter table -> atomics,
// event dispatch, try/catch guard, RATPIPE_DEFINE_ENTRY) once that header is
// the recommended starting point; the DSP below should carry over unchanged.
//
// The rules that must hold (docs/CONTRACT.md):
//   * render() never allocates, locks or does IO — everything is sized in create().
//   * render() ADDS into L/R, writes only [0, n), output is finite and <= 2.0.
//   * every sound-affecting value is a declared parameter; ALL_OFF / FLUSH
//     return the voice to silence so two instances end up identical.
//   * only tanh-family saturation, no cubic soft-clippers.
#include <ratpipe/ratpipe_abi.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <new>

namespace {

constexpr double kPi = 3.14159265358979323846;

enum ParamIndex { kCutoff = 0, kDecay, kDrive, kGlide, kLevel, kParamCount };

// Field order: struct_size, id, label (<= 8 chars), group, default, min, max,
// mutate_lo, mutate_hi, flags, steps, readout_kind, readout_min, readout_max,
// choices, reserved[8] (zero).
const RatPipeParamDesc kParams[kParamCount] = {
    { sizeof(RatPipeParamDesc), "@PREFIX@.cutoff", "Cut",   "Filter", 0.55f, 0.0f, 1.0f, 0.15f, 0.95f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_HZ,   60.0f, 11000.0f, nullptr, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "@PREFIX@.decay",  "Decay", "Env",    0.40f, 0.0f, 1.0f, 0.10f, 0.90f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_MS,   30.0f, 1500.0f,  nullptr, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "@PREFIX@.drive",  "Drive", "Tone",   0.30f, 0.0f, 1.0f, 0.00f, 0.90f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_PCT,  0.0f,  100.0f,   nullptr, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "@PREFIX@.glide",  "Glide", "Tone",   0.35f, 0.0f, 1.0f, 0.00f, 0.80f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_MS,   5.0f,  250.0f,   nullptr, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "@PREFIX@.level",  "Level", "Out",    1.00f, 0.0f, 1.5f, 0.00f, 0.00f, RATPIPE_PARAM_GAIN_STYLE | RATPIPE_PARAM_NOT_MUTABLE, 0, RATPIPE_READOUT_NONE, 0.0f, 0.0f, nullptr, { 0, 0, 0, 0, 0, 0, 0, 0 } },
};

// polyBLEP residual for a discontinuity at phase t (0..1) with step dt.
inline double polyBlep(double t, double dt)
{
    if (t < dt)       { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

inline double midiToHz(int note) { return 440.0 * std::pow(2.0, ((double) note - 69.0) / 12.0); }

struct Pipe
{
    // The ONLY state shared with the message thread: one atomic per parameter.
    std::atomic<float> param[kParamCount];

    double sr = 48000.0;
    double phase = 0.0, freq = 110.0, target = 110.0;
    float  env = 0.0f, accent = 0.0f, lp = 0.0f;
    bool   gate = false;
    int    note = -1;

    void reset()
    {
        phase = 0.0; env = 0.0f; accent = 0.0f; lp = 0.0f; gate = false; note = -1;
    }

    float get(int i) const { return param[i].load(std::memory_order_relaxed); }

    void noteOn(const RatPipeEvent& e)
    {
        target = midiToHz(e.note);
        const bool slide = (e.flags & RATPIPE_EVF_SLIDE_FROM_PREV) != 0 && gate;
        if (! slide) { freq = target; env = 1.0f; phase = 0.0; }    // legato slide: glide, don't retrigger
        accent = (e.flags & RATPIPE_EVF_ACCENT) != 0 ? 1.0f : 0.0f;
        gate = true;
        note = e.note;
    }

    void handle(const RatPipeEvent& e)
    {
        switch (e.type)
        {
            case RATPIPE_EV_NOTE_ON:  noteOn(e); break;
            case RATPIPE_EV_NOTE_OFF:
                // The delayed off of a slide source belongs to the note we already left.
                if ((e.flags & RATPIPE_EVF_SLIDE_SOURCE) == 0 && e.note == note) gate = false;
                break;
            case RATPIPE_EV_RELEASE_HELD: gate = false; break;
            case RATPIPE_EV_ALL_OFF:
            case RATPIPE_EV_FLUSH:        reset(); break;
            default: break;                                           // TRIGGER does not occur for mono pipes
        }
    }

    void render(float* L, float* R, int n, const RatPipeEvent* ev, int evCount)
    {
        const float cutoff = get(kCutoff), decay = get(kDecay), drive = get(kDrive), glide = get(kGlide), level = get(kLevel);

        const double decayS   = 0.03 + 1.47 * (double) decay * (double) decay;       // seconds
        const double decayK   = std::exp(-1.0 / (decayS * 0.35 * sr));               // per-sample multiplier while gated
        const double releaseK = std::exp(-1.0 / (0.012 * sr));                       // fast release when the gate closes
        const double glideK   = 1.0 - std::exp(-1.0 / ((0.002 + 0.25 * (double) glide) * sr));
        const float  gain     = 1.0f + 7.0f * drive;
        const float  norm     = 1.0f / std::tanh(gain);                              // keeps loudness steady as Drive moves

        int e = 0;
        double a = 0.0;                                                              // filter coefficient, refreshed every 32 samples
        for (int i = 0; i < n; ++i)
        {
            while (e < evCount && ev[e].sample_offset <= i) handle(ev[e++]);

            if ((i & 31) == 0)
            {
                const double octaves = 3.0 + 8.0 * (double) cutoff + (double) env * (1.0 + 1.5 * (double) accent);
                double fc = 40.0 * std::exp2(octaves - 3.0);
                if (fc > 0.45 * sr) fc = 0.45 * sr;
                a = 1.0 - std::exp(-2.0 * kPi * fc / sr);
            }

            freq += (target - freq) * glideK;
            const double dt = freq / sr;
            phase += dt;
            if (phase >= 1.0) phase -= 1.0;
            const double saw = 2.0 * phase - 1.0 - polyBlep(phase, dt);

            env = (float) ((double) env * (gate ? decayK : releaseK));
            if (env < 1.0e-6f) env = 0.0f;

            lp += (float) (a * (saw - (double) lp));
            const float y = std::tanh(gain * lp) * norm * env * (0.45f + 0.25f * accent) * level;
            L[i] += y;
            R[i] += y;
        }
    }
};

// ── C ABI glue ───────────────────────────────────────────────────────────────

void* create(double sampleRate, int32_t /*maxBlock*/, const RatPipeHostInfo* /*host*/)
{
    try
    {
        Pipe* p = new Pipe();
        p->sr = sampleRate > 1000.0 ? sampleRate : 48000.0;
        for (int i = 0; i < kParamCount; ++i) p->param[i].store(kParams[i].default_value, std::memory_order_relaxed);
        return p;
    }
    catch (...) { return nullptr; }
}

void destroy(void* inst) { delete static_cast<Pipe*>(inst); }

void prepare(void* inst, double sampleRate, int32_t /*maxBlock*/)
{
    Pipe* p = static_cast<Pipe*>(inst);
    if (sampleRate > 1000.0) p->sr = sampleRate;
    p->reset();
}

void setParam(void* inst, int32_t index, float value)
{
    if (index < 0 || index >= kParamCount || ! std::isfinite(value)) return;
    static_cast<Pipe*>(inst)->param[index].store(value, std::memory_order_relaxed);
}

float getParam(void* inst, int32_t index)
{
    if (index < 0 || index >= kParamCount) return 0.0f;
    return static_cast<Pipe*>(inst)->get(index);
}

void render(void* inst, float* L, float* R, int32_t n, const RatPipeEvent* events, int32_t eventCount,
            const RatPipeTransport* /*transport*/)
{
    // No exception may cross the ABI.
    try
    {
        // Events are only meaningful if the host's struct size matches ours (see ratpipe_abi.h).
        if (eventCount > 0 && events[0].struct_size != sizeof(RatPipeEvent)) eventCount = 0;
        static_cast<Pipe*>(inst)->render(L, R, n, events, eventCount);
    }
    catch (...) {}
}

const RatPipeDescriptor kDescriptor = {
    sizeof(RatPipeDescriptor), RATPIPE_ABI_MAJOR, RATPIPE_ABI_MINOR,
    0u,                                           // flags
    "@ID@", "@DISPLAY@", "@AUTHOR@", "0.1.0", "@SDK_VERSION@",
    RATPIPE_MONO, 0, "@PREFIX@",
    kParams, kParamCount, 0,
    { kCutoff, kDecay, kDrive, kGlide },          // the four Perform knobs
    create, destroy, prepare, render, setParam, getParam,
    nullptr, { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }
};

} // namespace

extern "C" RATPIPE_EXPORT const RatPipeDescriptor* ratpipe_entry(const RatPipeHostInfo* /*host*/)
{
    return &kDescriptor;
}
