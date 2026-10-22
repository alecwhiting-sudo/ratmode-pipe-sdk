/*
 * test_tone — in-repo TEST EXTENSION for the extension loader (NOT an engine,
 * NOT shipped). A melodic mono part implemented straight against
 * ratpipe_abi.h in plain C11: PolyBLEP saw/square, a tanh-bounded two-pole
 * resonant filter, a decay envelope, note on/off/slide/accent handling.
 *
 * Why plain C with only libm/libc calls: the bundle must pass
 * ExtMachOCheck as written — dylib allowlist (libSystem.B only), import
 * allowlist (libm + memset/calloc/free family), no raw syscalls, no
 * LC_RPATH. No stdio, no threads, no C++ runtime.
 *
 * Constants: every lookup-free. The only literal data this file produces
 * are a few float/double immediates and the descriptor/param tables
 * (pointer-bearing → __DATA_CONST, never __TEXT), so the whole-__TEXT
 * syscall scan (ExtMachOCheck B1) has nothing but code to look at.
 *
 * Threading: set_param writes _Atomic floats; render reads them. Nothing
 * else is shared. create/destroy allocate; render never does.
 */
#include "ratpipe_abi.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TT_PI 3.14159265358979323846

enum { P_CUTOFF = 0, P_RESO, P_DECAY, P_DRIVE, P_WAVE, P_LEVEL, P_COUNT };

static const char* const kWaveChoices[2] = { "Saw", "Square" };

/* Field order: struct_size, id, label, group, default, min, max, mutate_lo,
 * mutate_hi, flags, steps, readout_kind, readout_min, readout_max, choices,
 * reserved[8] (must be zero in ABI 1.0). */
static const RatPipeParamDesc kParams[P_COUNT] = {
    { sizeof(RatPipeParamDesc), "tt.cutoff", "Cut",   "Filter", 0.55f, 0.0f, 1.0f, 0.15f, 0.95f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_HZ,   60.0f,  11000.0f, NULL, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "tt.reso",   "Res",   "Filter", 0.35f, 0.0f, 1.0f, 0.0f,  0.85f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_PCT,  0.0f,   100.0f,   NULL, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "tt.decay",  "Decay", "Env",    0.40f, 0.0f, 1.0f, 0.1f,  0.9f,  RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_MS,   20.0f,  2000.0f,  NULL, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "tt.drive",  "Drive", "Tone",   0.30f, 0.0f, 1.0f, 0.0f,  0.9f,  RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_PCT,  0.0f,   100.0f,   NULL, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "tt.wave",   "Wave",  "Tone",   0.0f,  0.0f, 1.0f, 0.0f,  0.0f,  RATPIPE_PARAM_DISCRETE,   2, RATPIPE_READOUT_NONE, 0.0f,   0.0f,     kWaveChoices, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "tt.level",  "Level", "Out",    1.0f,  0.0f, 1.5f, 0.0f,  0.0f,  RATPIPE_PARAM_GAIN_STYLE | RATPIPE_PARAM_NOT_MUTABLE, 0, RATPIPE_READOUT_NONE, 0.0f, 0.0f, NULL, { 0, 0, 0, 0, 0, 0, 0, 0 } },
};

typedef struct Inst
{
    _Atomic float params[P_COUNT];

    double sr;

    /* oscillator */
    double phase;          /* 0..1 */
    double inc;            /* phase increment per sample */
    double freqHz;         /* current (glided) frequency */
    double targetHz;       /* glide target */
    double glideCoef;      /* one-pole glide coefficient */

    /* envelope */
    float  env;            /* 0..1.3 (accent) */
    float  envCoef;        /* per-sample multiplier while decaying */
    int    gate;           /* 1 while a note is held */
    int    note;           /* MIDI note of the held voice, -1 none */

    /* filter */
    float  y1, y2;

    int    firstRender;
} Inst;

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float loadp(const Inst* in, int i) { return atomic_load_explicit(&in->params[i], memory_order_relaxed); }

static double midiToHz(int note) { return 440.0 * pow(2.0, ((double) note - 69.0) / 12.0); }

/* PolyBLEP residual for a discontinuity at phase t (0..1) with step dt. */
static double polyBlep(double t, double dt)
{
    if (t < dt)       { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

static void* tt_create(double sr, int32_t max_block, const RatPipeHostInfo* host)
{
    (void) max_block; (void) host;
    Inst* in = (Inst*) calloc(1, sizeof(Inst));
    if (in == NULL) return NULL;
    in->sr   = sr > 1000.0 ? sr : 44100.0;
    in->note = -1;
    in->freqHz = in->targetHz = 110.0;
    for (int i = 0; i < P_COUNT; ++i)
        atomic_store_explicit(&in->params[i], kParams[i].default_value, memory_order_relaxed);
    return in;
}

static void tt_destroy(void* p) { free(p); }

static void tt_prepare(void* p, double sr, int32_t max_block)
{
    (void) max_block;
    Inst* in = (Inst*) p;
    if (sr > 1000.0) in->sr = sr;
    in->env = 0.0f; in->gate = 0; in->note = -1;
    in->y1 = in->y2 = 0.0f;
    in->phase = 0.0;
}

static void tt_set_param(void* p, int32_t idx, float v)
{
    Inst* in = (Inst*) p;
    if (idx < 0 || idx >= P_COUNT) return;
    if (! (v == v)) return;                        /* NaN: ignore */
    atomic_store_explicit(&in->params[idx], v, memory_order_relaxed);
}

static float tt_get_param(void* p, int32_t idx)
{
    Inst* in = (Inst*) p;
    if (idx < 0 || idx >= P_COUNT) return 0.0f;
    return loadp(in, idx);
}

static void tt_note_on(Inst* in, int note, float vel, unsigned flags)
{
    const float decay = clamp01(loadp(in, P_DECAY));
    const double decayS = 0.02 + 1.98 * (double) decay * decay;     /* 20 ms .. 2 s */
    in->envCoef  = (float) exp(-1.0 / (decayS * in->sr));
    in->targetHz = midiToHz(note);
    in->note     = note;
    in->gate     = 1;

    if ((flags & RATPIPE_EVF_SLIDE_FROM_PREV) != 0 && in->env > 0.001f)
    {
        /* glide: retarget the running voice, keep the envelope going */
        in->glideCoef = 1.0 - exp(-1.0 / (0.06 * in->sr));         /* ~60 ms */
    }
    else
    {
        in->freqHz    = in->targetHz;
        in->glideCoef = 1.0;
        in->env       = vel < 0.0f ? 0.0f : (vel > 1.0f ? 1.0f : vel);
        if ((flags & RATPIPE_EVF_ACCENT) != 0) in->env = in->env * 1.3f;
        if (in->env > 1.3f) in->env = 1.3f;
    }
}

static void tt_note_off(Inst* in, int note)
{
    if (in->note == note || note < 0) in->gate = 0;
}

static void tt_all_off(Inst* in)
{
    in->env = 0.0f; in->gate = 0; in->note = -1;
    in->y1 = in->y2 = 0.0f;
}

static void tt_render(void* p, float* L, float* R, int32_t n,
                      const RatPipeEvent* ev, int32_t count, const RatPipeTransport* tr)
{
    (void) tr;
    Inst* in = (Inst*) p;
    if (in == NULL || n <= 0) return;
    if (count > 0 && (ev == NULL || ev[0].struct_size != (uint32_t) sizeof(RatPipeEvent))) count = 0;   /* not my ABI */

    const float cutoff = clamp01(loadp(in, P_CUTOFF));
    const float reso   = clamp01(loadp(in, P_RESO));
    const float drive  = clamp01(loadp(in, P_DRIVE));
    const int   square = loadp(in, P_WAVE) >= 0.5f;
    float level = loadp(in, P_LEVEL);
    if (level < 0.0f) level = 0.0f;
    if (level > 1.5f) level = 1.5f;

    /* cutoff 60 Hz .. 11 kHz, exponential; env adds up to +2 octaves */
    const double baseHz = 60.0 * pow(2.0, (double) cutoff * 7.5);
    const double nyq    = 0.45 * in->sr;
    const float  k      = 3.6f * reso;                   /* feedback amount, tanh-bounded below */
    const float  preGain = 1.0f + 7.0f * drive;
    const float  relCoef = (float) exp(-1.0 / (0.03 * in->sr));   /* 30 ms release after gate off */

    int32_t e = 0;
    for (int32_t i = 0; i < n; ++i)
    {
        while (e < count && ev[e].sample_offset <= i)
        {
            const RatPipeEvent* x = &ev[e++];
            switch (x->type)
            {
                case RATPIPE_EV_NOTE_ON:      tt_note_on(in, x->note, x->velocity, x->flags); break;
                case RATPIPE_EV_NOTE_OFF:     tt_note_off(in, x->note); break;
                case RATPIPE_EV_RELEASE_HELD: in->gate = 0; break;
                case RATPIPE_EV_ALL_OFF:
                case RATPIPE_EV_FLUSH:        tt_all_off(in); break;
                default: break;
            }
        }

        if (in->env < 1.0e-5f) { in->env = 0.0f; continue; }   /* additive: nothing to add */

        /* glide */
        in->freqHz += (in->targetHz - in->freqHz) * in->glideCoef;
        in->inc = in->freqHz / in->sr;
        if (in->inc > 0.49) in->inc = 0.49;

        /* PolyBLEP oscillator */
        double osc;
        if (square)
        {
            osc = in->phase < 0.5 ? 1.0 : -1.0;
            osc += polyBlep(in->phase, in->inc);
            double t2 = in->phase + 0.5; if (t2 >= 1.0) t2 -= 1.0;
            osc -= polyBlep(t2, in->inc);
        }
        else
        {
            osc = 2.0 * in->phase - 1.0;
            osc -= polyBlep(in->phase, in->inc);
        }
        in->phase += in->inc;
        if (in->phase >= 1.0) in->phase -= 1.0;

        /* envelope: decay while gated, faster release when released */
        in->env *= in->gate ? in->envCoef : relCoef;

        /* tanh-bounded two-pole: fb through tanh, input through tanh —
         * bounded saturation only (CLAUDE.md), never a polynomial. */
        double fc = baseHz * (1.0 + 2.0 * (double) in->env);
        if (fc > nyq) fc = nyq;
        const float g  = (float) (1.0 - exp(-2.0 * TT_PI * fc / in->sr));
        const float x  = tanhf(((float) osc * preGain * in->env) - k * tanhf(in->y2));
        in->y1 += g * (x - in->y1);
        in->y2 += g * (in->y1 - in->y2);

        float s = in->y2 * 0.6f * level;

        /* non-finite / runaway guard (the host sanitises too; be polite) */
        if (! (s == s)) { s = 0.0f; in->y1 = in->y2 = 0.0f; in->env = 0.0f; }
        else if (s > 2.0f) s = 2.0f; else if (s < -2.0f) s = -2.0f;

        L[i] += s;
        R[i] += s;
    }
    in->firstRender = 1;
}

static const RatPipeDescriptor kDesc = {
    sizeof(RatPipeDescriptor),
    RATPIPE_ABI_MAJOR, RATPIPE_ABI_MINOR,
    0u,
    "dev.ratmode.test_tone", "Test Tone", "RatMode test", "0.1.0", "",
    RATPIPE_MONO, 0,
    "tt",
    kParams, P_COUNT, 0,
    { P_CUTOFF, P_RESO, P_DECAY, P_DRIVE },
    tt_create, tt_destroy, tt_prepare, tt_render, tt_set_param, tt_get_param,
    NULL, { NULL, NULL, NULL, NULL, NULL, NULL }
};

RATPIPE_EXPORT const RatPipeDescriptor* ratpipe_entry(const RatPipeHostInfo* host)
{
    if (host == NULL || host->struct_size < sizeof(RatPipeHostInfo)) return NULL;
    if (host->abi_major != RATPIPE_ABI_MAJOR) return NULL;
    return &kDesc;
}
