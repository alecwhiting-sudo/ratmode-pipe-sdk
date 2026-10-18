// ratpipe kit — finite_guard.hpp
//
// Non-finite detection and clamping without relying on <cmath> classification
// (which -ffast-math is allowed to fold to "always finite"). Every helper here
// is a pure function on float bits: no allocation, no libm call, no state.
//
// Guarantees
//   isFinite(x)          exact IEEE-754 test (exponent field != all-ones).
//   sanitize(x, fb)      x if finite else fb (default 0).
//   clamp(x, lo, hi)     NaN -> lo (comparison ordering makes NaN fall to lo).
//   guard(x, limit)      finite AND |x| <= limit, NaN/Inf -> 0.
//   guardBuffer(p, n, l) in-place guard, returns true if anything was replaced.
//   zeroBuffer(p, n)     zero n floats WITHOUT a memset/bzero/memset_pattern
//                        library call (see below); use it for every runtime-
//                        length clear in a pipe, on any thread.
//
// Why zeroBuffer exists: Apple clang turns a zero memset of runtime length —
// including one it forms itself from a `p[i] = 0` loop, std::fill or
// vector::assign — into `___bzero` on x86_64 (`_bzero` on arm64), and a
// constant non-zero fill loop into `_memset_pattern16`. RatMode builds released
// before its import allowlist learnt those names refuse a bundle importing
// `___bzero` / `_memset_pattern*` at load (ExtMachOCheck), so the SDK never
// emits them: zeroBuffer loads the
// zero through `volatile`, which the loop-idiom pass cannot recognise as a
// fill. The loop still vectorises.
#pragma once
#include <cstdint>
#include <cstring>

namespace ratpipe { namespace kit {

inline uint32_t floatBits(float x) noexcept
{
    uint32_t b; std::memcpy(&b, &x, sizeof b); return b;
}

inline bool isFinite(float x) noexcept
{
    return (floatBits(x) & 0x7F800000u) != 0x7F800000u;
}

inline bool isFinite(double x) noexcept
{
    uint64_t b; std::memcpy(&b, &x, sizeof b);
    return (b & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}

inline float sanitize(float x, float fallback = 0.0f) noexcept
{
    return isFinite(x) ? x : fallback;
}

inline float clamp(float x, float lo, float hi) noexcept
{
    // A NaN fails both comparisons; make it land on `lo` deterministically.
    if (!(x >= lo)) return lo;
    return x > hi ? hi : x;
}

inline float clamp01(float x) noexcept { return clamp(x, 0.0f, 1.0f); }

/// Finite and bounded to [-limit, limit]; non-finite -> 0.
inline float guard(float x, float limit = 2.0f) noexcept
{
    if (!isFinite(x)) return 0.0f;
    return x > limit ? limit : (x < -limit ? -limit : x);
}

/// Guards n samples in place. Returns true if any sample was non-finite.
inline bool guardBuffer(float* p, int n, float limit = 2.0f) noexcept
{
    bool hadNonFinite = false;
    for (int i = 0; i < n; ++i)
    {
        const float v = p[i];
        if (!isFinite(v)) { p[i] = 0.0f; hadNonFinite = true; }
        else if (v > limit) p[i] = limit;
        else if (v < -limit) p[i] = -limit;
    }
    return hadNonFinite;
}

/// Zeroes n floats without a memset / bzero / memset_pattern call (see the
/// header comment). RT-safe: no allocation, no library call.
inline void zeroBuffer(float* p, size_t n) noexcept
{
    volatile float source = 0.0f;
    const float z = source;
    for (size_t i = 0; i < n; ++i) p[i] = z;
}

}} // namespace ratpipe::kit
