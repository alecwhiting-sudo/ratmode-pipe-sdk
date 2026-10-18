/*
 * bad_pipe.c — deliberately broken pipes for the validator's own tests.
 *
 * ONE source, many defects: build with exactly one -DBAD_<NAME>=1 and the
 * matching check (see host/tests/test_validator.py) must fail while the
 * others stay green. Without any BAD_ define this is a tiny well-behaved pipe
 * (a decaying sine for drum kinds, a gated sine for melodic kinds).
 *
 * Plain C99 against the vendored ABI header; not an example of good style.
 */
#include <ratpipe/ratpipe_abi.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(BAD_LOCK)
 #include <pthread.h>
#endif

#if defined(BAD_IDS)
 #define PFX "fx"                 /* reserved prefix */
 #define ID0 "fx.level"
 #define ID1 "level2"             /* not prefix-rooted */
#else
 #define PFX "bp"
 #define ID0 "bp.freq"
 #define ID1 "bp.level"
#endif

#if defined(BAD_DRUM)
 #define KIND RATPIPE_DRUM
#else
 #define KIND RATPIPE_MONO
#endif

#if defined(BAD_BOUNDS)
 #define DEF0 1.4f                /* default outside [0, 1] */
#else
 #define DEF0 0.5f
#endif

static const RatPipeParamDesc kParams[2] = {
    { sizeof(RatPipeParamDesc), ID0, "Freq",  "Tone", DEF0, 0.0f, 1.0f, 0.0f, 0.0f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_NONE, 0.0f, 0.0f, NULL, { 0 } },
#if defined(BAD_STRUCT)
    { sizeof(RatPipeParamDesc) - 4, ID1, "Level", "Out", 0.8f, 0.0f, 1.0f, 0.0f, 0.0f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_NONE, 0.0f, 0.0f, NULL, { 0 } },
#else
    { sizeof(RatPipeParamDesc), ID1, "Level", "Out", 0.8f, 0.0f, 1.0f, 0.0f, 0.0f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_NONE, 0.0f, 0.0f, NULL, { 0 } },
#endif
};

typedef struct Inst
{
    _Atomic float p[2];
    double sr, phase, env;
    int    gate, notes;
    unsigned noise;
    float  idleDc;
} Inst;

#if defined(BAD_NONDET)
static unsigned g_instances = 0;
#endif

static void* bp_create(double sr, int32_t max_block, const RatPipeHostInfo* host)
{
    Inst* in = (Inst*) calloc(1, sizeof(Inst));
    (void) max_block; (void) host;
    if (! in) return NULL;
    in->sr = sr > 1000.0 ? sr : 48000.0;
    atomic_store(&in->p[0], DEF0);
    atomic_store(&in->p[1], 0.8f);
    in->noise = 12345u;
#if defined(BAD_NONDET)
    in->noise += 7919u * (++g_instances);
#endif
    return in;
}
static void bp_destroy(void* p) { free(p); }
static void bp_prepare(void* p, double sr, int32_t mb) { Inst* in = (Inst*) p; (void) mb; if (sr > 1000.0) in->sr = sr; }
static void bp_set(void* p, int32_t i, float v) { if (i >= 0 && i < 2) atomic_store(&((Inst*) p)->p[i], v); }
static float bp_get(void* p, int32_t i)
{
    if (i < 0 || i >= 2) return 0.0f;
    const float v = atomic_load(&((Inst*) p)->p[i]);
#if defined(BAD_ROUNDTRIP)
    return v * 0.5f;
#else
    return v;
#endif
}

static void bp_render(void* p, float* L, float* R, int32_t n, const RatPipeEvent* ev, int32_t evc, const RatPipeTransport* tr)
{
    Inst* in = (Inst*) p;
    int32_t e = 0;
    (void) tr;
    const float lvl = atomic_load(&in->p[1]);
    const float fq  = atomic_load(&in->p[0]);

#if defined(BAD_ALLOC)
    { volatile char* m = (volatile char*) malloc(64); if (m) { m[0] = 1; } free((void*) m); }   /* volatile: the optimiser must not elide the pair */
#endif
#if defined(BAD_LOCK)
    { static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mx); pthread_mutex_unlock(&mx); }
#endif
#if defined(BAD_IO)
    { FILE* f = fopen("/dev/null", "w"); if (f) { fputs("x", f); fclose(f); } }
#endif
#if defined(BAD_SLOW)
    {   /* burn ~80 % of this block's real-time budget */
        const double budget = 0.8 * (double) n / in->sr;
        struct { long s, ns; } t0, t1; (void) t0; (void) t1;
        volatile double sink = 0.0;
        const long iters = (long) (budget * 4.0e8);
        for (long k = 0; k < iters; ++k) sink += (double) k * 1e-9;
    }
#endif
#if defined(BAD_OOB)
    L[n] = 0.5f;
#endif

    for (int32_t i = 0; i < n; ++i)
    {
        while (e < evc && ev[e].sample_offset <= i)
        {
            const RatPipeEvent* x = &ev[e++];
            if (x->type == RATPIPE_EV_TRIGGER || x->type == RATPIPE_EV_NOTE_ON)
            {
                in->env = 1.0; in->gate = 1; ++in->notes;
            }
            else if (x->type == RATPIPE_EV_NOTE_OFF) in->gate = 0;
            else if (x->type == RATPIPE_EV_ALL_OFF || x->type == RATPIPE_EV_FLUSH || x->type == RATPIPE_EV_RELEASE_HELD)
            {
#if defined(BAD_DRONE)
                /* ignored: the voice keeps droning */
#else
                in->gate = 0; in->env = 0.0;
#endif
            }
        }
        double s = 0.0;
        const double hz = 55.0 + 400.0 * fq;
        in->phase += 6.283185307179586 * hz / in->sr;
        if (in->phase > 6.283185307179586) in->phase -= 6.283185307179586;
#if defined(BAD_DRONE)
        if (in->env > 0.0) s = sin(in->phase) * in->env;             /* never decays, never releases */
#else
        if (KIND == RATPIPE_DRUM) { s = sin(in->phase) * in->env; in->env *= 0.9995; }
        else { in->env = in->gate ? 1.0 : in->env * 0.999; s = sin(in->phase) * in->env; }
#endif
        s *= 0.5 * lvl;
#if defined(BAD_STATE)
        s /= (1.0 + 0.05 * (double) in->notes);                      /* hidden, never-reset state */
#endif
#if defined(BAD_PEAK)
        s *= 6.0;
#endif
#if defined(BAD_NAN)
        if (fq > 0.9f) s = (double) (0.0f / (fq - fq));              /* NaN at high settings */
#endif
#if defined(BAD_DC)
        s += 0.05;
#endif
#if defined(BAD_NONDET)
        in->noise = in->noise * 1664525u + 1013904223u;
        s += 0.01 * ((double) (in->noise >> 8) / 8388608.0 - 1.0) * in->env;
#endif
#if defined(BAD_OVERWRITE)
        L[i] = (float) s; R[i] = (float) s;
#else
        L[i] += (float) s; R[i] += (float) s;
#endif
    }
}

static const RatPipeDescriptor kDesc = {
#if defined(BAD_DESC_SIZE)
    (uint32_t) (sizeof(RatPipeDescriptor) - 16),
#else
    (uint32_t) sizeof(RatPipeDescriptor),
#endif
    RATPIPE_ABI_MAJOR, RATPIPE_ABI_MINOR,
#if defined(BAD_NONDET)
    RATPIPE_FLAG_DETERMINISTIC_WITH_SEED,
#else
    0u,
#endif
    "dev.sdk.badpipe", "Bad Pipe", "SDK tests", "0.1.0", "0.1.0", KIND, 0, PFX,
    kParams, 2, 0, { 0, 1, -1, -1 },
    bp_create, bp_destroy, bp_prepare, bp_render, bp_set, bp_get,
    NULL, { NULL, NULL, NULL, NULL, NULL, NULL }
};

#if defined(BAD_EXPORT)
RATPIPE_EXPORT int ratpipe_extra_symbol(void) { return 42; }
#endif

RATPIPE_EXPORT const RatPipeDescriptor* ratpipe_entry(const RatPipeHostInfo* host)
{
    (void) host;
    return &kDesc;
}
