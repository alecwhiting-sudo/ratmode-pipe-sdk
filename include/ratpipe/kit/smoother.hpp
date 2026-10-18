// ratpipe kit — smoother.hpp
//
// One-pole parameter smoother. Use it to de-zipper atomics read once per block
// (or per control tick) before they hit a coefficient.
//
//   OnePole s; s.setTime(10.0f, sr); s.snap(initial);
//   per sample: float v = s.process(target);
//
// Guarantees: output always between the last value and the target (never
// overshoots), finite for finite inputs, snaps exactly onto the target once
// within 1e-6 relative so recursive tails never go denormal or stall short.
// No allocation, exp() only in setTime (call from prepare or a control tick,
// not per sample).
#pragma once
#include <cmath>

namespace ratpipe { namespace kit {

class OnePole
{
public:
    /// Time to settle to ~63% (one time constant). ms <= 0 disables smoothing.
    void setTime(float ms, double sampleRate) noexcept
    {
        if (!(ms > 0.0f) || !(sampleRate > 0.0))
        {
            coef_ = 1.0;
            return;
        }
        const double samples = static_cast<double>(ms) * 0.001 * sampleRate;
        coef_ = 1.0 - std::exp(-1.0 / samples);
        if (coef_ > 1.0) coef_ = 1.0;
        if (coef_ < 1.0e-7) coef_ = 1.0e-7;
    }

    /// Set the coefficient directly (0 < c <= 1). 1 = no smoothing.
    void setCoefficient(float c) noexcept
    {
        coef_ = c > 1.0f ? 1.0 : (c < 1.0e-7f ? 1.0e-7 : static_cast<double>(c));
    }

    void snap(float v) noexcept { y_ = v; }

    float process(float target) noexcept
    {
        // double accumulator: a float one stalls short of the target once
        // coef * (target - y) drops below half an ulp of y
        const double t = static_cast<double>(target);
        y_ += coef_ * (t - y_);
        const double d   = t - y_;
        const double eps = 1.0e-6 * (1.0 + (t < 0.0 ? -t : t));
        if (d < eps && d > -eps) y_ = t;
        return static_cast<float>(y_);
    }

    float value() const noexcept { return static_cast<float>(y_); }

private:
    double coef_ = 1.0;
    double y_    = 0.0;
};

}} // namespace ratpipe::kit
