// ratpipe kit — osc_polyblep.hpp
//
// Band-limited oscillator (contract M5): saw, pulse/square and triangle use
// 2-sample PolyBLEP residuals at every discontinuity; sine is a plain
// std::sin; SubOsc is a sine one or two octaves below a master frequency.
// Everything is lookup-table free, so nothing ends up as constant data in
// __TEXT (keeps RatMode's raw-syscall byte scan quiet).
//
//   Osc o; o.prepare(sr); o.setWave(Osc::Saw); o.setFrequency(110.0f);
//   per sample: float s = o.process();     // in roughly [-1, 1]
//
// Guarantees
//   - Frequency is clamped so the phase increment is in [0, 0.49] (below
//     Nyquist); a NaN/negative/absurd frequency becomes 0 Hz (DC-free silence
//     for saw/pulse, held value for tri).
//   - |output| <= 1.1 for every waveform at every frequency (PolyBLEP residual
//     overshoot included). Triangle is a leaky integration of the band-limited
//     pulse with a frequency-proportional leak, so it cannot drift.
//   - No allocation, no state outside the object, finite for all inputs.
//   - Phase and increments are double; audio output is float.
#pragma once
#include <cmath>
#include "finite_guard.hpp"

namespace ratpipe { namespace kit {

/// PolyBLEP residual for a unit step (+2, i.e. -1 -> +1) at phase 0, evaluated at
/// phase t in [0,1) with increment dt. Zero outside the 2-sample window.
inline double polyBlepResidual(double t, double dt) noexcept
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

class Osc
{
public:
    enum Wave { Saw = 0, Pulse = 1, Square = 1, Tri = 2, Sine = 3 };

    void prepare(double sampleRate) noexcept
    {
        sr_ = sampleRate > 1000.0 ? sampleRate : 44100.0;
        setFrequency(hz_);
    }

    void reset(double phase01 = 0.0) noexcept
    {
        phase_ = phase01 - std::floor(phase01);
        tri_   = -1.0;
    }

    void setWave(Wave w) noexcept { wave_ = w; }
    Wave wave() const noexcept { return wave_; }

    /// Pulse width for Pulse/Square, clamped to [0.05, 0.95].
    void setPulseWidth(float w) noexcept { width_ = static_cast<double>(clamp(w, 0.05f, 0.95f)); }

    void setFrequency(float hz) noexcept
    {
        hz_ = isFinite(hz) && hz > 0.0f ? hz : 0.0f;
        inc_ = static_cast<double>(hz_) / sr_;
        if (inc_ > 0.49) inc_ = 0.49;
    }

    float frequency() const noexcept { return hz_; }
    double phase() const noexcept { return phase_; }

    float process() noexcept
    {
        double out;
        switch (wave_)
        {
            case Saw:   out = sawSample(); break;
            case Pulse: out = pulseSample(); break;
            case Tri:   out = triSample(); break;
            default:    out = std::sin(6.283185307179586 * phase_); break;
        }
        advance();
        return static_cast<float>(out);
    }

    /// Hard sync: resets the phase at the next sample boundary, no BLEP on the
    /// sync edge (acceptable for sub/bass use; expect some alias on sync).
    void sync() noexcept { phase_ = 0.0; }

private:
    double sawSample() const noexcept
    {
        return (2.0 * phase_ - 1.0) - polyBlepResidual(phase_, inc_ > 1.0e-9 ? inc_ : 1.0e-9);
    }

    double pulseSample() const noexcept
    {
        const double dt = inc_ > 1.0e-9 ? inc_ : 1.0e-9;
        double v = phase_ < width_ ? 1.0 : -1.0;
        v += polyBlepResidual(phase_, dt);                // rising edge at 0
        double t2 = phase_ - width_;
        if (t2 < 0.0) t2 += 1.0;
        v -= polyBlepResidual(t2, dt);                    // falling edge at width
        return v;
    }

    double triSample() noexcept
    {
        if (inc_ < 1.0e-7) return tri_;                   // no frequency: hold, never integrate DC
        const double sq   = pulseSample();
        const double leak = 1.0 - 0.125 * inc_;           // time constant = 8 periods
        tri_ = tri_ * leak + 4.0 * inc_ * sq;
        if (tri_ > 1.1) tri_ = 1.1; else if (tri_ < -1.1) tri_ = -1.1;
        return tri_;
    }

    void advance() noexcept
    {
        phase_ += inc_;
        if (phase_ >= 1.0) phase_ -= 1.0;
    }

    double sr_    = 44100.0;
    double phase_ = 0.0;
    double inc_   = 0.0;
    double width_ = 0.5;
    double tri_   = -1.0;
    float  hz_    = 0.0f;
    Wave   wave_  = Saw;
};

/// Sine sub-oscillator locked `octavesDown` (1 or 2) below the frequency you
/// pass each call. Bounded to [-1, 1].
class SubOsc
{
public:
    void prepare(double sampleRate) noexcept { sr_ = sampleRate > 1000.0 ? sampleRate : 44100.0; }
    void reset() noexcept { phase_ = 0.0; }
    void setOctavesDown(int n) noexcept { div_ = n >= 2 ? 0.25 : (n == 1 ? 0.5 : 1.0); }

    float process(float masterHz) noexcept
    {
        double inc = (isFinite(masterHz) && masterHz > 0.0f ? static_cast<double>(masterHz) : 0.0) * div_ / sr_;
        if (inc > 0.49) inc = 0.49;
        const float out = static_cast<float>(std::sin(6.283185307179586 * phase_));
        phase_ += inc;
        if (phase_ >= 1.0) phase_ -= 1.0;
        return out;
    }

private:
    double sr_    = 44100.0;
    double phase_ = 0.0;
    double div_   = 0.5;
};

}} // namespace ratpipe::kit
