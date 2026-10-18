// Shared state of the real-time guard (V5 / V6). Header-only; included by exactly
// one translation unit per binary (the Linux interposer compiled into the host,
// or the macOS DYLD_INTERPOSE dylib), so the inline variables below are unique.
//
// The flag is thread-local and set only around the pipe's `render` call, so
// anything the host itself does (printing, allocating, timing) is never counted.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#if defined(__APPLE__)
 #include <pthread.h>
#endif

namespace rtcore {

enum Kind { kAlloc = 0, kFree, kIo, kLock, kSleep, kKinds };

inline std::atomic<uint32_t> g_count[kKinds];
inline std::atomic<int>      g_firstSet{0};
inline char                  g_first[64];

#if defined(__APPLE__)
// TLV access can itself malloc on first touch per thread, which would recurse
// into the interposed malloc; pthread_getspecific never allocates.
inline pthread_key_t     g_key;
inline std::atomic<int>  g_keyReady{0};
inline void initKey() { if (g_keyReady.exchange(1) == 0) pthread_key_create(&g_key, nullptr); }
inline bool inRender() { return g_keyReady.load(std::memory_order_relaxed) != 0 && pthread_getspecific(g_key) != nullptr; }
inline void setRender(bool b) { initKey(); pthread_setspecific(g_key, b ? reinterpret_cast<void*>(1) : nullptr); }
#else
inline thread_local int  t_flag = 0;
inline void initKey() {}
inline bool inRender() { return t_flag != 0; }
inline void setRender(bool b) { t_flag = b ? 1 : 0; }
#endif

inline void note(Kind k, const char* name)
{
    if (! inRender()) return;
    g_count[k].fetch_add(1, std::memory_order_relaxed);
    if (g_firstSet.exchange(1) == 0)
    {
        std::strncpy(g_first, name, sizeof(g_first) - 1);
        g_first[sizeof(g_first) - 1] = 0;
    }
}

} // namespace rtcore

// C entry points the host calls (directly on Linux, via dlsym on macOS).
extern "C" {
void     ratpipe_rt_enter();
void     ratpipe_rt_leave();
void     ratpipe_rt_reset();
uint32_t ratpipe_rt_count(int kind);
const char* ratpipe_rt_first();
}
