// ratpipe kit — ladder_lite.hpp
//
// Four-pole "ladder" low-pass with a tanh-bounded feedback loop (24 dB/oct, a
// 12 dB tap is available). Cheap: two tanh per sample, four one-pole stages.
//
//   LadderLite f; f.prepare(sr); f.set(800.0f, 0.6f, 1.0f);  // Hz, res 0..1, drive
//   per sample: float y = f.process(x);
//
// Guarantees — this is the kit's "cannot blow up" filter:
//   - The loop input is u = tanh(drive*x - k*tanh(s4)), so |u| < 1 always.
//   - Each stage is s += g*(in - s) with g in (0, 0.95]: a convex combination,
//     so |s_i| <= 1 for all time, for ANY per-sample cutoff/resonance trajectory
//     and ANY input (including NaN, which is replaced by 0). No polynomial
//     anywhere in the loop (contract M4).
//   - Output = s4 * makeup, makeup = 1 + 0.5*k <= 3, so |y| <= 3.
//   - Cutoff clamped to [5 Hz, 0.45 * sampleRate]; resonance 0..1 -> k 0..4
//     (self-oscillation begins just below 1).
//   - No allocation; exp() only in set().
#pragma once
#include <cmath>
#include "finite_guard.hpp"

namespace ratpipe { namespace kit {

class LadderLite
{
public:
    void prepare(double sampleRate) noexcept
    {
        sr_ = sampleRate > 1000.0 ? sampleRate : 44100.0;
        reset();
        set(cutoffHz_, res_, drive_);
    }

    void reset() noexcept { s1_ = s2_ = s3_ = s4_ = 0.0f; }

    void set(float cutoffHz, float res, float drive = 1.0f) noexcept
    {
        cutoffHz_ = isFinite(cutoffHz) ? cutoffHz : 1000.0f;
        res_   = clamp01(res);
        drive_ = clamp(drive, 0.0f, 16.0f);
        const float fc = clamp(cutoffHz_, 5.0f, static_cast<float>(0.45 * sr_));
        g_ = static_cast<float>(1.0 - std::exp(-6.283185307179586 * static_cast<double>(fc) / sr_));
        if (g_ > 0.95f) g_ = 0.95f;
        if (g_ < 1.0e-6f) g_ = 1.0e-6f;
        k_ = 4.0f * res_;
        makeup_ = 1.0f + 0.5f * k_;
    }

    float process(float x) noexcept
    {
        if (!isFinite(x)) x = 0.0f;
        const float u = std::tanh(x * drive_ - k_ * std::tanh(s4_));
        s1_ += g_ * (u   - s1_);
        s2_ += g_ * (s1_ - s2_);
        s3_ += g_ * (s2_ - s3_);
        s4_ += g_ * (s3_ - s4_);
        return s4_ * makeup_;
    }

    /// 12 dB/oct tap from the same loop (after the last process()).
    float lp12() const noexcept { return s2_ * makeup_; }

    float cutoff() const noexcept { return cutoffHz_; }
    float resonance() const noexcept { return res_; }

private:
    double sr_ = 44100.0;
    float cutoffHz_ = 1000.0f, res_ = 0.0f, drive_ = 1.0f;
    float g_ = 0.1f, k_ = 0.0f, makeup_ = 1.0f;
    float s1_ = 0.0f, s2_ = 0.0f, s3_ = 0.0f, s4_ = 0.0f;
};

}} // namespace ratpipe::kit
