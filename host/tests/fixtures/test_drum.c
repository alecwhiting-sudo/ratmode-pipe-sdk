/*
 * test_drum — in-repo TEST EXTENSION for the extension loader (NOT an engine,
 * NOT shipped). A drum part: sine kick with a pitch envelope, a short
 * noise click and tanh drive, implemented straight against ratpipe_abi.h
 * in plain C11. Same constraints as test_tone.c: libm + calloc/free only,
 * no stdio/threads, atomics for the params, no allocation in render.
 */
#include "ratpipe_abi.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TD_TWO_PI 6.28318530717958647692

enum { P_PITCH = 0, P_DECAY, P_PUNCH, P_CLICK, P_LEVEL, P_COUNT };

static const RatPipeParamDesc kParams[P_COUNT] = {
    { sizeof(RatPipeParamDesc), "td.pitch", "Pitch", "Tone", 0.35f, 0.0f, 1.0f, 0.1f, 0.8f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_HZ,  35.0f, 160.0f,  NULL, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "td.decay", "Decay", "Env",  0.45f, 0.0f, 1.0f, 0.1f, 0.9f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_MS,  40.0f, 1200.0f, NULL, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "td.punch", "Punch", "Env",  0.50f, 0.0f, 1.0f, 0.0f, 1.0f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_PCT, 0.0f,  100.0f,  NULL, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "td.click", "Click", "Tone", 0.25f, 0.0f, 1.0f, 0.0f, 0.8f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_PCT, 0.0f,  100.0f,  NULL, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { sizeof(RatPipeParamDesc), "td.level", "Level", "Out",  1.0f,  0.0f, 1.5f, 0.0f, 0.0f, RATPIPE_PARAM_GAIN_STYLE | RATPIPE_PARAM_NOT_MUTABLE, 0, RATPIPE_READOUT_NONE, 0.0f, 0.0f, NULL, { 0, 0, 0, 0, 0, 0, 0, 0 } },
};

typedef struct Inst
{
    _Atomic float params[P_COUNT];
    double   sr;
    double   phase;        /* radians */
    double   baseHz;       /* resting pitch of the current hit */
    float    amp, ampCoef; /* amplitude envelope */
    float    pEnv, pCoef;  /* pitch envelope 0..1 */
    float    punch;        /* octaves of pitch sweep for this hit */
    float    click, clickCoef;
    float    drive;
    unsigned rng;
    int      active;
} Inst;

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float loadp(const Inst* in, int i) { return atomic_load_explicit(&in->params[i], memory_order_relaxed); }

static void* td_create(double sr, int32_t max_block, const RatPipeHostInfo* host)
{
    (void) max_block; (void) host;
    Inst* in = (Inst*) calloc(1, sizeof(Inst));
    if (in == NULL) return NULL;
    in->sr  = sr > 1000.0 ? sr : 44100.0;
    in->rng = 0x9E3779B9u;
    for (int i = 0; i < P_COUNT; ++i)
        atomic_store_explicit(&in->params[i], kParams[i].default_value, memory_order_relaxed);
    return in;
}

static void td_destroy(void* p) { free(p); }

static void td_prepare(void* p, double sr, int32_t max_block)
{
    (void) max_block;
    Inst* in = (Inst*) p;
    if (sr > 1000.0) in->sr = sr;
    in->active = 0; in->amp = 0.0f; in->click = 0.0f; in->phase = 0.0;
}

static void td_set_param(void* p, int32_t idx, float v)
{
    Inst* in = (Inst*) p;
    if (idx < 0 || idx >= P_COUNT) return;
    if (! (v == v)) return;
    atomic_store_explicit(&in->params[idx], v, memory_order_relaxed);
}

static float td_get_param(void* p, int32_t idx)
{
    Inst* in = (Inst*) p;
    if (idx < 0 || idx >= P_COUNT) return 0.0f;
    return loadp(in, idx);
}

static void td_hit(Inst* in, float vel, int semitones)
{
    const float pitch = clamp01(loadp(in, P_PITCH));
    const float decay = clamp01(loadp(in, P_DECAY));
    const float punch = clamp01(loadp(in, P_PUNCH));
    const float click = clamp01(loadp(in, P_CLICK));

    if (semitones < -24) semitones = -24;
    if (semitones >  24) semitones =  24;
    in->baseHz = (35.0 + 125.0 * (double) pitch) * pow(2.0, (double) semitones / 12.0);

    const double decayS = 0.04 + 1.16 * (double) decay * decay;      /* 40 ms .. 1.2 s */
    in->ampCoef = (float) exp(-1.0 / (decayS * in->sr));
    in->pCoef   = (float) exp(-1.0 / ((0.015 + 0.06 * (double) punch) * in->sr));   /* 15..75 ms sweep */
    in->punch   = 1.0f + 3.0f * punch;                                  /* octaves above base at t=0 */
    in->clickCoef = (float) exp(-1.0 / (0.004 * in->sr));               /* 4 ms burst */
    in->click   = click * 0.6f;
    in->drive   = 1.0f + 2.5f * click;

    in->amp   = vel < 0.0f ? 0.0f : (vel > 1.0f ? 1.0f : vel);
    in->pEnv  = 1.0f;
    in->phase = 0.0;
    in->active = 1;
}

static void td_render(void* p, float* L, float* R, int32_t n,
                      const RatPipeEvent* ev, int32_t count, const RatPipeTransport* tr)
{
    (void) tr;
    Inst* in = (Inst*) p;
    if (in == NULL || n <= 0) return;
    if (count > 0 && (ev == NULL || ev[0].struct_size != (uint32_t) sizeof(RatPipeEvent))) count = 0;

    float level = loadp(in, P_LEVEL);
    if (level < 0.0f) level = 0.0f;
    if (level > 1.5f) level = 1.5f;
    const double nyq = 0.45 * in->sr;

    int32_t e = 0;
    for (int32_t i = 0; i < n; ++i)
    {
        while (e < count && ev[e].sample_offset <= i)
        {
            const RatPipeEvent* x = &ev[e++];
            switch (x->type)
            {
                case RATPIPE_EV_TRIGGER:      td_hit(in, x->velocity, x->semitone_offset); break;
                case RATPIPE_EV_NOTE_ON:      td_hit(in, x->velocity, x->note - 36); break;   /* a drum on a melodic lane: still makes a sound */
                case RATPIPE_EV_RELEASE_HELD: in->ampCoef = 0.995f; break;
                case RATPIPE_EV_ALL_OFF:
                case RATPIPE_EV_FLUSH:        in->active = 0; in->amp = 0.0f; in->click = 0.0f; break;
                default: break;
            }
        }
        if (! in->active) continue;

        /* pitch envelope: base * 2^(punch * pEnv) */
        double hz = in->baseHz * pow(2.0, (double) (in->punch * in->pEnv));
        if (hz > nyq) hz = nyq;
        in->phase += TD_TWO_PI * hz / in->sr;
        if (in->phase > TD_TWO_PI) in->phase -= TD_TWO_PI;

        float s = (float) sin(in->phase) * in->amp;

        /* click: tiny LCG noise burst (no rand() import) */
        in->rng = in->rng * 1664525u + 1013904223u;
        const float noise = ((float) (in->rng >> 8) * (1.0f / 8388608.0f)) - 1.0f;
        s += noise * in->click;

        s = tanhf(s * in->drive) * 0.9f * level;    /* bounded saturation only */

        in->amp   *= in->ampCoef;
        in->pEnv  *= in->pCoef;
        in->click *= in->clickCoef;
        if (in->amp < 1.0e-5f) { in->active = 0; in->amp = 0.0f; }

        if (! (s == s)) { s = 0.0f; in->active = 0; in->amp = 0.0f; }
        else if (s > 2.0f) s = 2.0f; else if (s < -2.0f) s = -2.0f;

        L[i] += s;
        R[i] += s;
    }
}

static const RatPipeDescriptor kDesc = {
    sizeof(RatPipeDescriptor),
    RATPIPE_ABI_MAJOR, RATPIPE_ABI_MINOR,
    0u,
    "dev.ratmode.test_drum", "Test Drum", "RatMode test", "0.1.0", "",
    RATPIPE_DRUM, 0,
    "td",
    kParams, P_COUNT, 0,
    { P_PITCH, P_DECAY, P_PUNCH, P_CLICK },
    td_create, td_destroy, td_prepare, td_render, td_set_param, td_get_param,
    NULL, { NULL, NULL, NULL, NULL, NULL, NULL }
};

RATPIPE_EXPORT const RatPipeDescriptor* ratpipe_entry(const RatPipeHostInfo* host)
{
    if (host == NULL || host->struct_size < sizeof(RatPipeHostInfo)) return NULL;
    if (host->abi_major != RATPIPE_ABI_MAJOR) return NULL;
    return &kDesc;
}
