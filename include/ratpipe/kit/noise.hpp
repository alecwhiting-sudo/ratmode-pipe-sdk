// ratpipe kit — noise.hpp
//
// Noise sources driven by the deterministic Rng (seed them from the host seed
// for RATPIPE_FLAG_DETERMINISTIC_WITH_SEED).
//
//   WhiteNoise  uniform in [-1, 1)
//   PinkNoise   ~-3 dB/oct approximation (three octave-spaced one-poles + a
//               white term), bounded |y| <= 1
//   BrownNoise  leaky integration of white, bounded |y| <= 1
//
// Guarantees: bounded as stated, finite, no allocation, no libm, no <random>.
#pragma once
#include "rng.hpp"

namespace ratpipe { namespace kit {

class WhiteNoise
{
public:
    void seed(uint64_t s) noexcept { rng_.seed(s); }
    float process() noexcept { return rng_.nextBipolar(); }
private:
    Rng rng_;
};

class PinkNoise
{
public:
    void seed(uint64_t s) noexcept { rng_.seed(s); reset(); }
    void reset() noexcept { b0_ = b1_ = b2_ = 0.0f; }

    float process() noexcept
    {
        const float w = rng_.nextBipolar();
        b0_ += 0.0032f * (w - b0_);
        b1_ += 0.0260f * (w - b1_);
        b2_ += 0.1800f * (w - b2_);
        // weights sum to 1 and every term is bounded by 1 -> |y| <= 1
        return 0.38f * b0_ + 0.28f * b1_ + 0.22f * b2_ + 0.12f * w;
    }

private:
    Rng rng_;
    float b0_ = 0.0f, b1_ = 0.0f, b2_ = 0.0f;
};

class BrownNoise
{
public:
    void seed(uint64_t s) noexcept { rng_.seed(s); reset(); }
    void reset() noexcept { y_ = 0.0f; }

    float process() noexcept
    {
        // y = 0.998*y + 0.02*w  ->  |y| <= 0.02 / 0.002 = 10, scaled by 0.1
        y_ = 0.998f * y_ + 0.02f * rng_.nextBipolar();
        return 0.1f * y_;
    }

private:
    Rng rng_;
    float y_ = 0.0f;
};

}} // namespace ratpipe::kit
