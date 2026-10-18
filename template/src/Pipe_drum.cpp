// @DISPLAY@ — drum pipe (starter).
//
// A sine kick: decaying amplitude, a pitch envelope that sweeps down from
// "punch" octaves above the resting pitch, and tanh drive. Written straight
// against the C ABI so it builds with nothing but this repo.
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

constexpr double kTwoPi = 6.28318530717958647692;

enum ParamIndex { kPitch = 0, kDecay, kPunch, kDrive, kLevel, kParamCount };

// Field order: struct_size, id, label (<= 8 chars), group, default, min, max,
// mutate_lo, mutate_hi, flags, steps, readout_kind, readout_min, readout_max,
// choices, reserved[8] (zero).
const RatPipeParamDesc kParams[kParamCount] = {
    { sizeof(RatPipeParamDesc), "@PREFIX@.pitch", "Pitch", "Tone", 0.35f, 0.0f, 1.0f, 0.10f, 0.80f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_HZ,   35.0f, 160.0f,  nullptr, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "@PREFIX@.decay", "Decay", "Env",  0.45f, 0.0f, 1.0f, 0.10f, 0.90f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_MS,   40.0f, 1200.0f, nullptr, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "@PREFIX@.punch", "Punch", "Env",  0.50f, 0.0f, 1.0f, 0.00f, 1.00f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_PCT,  0.0f,  100.0f,  nullptr, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "@PREFIX@.drive", "Drive", "Tone", 0.25f, 0.0f, 1.0f, 0.00f, 0.90f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_PCT,  0.0f,  100.0f,  nullptr, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "@PREFIX@.level", "Level", "Out",  1.00f, 0.0f, 1.5f, 0.00f, 0.00f, RATPIPE_PARAM_GAIN_STYLE | RATPIPE_PARAM_NOT_MUTABLE, 0, RATPIPE_READOUT_NONE, 0.0f, 0.0f, nullptr, { 0, 0, 0, 0, 0, 0, 0, 0 } },
};

struct Pipe
{
    // The ONLY state shared with the message thread: one atomic per parameter.
    std::atomic<float> param[kParamCount];

    double sr = 48000.0;
    double phase = 0.0;
    double baseHz = 50.0, sweepOct = 1.0;     // frozen per hit
    float  amp = 0.0f, ampK = 0.9995f;
    float  pitchEnv = 0.0f, pitchK = 0.998f;
    float  velocity = 1.0f;

    void reset() { phase = 0.0; amp = 0.0f; pitchEnv = 0.0f; }

    float get(int i) const { return param[i].load(std::memory_order_relaxed); }

    void trigger(const RatPipeEvent& e)
    {
        const double pitch = 35.0 + 125.0 * (double) get(kPitch);                         // Hz
        baseHz = pitch * std::exp2((double) e.semitone_offset / 12.0);
        sweepOct = 3.0 * (double) get(kPunch);                                            // octaves above rest
        const double decayS = 0.04 + 1.16 * (double) get(kDecay) * (double) get(kDecay);
        ampK   = (float) std::exp(-1.0 / (decayS * 0.3 * sr));
        pitchK = (float) std::exp(-1.0 / (0.03 * sr));
        amp = 1.0f;
        pitchEnv = 1.0f;
        velocity = e.velocity < 0.0f ? 0.0f : (e.velocity > 1.0f ? 1.0f : e.velocity);
        phase = 0.0;
    }

    void handle(const RatPipeEvent& e)
    {
        switch (e.type)
        {
            case RATPIPE_EV_TRIGGER:     trigger(e); break;
            case RATPIPE_EV_ALL_OFF:
            case RATPIPE_EV_FLUSH:       reset(); break;
            default: break;                                  // a kick has no note-off / release
        }
    }

    void render(float* L, float* R, int n, const RatPipeEvent* ev, int evCount)
    {
        const float drive = get(kDrive), level = get(kLevel);
        const float gain = 1.0f + 6.0f * drive;
        const float norm = 1.0f / std::tanh(gain);

        int e = 0;
        for (int i = 0; i < n; ++i)
        {
            while (e < evCount && ev[e].sample_offset <= i) handle(ev[e++]);
            if (amp <= 1.0e-6f) { amp = 0.0f; continue; }

            const double hz = baseHz * std::exp2(sweepOct * (double) pitchEnv);
            phase += hz / sr;
            if (phase >= 1.0) phase -= 1.0;
            const float y = std::tanh(gain * (float) std::sin(kTwoPi * phase) * amp) * norm * amp * velocity * 0.9f * level;

            amp *= ampK;
            pitchEnv *= pitchK;
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
    RATPIPE_DRUM, 0, "@PREFIX@",
    kParams, kParamCount, 0,
    { kPitch, kDecay, kPunch, kDrive },           // the four Perform knobs
    create, destroy, prepare, render, setParam, getParam,
    nullptr, { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }
};

} // namespace

extern "C" RATPIPE_EXPORT const RatPipeDescriptor* ratpipe_entry(const RatPipeHostInfo* /*host*/)
{
    return &kDescriptor;
}
