// ratpipe kit — svf.hpp
//
// Topology-preserving-transform (zero-delay feedback) state-variable filter,
// trapezoidal integrators. One `set()` yields LP / BP / HP / notch / peak
// outputs from the same state, 12 dB/oct.
//
//   Svf f; f.prepare(sr); f.set(1200.0f, 0.4f);      // cutoff Hz, resonance 0..1
//   per sample: f.process(x); float lp = f.lp();   // or f.processLp(x)
//
// Guarantees
//   - Cutoff is clamped to [5 Hz, 0.45 * sampleRate] (below Nyquist).
//   - Resonance 0..1 maps to damping k = 2 - 1.96 * res, i.e. Q in [0.5, 25];
//     k never reaches 0 so the filter is never lossless.
//   - The TPT structure is stable under per-sample coefficient modulation; the
//     linear gain at resonance is bounded by Q (<= 25), so output is bounded by
//     25 * max|x| in Linear mode. In Bounded mode (setBounded(true)) the input
//     and both integrator states pass through tanh each sample (|x|, |state|
//     <= 4, hence |lp| <= 16, |bp| <= 8, |hp| <= 40 for ANY input, including
//     1e6 spikes) — use this when the input can be hot or when the SVF sits
//     inside another feedback loop.
//   - States are checked for non-finite at the end of every process() and
//     reset to 0 if they ever become non-finite, so a bad input cannot leave
//     the filter dead or screaming.
//   - No allocation; tan() only in set() (call at control rate, not per sample).
#pragma once
#include <cmath>
#include "finite_guard.hpp"

namespace ratpipe { namespace kit {

class Svf
{
public:
    void prepare(double sampleRate) noexcept
    {
        sr_ = sampleRate > 1000.0 ? sampleRate : 44100.0;
        reset();
        set(cutoffHz_, res_);
    }

    void reset() noexcept { ic1_ = ic2_ = 0.0f; v1_ = v2_ = v0_ = 0.0f; }

    void setBounded(bool b) noexcept { bounded_ = b; }

    /// cutoff in Hz (clamped), resonance 0..1.
    void set(float cutoffHz, float res) noexcept
    {
        cutoffHz_ = isFinite(cutoffHz) ? cutoffHz : 1000.0f;
        res_ = clamp01(res);
        const float maxHz = static_cast<float>(0.45 * sr_);
        const float fc = clamp(cutoffHz_, 5.0f, maxHz);
        g_ = static_cast<float>(std::tan(3.141592653589793 * static_cast<double>(fc) / sr_));
        k_ = 2.0f - 1.96f * res_;
        a1_ = 1.0f / (1.0f + g_ * (g_ + k_));
        a2_ = g_ * a1_;
        a3_ = g_ * a2_;
    }

    /// Advance one sample; read the outputs with lp()/bp()/hp()/notch()/peak().
    void process(float x) noexcept
    {
        if (!isFinite(x)) x = 0.0f;
        if (bounded_) x = 4.0f * std::tanh(x * 0.25f);
        v0_ = x;
        const float v3 = x - ic2_;
        v1_ = a1_ * ic1_ + a2_ * v3;
        v2_ = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1_ - ic1_;
        ic2_ = 2.0f * v2_ - ic2_;
        if (bounded_)
        {
            ic1_ = 4.0f * std::tanh(ic1_ * 0.25f);
            ic2_ = 4.0f * std::tanh(ic2_ * 0.25f);
        }
        if (!isFinite(ic1_) || !isFinite(ic2_)) reset();
    }

    float processLp(float x) noexcept { process(x); return lp(); }
    float processBp(float x) noexcept { process(x); return bp(); }
    float processHp(float x) noexcept { process(x); return hp(); }

    float lp() const noexcept    { return v2_; }
    float bp() const noexcept    { return v1_; }
    float hp() const noexcept    { return v0_ - k_ * v1_ - v2_; }
    float notch() const noexcept { return v0_ - k_ * v1_; }
    float peak() const noexcept  { return lp() - hp(); }

    float cutoff() const noexcept { return cutoffHz_; }
    float resonance() const noexcept { return res_; }

private:
    double sr_ = 44100.0;
    float cutoffHz_ = 1000.0f, res_ = 0.0f;
    float g_ = 0.0f, k_ = 2.0f, a1_ = 1.0f, a2_ = 0.0f, a3_ = 0.0f;
    float ic1_ = 0.0f, ic2_ = 0.0f;
    float v0_ = 0.0f, v1_ = 0.0f, v2_ = 0.0f;
    bool bounded_ = false;
};

}} // namespace ratpipe::kit
