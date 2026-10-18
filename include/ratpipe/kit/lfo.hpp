// ratpipe kit — lfo.hpp
//
// Low-frequency oscillator: sine, triangle, saw (ramp), square, sample-and-
// hold and smoothed random. Bipolar output in [-1, 1]; unipolar() gives [0,1].
//
//   Lfo l; l.prepare(sr); l.setWave(Lfo::Tri); l.setRate(4.0f);
//   per sample (or per control tick with tick(32)): float m = l.process();
//   Transport sync: l.setPhase(fractional beat position).
//
// Guarantees: output in [-1, 1] always, finite, rate clamped to [0, sr/4]
// (0 = frozen), setPhase() sanitises non-finite input, a non-finite phase
// state resets to 0, deterministic given the seed passed to seed(). No
// allocation, no libm except std::sin / std::cos for Sine / RandomSmooth.
#pragma once
#include <cmath>
#include "finite_guard.hpp"
#include "rng.hpp"

namespace ratpipe { namespace kit {

class Lfo
{
public:
    enum Wave { Sine = 0, Tri, Saw, Square, SampleHold, RandomSmooth };

    void prepare(double sampleRate) noexcept
    {
        sr_ = sampleRate > 1000.0 ? sampleRate : 44100.0;
        setRate(rateHz_);
    }

    void seed(uint64_t s) noexcept { rng_.seed(s); }
    void setWave(Wave w) noexcept { wave_ = w; }

    void setRate(float hz) noexcept
    {
        rateHz_ = isFinite(hz) && hz > 0.0f ? hz : 0.0f;
        inc_ = static_cast<double>(rateHz_) / sr_;
        if (inc_ > 0.25) inc_ = 0.25;
    }

    /// Set the phase (0..1) directly, e.g. from the transport. Wraps any finite
    /// value; a non-finite value resets the phase to 0.
    void setPhase(double phase01) noexcept
    {
        phase_ = isFinite(phase01) ? phase01 - std::floor(phase01) : 0.0;
        if (!(phase_ >= 0.0 && phase_ < 1.0)) phase_ = 0.0;      // 1.0 can appear from rounding of tiny negatives
    }
    void reset() noexcept { phase_ = 0.0; held_ = next_ = 0.0f; }

    /// Advance by `samples` samples (1 per sample, or the control-rate count).
    float tick(int samples = 1) noexcept
    {
        const double step = inc_ * static_cast<double>(samples > 0 ? samples : 1);
        if (!isFinite(phase_)) phase_ = 0.0;
        float out = valueAt();
        phase_ += step;
        if (phase_ >= 1.0)
        {
            phase_ -= std::floor(phase_);
            held_ = next_;
            next_ = rng_.nextBipolar();
        }
        return out;
    }

    float process() noexcept { return tick(1); }

    /// Current value mapped to [0, 1].
    float unipolar() const noexcept { return 0.5f * (valueAt() + 1.0f); }

    float rate() const noexcept { return rateHz_; }

private:
    float valueAt() const noexcept
    {
        const double p = phase_;
        switch (wave_)
        {
            case Sine:   return static_cast<float>(std::sin(6.283185307179586 * p));
            case Tri:    return static_cast<float>(p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p);
            case Saw:    return static_cast<float>(2.0 * p - 1.0);
            case Square: return p < 0.5 ? 1.0f : -1.0f;
            case SampleHold: return held_;
            case RandomSmooth:
            {
                // raised-cosine blend between the held and upcoming random values
                const float t = static_cast<float>(0.5 - 0.5 * std::cos(3.141592653589793 * p));
                return held_ + (next_ - held_) * t;
            }
            default: return 0.0f;
        }
    }

    double sr_ = 44100.0, phase_ = 0.0, inc_ = 0.0;
    float rateHz_ = 1.0f, held_ = 0.0f, next_ = 0.0f;
    Wave wave_ = Sine;
    Rng rng_;
};

}} // namespace ratpipe::kit
