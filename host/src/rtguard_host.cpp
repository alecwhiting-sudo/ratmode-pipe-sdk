#include "rtguard.hpp"
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__APPLE__)
 #include <dlfcn.h>
 #include <mach-o/dyld.h>
 #include <unistd.h>
#endif

extern "C" int ratpipe_rt_linux_active();   // rtguard_linux.cpp (Linux only)
#if defined(__linux__)
extern "C" {
void ratpipe_rt_enter(); void ratpipe_rt_leave(); void ratpipe_rt_reset();
uint32_t ratpipe_rt_count(int); const char* ratpipe_rt_first();
}
#endif

namespace rp {
namespace rtguard {

namespace {
typedef void (*VoidFn)();
typedef uint32_t (*CountFn)(int);
typedef const char* (*FirstFn)();

VoidFn  g_enter = nullptr, g_leave = nullptr, g_reset = nullptr;
CountFn g_count = nullptr;
FirstFn g_first = nullptr;
bool    g_resolved = false;
std::string g_why;

void resolve()
{
    if (g_resolved) return;
    g_resolved = true;
#if defined(__linux__)
    if (ratpipe_rt_linux_active() == 0)
    {
        g_why = "this host was built without the allocation/IO interposers (non-glibc libc or sanitizer build)";
        return;
    }
    g_enter = ratpipe_rt_enter; g_leave = ratpipe_rt_leave; g_reset = ratpipe_rt_reset;
    g_count = ratpipe_rt_count; g_first = ratpipe_rt_first;
#elif defined(__APPLE__)
    g_enter = (VoidFn) dlsym(RTLD_DEFAULT, "ratpipe_rt_enter");
    g_leave = (VoidFn) dlsym(RTLD_DEFAULT, "ratpipe_rt_leave");
    g_reset = (VoidFn) dlsym(RTLD_DEFAULT, "ratpipe_rt_reset");
    g_count = (CountFn) dlsym(RTLD_DEFAULT, "ratpipe_rt_count");
    g_first = (FirstFn) dlsym(RTLD_DEFAULT, "ratpipe_rt_first");
    if (g_enter == nullptr || g_leave == nullptr || g_reset == nullptr || g_count == nullptr || g_first == nullptr)
    {
        g_enter = g_leave = g_reset = nullptr; g_count = nullptr; g_first = nullptr;
        g_why = "libratpipe_rtguard.dylib is not preloaded (DYLD_INSERT_LIBRARIES); run through `tools/ratpipe validate`, or via the host's own re-exec";
    }
#else
    g_why = "no interposition support on this platform";
#endif
}
} // namespace

bool available(std::string& whyNot)
{
    resolve();
    if (g_enter == nullptr) { whyNot = g_why; return false; }
    return true;
}

void ensurePreloaded(char** argv)
{
#if defined(__APPLE__)
    if (std::getenv("RATPIPE_RTGUARD_PRELOADED") != nullptr) return;
    char path[4096]; uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0) return;
    std::string exe(path);
    const size_t slash = exe.rfind('/');
    if (slash == std::string::npos) return;
    const std::string dylib = exe.substr(0, slash) + "/libratpipe_rtguard.dylib";
    if (access(dylib.c_str(), R_OK) != 0) return;
    setenv("DYLD_INSERT_LIBRARIES", dylib.c_str(), 1);
    setenv("RATPIPE_RTGUARD_PRELOADED", "1", 1);
    execv(path, argv);   // returns only on failure; then we carry on without the guard
#else
    (void) argv;
#endif
}

void enter() { resolve(); if (g_enter) g_enter(); }
void leave() { if (g_leave) g_leave(); }
void reset() { resolve(); if (g_reset) g_reset(); }

Report snapshot()
{
    Report r;
    resolve();
    if (g_count == nullptr) return r;
    r.allocs = g_count(0); r.frees = g_count(1); r.io = g_count(2); r.locks = g_count(3); r.sleeps = g_count(4);
    r.first = g_first ? g_first() : "";
    return r;
}

} // namespace rtguard
} // namespace rp
