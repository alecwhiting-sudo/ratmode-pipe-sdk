// ratpipe kit — pitch.hpp
//
// Pitch helpers and a slide/legato glide.
//
//   midiToHz(note)            A4 = 440 Hz; note may be fractional.
//   hzToMidi(hz)
//   semitonesToRatio(st)
//   Glide                     one-pole glide in the semitone domain (constant
//                             time regardless of interval), 303-style:
//                               g.prepare(sr); g.setTime(60.0f);
//                               on NOTE_ON: g.noteOn(note, ev.slide);
//                               per sample: float hz = g.tickHz();
//
// Guarantees: midiToHz is clamped to [0.1 Hz, 40 kHz] for any input (NaN -> A4
// fallback), so an oscillator fed from here can never see a non-finite or
// absurd frequency. No allocation; exp2/pow only via the pitch conversions
// (one call per sample in Glide::tickHz — cheap enough for mono; cache per
// control tick for polyphony).
#pragma once
#include <cmath>
#include "finite_guard.hpp"

namespace ratpipe { namespace kit {

inline float midiToHz(float note) noexcept
{
    if (!isFinite(note)) return 440.0f;
    if (note < -36.0f) note = -36.0f;
    if (note > 150.0f) note = 150.0f;
    const float hz = 440.0f * std::exp2((note - 69.0f) * (1.0f / 12.0f));
    return clamp(hz, 0.1f, 40000.0f);
}

inline float hzToMidi(float hz) noexcept
{
    if (!(hz > 0.0f) || !isFinite(hz)) return 69.0f;
    return 69.0f + 12.0f * std::log2(hz * (1.0f / 440.0f));
}

inline float semitonesToRatio(float st) noexcept
{
    return std::exp2(clamp(st, -120.0f, 120.0f) * (1.0f / 12.0f));
}

class Glide
{
public:
    void prepare(double sampleRate) noexcept
    {
        sr_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        setTime(timeMs_);
    }

    /// Glide time constant in ms (time to cover ~63% of the interval).
    void setTime(float ms) noexcept
    {
        timeMs_ = ms;
        if (!(ms > 0.0f)) { coef_ = 1.0; return; }
        coef_ = 1.0 - std::exp(-1.0 / (static_cast<double>(ms) * 0.001 * sr_));
        if (coef_ > 1.0) coef_ = 1.0;
        if (coef_ < 1.0e-7) coef_ = 1.0e-7;
    }

    /// slide == true glides from the current pitch; false jumps immediately.
    void noteOn(float note, bool slide) noexcept
    {
        target_ = static_cast<double>(clamp(note, -36.0f, 150.0f));
        if (!slide || !started_) current_ = target_;
        started_ = true;
    }

    /// Set the pitch directly with no glide (e.g. on FLUSH / ALL_OFF).
    void snap(float note) noexcept { target_ = current_ = static_cast<double>(clamp(note, -36.0f, 150.0f)); started_ = true; }

    /// Advance one sample; returns the current note (fractional MIDI). State is
    /// double so the glide always lands exactly on the target (a float
    /// accumulator stalls ~0.01 st short at long glide times).
    float tickNote() noexcept
    {
        current_ += coef_ * (target_ - current_);
        const double d = target_ - current_;
        if (d < 1.0e-4 && d > -1.0e-4) current_ = target_;
        return static_cast<float>(current_);
    }

    float tickHz() noexcept { return midiToHz(tickNote()); }
    float currentNote() const noexcept { return static_cast<float>(current_); }
    float targetNote() const noexcept { return static_cast<float>(target_); }
    bool isGliding() const noexcept { return current_ != target_; }

private:
    double sr_      = 44100.0;
    float  timeMs_  = 60.0f;
    double coef_    = 1.0;
    double current_ = 45.0;
    double target_  = 45.0;
    bool   started_ = false;
};

}} // namespace ratpipe::kit
