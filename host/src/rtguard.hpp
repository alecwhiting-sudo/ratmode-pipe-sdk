// Host-side view of the real-time guard (V5 / V6).
#pragma once
#include <cstdint>
#include <string>

namespace rp {
namespace rtguard {

struct Report
{
    uint32_t allocs = 0;    // malloc / calloc / realloc / operator new ...
    uint32_t frees  = 0;
    uint32_t io     = 0;    // open / read / write / fopen / printf / syscall ...
    uint32_t locks  = 0;    // pthread mutex / rwlock / cond / sem / dispatch ...
    uint32_t sleeps = 0;
    std::string first;      // name of the first offending call
};

// Resolves the guard. Returns true when interposition is active in this
// process. On macOS that needs the preloaded libratpipe_rtguard.dylib; see
// ensurePreloaded().
bool available(std::string& whyNot);

// macOS: re-exec the current process with the interposer dylib inserted
// (no-op elsewhere, or when already done). Returns only if no re-exec happened.
void ensurePreloaded(char** argv);

void enter();    // calling thread is now "inside render"
void leave();
void reset();
Report snapshot();

} // namespace rtguard
} // namespace rp
