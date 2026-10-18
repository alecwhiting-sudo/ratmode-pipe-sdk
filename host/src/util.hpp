// Small shared helpers for ratpipe-host.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace rp {

// splitmix64 — tiny, deterministic, good enough for fuzz parameters.
struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed = 1) : s(seed) {}
    uint64_t next()
    {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return (double) (next() >> 11) * (1.0 / 9007199254740992.0); }
    int range(int lo, int hi) { return lo + (int) (next() % (uint64_t) (hi - lo + 1)); }   // inclusive
    bool chance(double p) { return uniform() < p; }
};

inline std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
inline std::string fmt(const char* f, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

inline double toDb(double lin) { return lin <= 1e-12 ? -240.0 : 20.0 * std::log10(lin); }

} // namespace rp
