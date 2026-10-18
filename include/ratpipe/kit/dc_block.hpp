// ratpipe kit — dc_block.hpp
//
// First-order DC blocker (one-pole high-pass), default corner ~8 Hz. Put one
// after any stage that can accumulate offset: asymmetric saturation, FM with a
// DC term, integrated triangle oscillators, delay feedback loops.
//
// Guarantees: |y| <= 2 * max|x| (worst case for a first-order HP), finite for
// finite input, no allocation. exp() only in prepare().
#pragma once
#include <cmath>

namespace ratpipe { namespace kit {

class DcBlock
{
public:
    void prepare(double sampleRate, float cornerHz = 8.0f) noexcept
    {
        if (!(sampleRate > 0.0)) sampleRate = 44100.0;
        if (!(cornerHz > 0.1f)) cornerHz = 0.1f;
        const double w = 6.283185307179586 * static_cast<double>(cornerHz) / sampleRate;
        r_ = static_cast<float>(std::exp(-w));
        if (r_ > 0.99999f) r_ = 0.99999f;
        if (r_ < 0.0f) r_ = 0.0f;
        reset();
    }

    void reset() noexcept { x1_ = 0.0f; y1_ = 0.0f; }

    float process(float x) noexcept
    {
        const float y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }

private:
    float r_  = 0.9989f;
    float x1_ = 0.0f;
    float y1_ = 0.0f;
};

}} // namespace ratpipe::kit
