// ratpipe kit — adsr.hpp
//
// Exponential ADSR envelope with a snappy (overshoot-target) attack.
//
//   Adsr e; e.prepare(sr); e.set(2.0f, 300.0f, 0.5f, 80.0f);  // ms, ms, level, ms
//   NOTE_ON: e.gateOn();   NOTE_OFF / RELEASE_HELD: e.gateOff();   FLUSH: e.kill();
//   per sample: float a = e.process();   // 0..1
//
// Time semantics: attack reaches 1.0 in ~attackMs; decay and release are the
// time to fall 60 dB (so "decay 400 ms" is inaudible after ~400 ms).
// Percussive one-shot: set sustain to 0 and call gateOn() per hit.
//
// Guarantees: output in [0, 1], finite, monotone within a stage; gateOn()
// restarts from the current level (no click); isActive() goes false once the
// level is below 1e-5 so voices can be skipped. No allocation; exp() only in
// set()/prepare().
#pragma once
#include <cmath>
#include "finite_guard.hpp"

namespace ratpipe { namespace kit {

class Adsr
{
public:
    enum Stage { Idle = 0, Attack, Decay, Sustain, Release };

    void prepare(double sampleRate) noexcept
    {
        sr_ = sampleRate > 1000.0 ? sampleRate : 44100.0;
        set(aMs_, dMs_, sus_, rMs_);
    }

    void set(float attackMs, float decayMs, float sustain01, float releaseMs) noexcept
    {
        aMs_ = clamp(attackMs,  0.05f, 20000.0f);
        dMs_ = clamp(decayMs,   0.5f,  60000.0f);
        rMs_ = clamp(releaseMs, 0.5f,  60000.0f);
        sus_ = clamp01(sustain01);
        aCoef_ = coefFor(aMs_, 1.4663f);   // ln(1.3 / 0.3): reach 1.0 aiming at 1.3
        dCoef_ = coefFor(dMs_, 6.9078f);   // ln(1000): -60 dB
        rCoef_ = coefFor(rMs_, 6.9078f);
    }

    void gateOn() noexcept  { stage_ = Attack; }
    void gateOff() noexcept { if (stage_ != Idle) stage_ = Release; }
    void kill() noexcept    { stage_ = Idle; y_ = 0.0f; }

    bool isActive() const noexcept { return stage_ != Idle; }
    Stage stage() const noexcept { return stage_; }
    float value() const noexcept { return y_; }

    float process() noexcept
    {
        switch (stage_)
        {
            case Attack:
                y_ += aCoef_ * (1.3f - y_);
                if (y_ >= 1.0f) { y_ = 1.0f; stage_ = Decay; }
                break;
            case Decay:
                y_ += dCoef_ * (sus_ - y_);
                if (y_ - sus_ < 1.0e-4f)
                {
                    y_ = sus_;
                    stage_ = sus_ > 1.0e-5f ? Sustain : Idle;
                    if (stage_ == Idle) y_ = 0.0f;
                }
                break;
            case Sustain:
                y_ = sus_;
                break;
            case Release:
                y_ -= rCoef_ * y_;
                if (y_ < 1.0e-5f) { y_ = 0.0f; stage_ = Idle; }
                break;
            default:
                y_ = 0.0f;
                break;
        }
        return y_;
    }

private:
    float coefFor(float ms, float logRatio) const noexcept
    {
        const double samples = static_cast<double>(ms) * 0.001 * sr_;
        float c = static_cast<float>(1.0 - std::exp(-static_cast<double>(logRatio) / samples));
        return clamp(c, 1.0e-7f, 1.0f);
    }

    double sr_ = 44100.0;
    float aMs_ = 2.0f, dMs_ = 300.0f, sus_ = 0.7f, rMs_ = 100.0f;
    float aCoef_ = 0.1f, dCoef_ = 0.001f, rCoef_ = 0.001f;
    float y_ = 0.0f;
    Stage stage_ = Idle;
};

}} // namespace ratpipe::kit
