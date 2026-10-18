// The extern "C" control surface of the guard. Compiled into the same binary
// as the interposers (host on Linux, ratpipe_rtguard dylib on macOS).
#include "rtguard_core.h"

extern "C" {

void ratpipe_rt_enter() { rtcore::setRender(true); }
void ratpipe_rt_leave() { rtcore::setRender(false); }

void ratpipe_rt_reset()
{
    for (auto& c : rtcore::g_count) c.store(0);
    rtcore::g_firstSet.store(0);
    rtcore::g_first[0] = 0;
}

uint32_t ratpipe_rt_count(int kind)
{
    return (kind >= 0 && kind < rtcore::kKinds) ? rtcore::g_count[kind].load() : 0u;
}

const char* ratpipe_rt_first() { return rtcore::g_first; }

} // extern "C"

#if defined(__APPLE__)
__attribute__((constructor)) static void ratpipe_rt_init() { rtcore::initKey(); }
#endif
