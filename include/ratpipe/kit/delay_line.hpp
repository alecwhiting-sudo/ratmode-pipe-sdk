// ratpipe kit — delay_line.hpp
//
// Fixed-maximum delay line and a feedback delay built on it. This is the ONE
// kit block that allocates: prepare(maxSamples) sizes the buffer (call it from
// your pipe's constructor / prepare(), never from render). Everything else is
// RT-safe.
//
//   DelayLine d; d.prepare(static_cast<int>(sr * 2.0));   // 2 s maximum
//   per sample: float y = d.read(delaySamples); d.write(x);
//
//   FeedbackDelay fd; fd.prepare(maxSamples);
//   per sample: float y = fd.process(x, delaySamples, feedback, damping01);
//
// Guarantees
//   - Delay time is clamped to [1, max-2] samples; fractional reads are
//     linearly interpolated.
//   - Feedback is clamped to |fb| <= 0.95 (contract M6), so the loop gain is
//     < 1 and |y| <= 20 * max|x|. There is no non-linearity in the loop; if you
//     add one, use kit::sat (tanh family) only.
//   - Damping is a one-pole low-pass in the loop (convex, cannot add gain).
//   - A non-finite value entering the buffer clears the whole line (memset on
//     the audio thread, bounded by the prepared size; this only ever happens
//     after an upstream bug).
//   - read()/write() before prepare() are no-ops returning 0.
#pragma once
#include <cstring>
#include <new>
#include <vector>
#include "finite_guard.hpp"

namespace ratpipe { namespace kit {

class DelayLine
{
public:
    /// Allocates. maxSamples is clamped to [4, 1<<24].
    void prepare(int maxSamples)
    {
        if (maxSamples < 4) maxSamples = 4;
        if (maxSamples > (1 << 24)) maxSamples = 1 << 24;
        buf_.assign(static_cast<size_t>(maxSamples), 0.0f);
        size_ = maxSamples;
        w_ = 0;
    }

    void clear() noexcept
    {
        if (size_ > 0) std::memset(buf_.data(), 0, sizeof(float) * static_cast<size_t>(size_));
        w_ = 0;
    }

    int maxDelay() const noexcept { return size_ > 2 ? size_ - 2 : 0; }

    void write(float x) noexcept
    {
        if (size_ == 0) return;
        if (!isFinite(x)) { clear(); return; }
        buf_[static_cast<size_t>(w_)] = x;
        if (++w_ >= size_) w_ = 0;
    }

    /// Linear-interpolated read `delaySamples` behind the write head.
    float read(float delaySamples) const noexcept
    {
        if (size_ == 0) return 0.0f;
        const float d = clamp(delaySamples, 1.0f, static_cast<float>(size_ - 2));
        const int   di = static_cast<int>(d);
        const float fr = d - static_cast<float>(di);
        int i0 = w_ - di;      if (i0 < 0) i0 += size_;
        int i1 = i0 - 1;       if (i1 < 0) i1 += size_;
        const float a = buf_[static_cast<size_t>(i0)];
        const float b = buf_[static_cast<size_t>(i1)];
        return a + (b - a) * fr;
    }

    float readInt(int delaySamples) const noexcept
    {
        if (size_ == 0) return 0.0f;
        if (delaySamples < 1) delaySamples = 1;
        if (delaySamples > size_ - 1) delaySamples = size_ - 1;
        int i = w_ - delaySamples; if (i < 0) i += size_;
        return buf_[static_cast<size_t>(i)];
    }

private:
    std::vector<float> buf_;
    int size_ = 0;
    int w_    = 0;
};

class FeedbackDelay
{
public:
    static constexpr float kMaxFeedback = 0.95f;

    void prepare(int maxSamples) { line_.prepare(maxSamples); lp_ = 0.0f; }
    void clear() noexcept { line_.clear(); lp_ = 0.0f; }
    int maxDelay() const noexcept { return line_.maxDelay(); }

    /// Returns the delayed (wet) signal; mix with the dry signal yourself.
    float process(float x, float delaySamples, float feedback, float damping01 = 0.0f) noexcept
    {
        if (!isFinite(x)) x = 0.0f;
        const float fb = clamp(feedback, -kMaxFeedback, kMaxFeedback);
        const float y  = line_.read(delaySamples);
        // one-pole damping in the loop: coefficient in [0.02, 1], convex
        const float c  = 1.0f - 0.98f * clamp01(damping01);
        lp_ += c * (y - lp_);
        line_.write(x + fb * lp_);
        return y;
    }

private:
    DelayLine line_;
    float lp_ = 0.0f;
};

}} // namespace ratpipe::kit
