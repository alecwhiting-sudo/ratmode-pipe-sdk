// ratpipe kit — rng.hpp
//
// Seeded, deterministic pseudo-random generator (PCG32, 64-bit state, 32-bit
// output). The same seed always yields the same sequence on every platform
// and compiler — this is what makes RATPIPE_FLAG_DETERMINISTIC_WITH_SEED
// honest: seed it from RatPipeHostInfo::seed in create() and never from time.
//
// Guarantees: POD, no allocation, no libm, no <random> (std::random_device is
// a denied import in RatMode's binary check). Never produces NaN. nextFloat()
// is in [0, 1), nextBipolar() in [-1, 1).
#pragma once
#include <cstdint>

namespace ratpipe { namespace kit {

class Rng
{
public:
    Rng() noexcept { seed(0x853c49e6748fea9bull); }
    explicit Rng(uint64_t s) noexcept { seed(s); }

    void seed(uint64_t s, uint64_t stream = 0xda3e39cb94b95bdbull) noexcept
    {
        inc_   = (stream << 1u) | 1u;
        state_ = 0u;
        nextU32();
        state_ += s;
        nextU32();
    }

    uint32_t nextU32() noexcept
    {
        const uint64_t old = state_;
        state_ = old * 6364136223846793005ull + inc_;
        const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }

    /// Uniform in [0, 1) with 24 bits of precision.
    float nextFloat() noexcept
    {
        return static_cast<float>(nextU32() >> 8) * (1.0f / 16777216.0f);
    }

    /// Uniform in [-1, 1).
    float nextBipolar() noexcept { return nextFloat() * 2.0f - 1.0f; }

    /// Uniform integer in [0, n) for n > 0 (slightly biased for huge n; fine for audio).
    uint32_t nextBelow(uint32_t n) noexcept
    {
        return n == 0 ? 0u : static_cast<uint32_t>((static_cast<uint64_t>(nextU32()) * n) >> 32);
    }

private:
    uint64_t state_ = 0;
    uint64_t inc_   = 0;
};

}} // namespace ratpipe::kit
