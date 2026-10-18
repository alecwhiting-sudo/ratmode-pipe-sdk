/*
 * test_crash — DELIBERATELY CRASHING test extension for the Arch38
 * quarantine test (T5 / §13d "crash test"). Built ONLY when the CMake option
 * RATMODE_BUILD_TEST_CRASH_PIPE is ON (default OFF). Never install this
 * next to work you care about: the first render() writes through NULL and
 * kills RatMode on purpose, once. The crash marker RatMode wrote before the
 * block must then quarantine the id on the next launch.
 *
 * Until that first render it behaves like a silent drum pipe, so loading,
 * CONFIGURE and the consent prompt work normally — the crash lands exactly
 * inside the "PLAY -> first block" marker window.
 */
#include "ratpipe_abi.h"
#include <stdatomic.h>
#include <stdlib.h>

enum { P_DUMMY = 0, P_COUNT };

static const RatPipeParamDesc kParams[P_COUNT] = {
    { sizeof(RatPipeParamDesc), "tc.dummy", "Dummy", "Test", 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, RATPIPE_PARAM_CONTINUOUS, 0, RATPIPE_READOUT_NONE, 0.0f, 0.0f, NULL, { 0, 0, 0, 0, 0, 0, 0, 0 } },
};

typedef struct Inst
{
    _Atomic float params[P_COUNT];
    int renders;
} Inst;

static void* tc_create(double sr, int32_t max_block, const RatPipeHostInfo* host)
{
    (void) sr; (void) max_block; (void) host;
    Inst* in = (Inst*) calloc(1, sizeof(Inst));
    if (in == NULL) return NULL;
    atomic_store_explicit(&in->params[0], kParams[0].default_value, memory_order_relaxed);
    return in;
}

static void tc_destroy(void* p) { free(p); }
static void tc_prepare(void* p, double sr, int32_t max_block) { (void) p; (void) sr; (void) max_block; }

static void tc_set_param(void* p, int32_t idx, float v)
{
    Inst* in = (Inst*) p;
    if (idx != 0 || ! (v == v)) return;
    atomic_store_explicit(&in->params[0], v, memory_order_relaxed);
}

static float tc_get_param(void* p, int32_t idx)
{
    Inst* in = (Inst*) p;
    return idx == 0 ? atomic_load_explicit(&in->params[0], memory_order_relaxed) : 0.0f;
}

static void tc_render(void* p, float* L, float* R, int32_t n,
                      const RatPipeEvent* ev, int32_t count, const RatPipeTransport* tr)
{
    (void) L; (void) R; (void) n; (void) ev; (void) count; (void) tr;
    Inst* in = (Inst*) p;
    if (in == NULL) return;
    if (in->renders++ == 0)
    {
        volatile int* nowhere = (volatile int*) 0;
        *nowhere = 0x0BADF00D;   /* SIGSEGV on the audio thread — on purpose */
    }
}

static const RatPipeDescriptor kDesc = {
    sizeof(RatPipeDescriptor),
    RATPIPE_ABI_MAJOR, RATPIPE_ABI_MINOR,
    0u,
    "dev.ratmode.test_crash", "Test CRASH", "RatMode test", "0.1.0", "",
    RATPIPE_DRUM, 0,
    "tc",
    kParams, P_COUNT, 0,
    { P_DUMMY, RATPIPE_PERFORM_KNOB_NONE, RATPIPE_PERFORM_KNOB_NONE, RATPIPE_PERFORM_KNOB_NONE },
    tc_create, tc_destroy, tc_prepare, tc_render, tc_set_param, tc_get_param,
    NULL, { NULL, NULL, NULL, NULL, NULL, NULL }
};

RATPIPE_EXPORT const RatPipeDescriptor* ratpipe_entry(const RatPipeHostInfo* host)
{
    if (host == NULL || host->struct_size < sizeof(RatPipeHostInfo)) return NULL;
    if (host->abi_major != RATPIPE_ABI_MAJOR) return NULL;
    return &kDesc;
}
