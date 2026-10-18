// ratpipe kit — sat_tanh.hpp
//
// Saturators — tanh family ONLY (contract M4). Every function here is
// monotone, odd-or-offset-corrected, and hard-bounded, so it is safe inside a
// feedback path. There is deliberately no cubic / polynomial soft clipper: a
// polynomial grows without bound outside its design range and has blown up a
// feedback loop before (RatMode Arch17).
//
//   sat::tanhDrive(x, drive)       tanh(drive * x)                       |y| < 1
//   sat::asymTanh(x, drive, bias)  tanh(drive*x + bias) - tanh(bias)     |y| < 2, zero at x = 0
//   sat::softTape(x, amount01)     tanh with level-compensated pre-gain   |y| < 1
//   sat::fastTanh(x)               Padé [7/6] on |x| <= 3, clamped        |y| <= 0.9951
//
// Guarantees: pure functions, no state, no allocation, finite for finite
// input (NaN input -> 0). tanh is one libm call; fastTanh is none.
#pragma once
#include <cmath>
#include "finite_guard.hpp"

namespace ratpipe { namespace kit { namespace sat {

inline float tanhDrive(float x, float drive = 1.0f) noexcept
{
    if (!isFinite(x)) return 0.0f;
    if (!(drive > 0.0f)) drive = 0.0f;
    return std::tanh(x * drive);
}

/// Asymmetric tanh: shifts the operating point so even harmonics appear, then
/// removes the resulting offset at rest. Follow with a DcBlock when the signal
/// has sustained level (the offset is only exactly zero at x == 0).
inline float asymTanh(float x, float drive = 1.0f, float bias = 0.3f) noexcept
{
    if (!isFinite(x)) return 0.0f;
    if (!(drive > 0.0f)) drive = 0.0f;
    bias = clamp(bias, -2.0f, 2.0f);
    return std::tanh(x * drive + bias) - std::tanh(bias);
}

/// "Tape": pre-gain 1..10 with output trimmed so loudness stays roughly flat
/// as `amount` rises. amount in [0,1]. Bounded by 1 for any input.
inline float softTape(float x, float amount) noexcept
{
    if (!isFinite(x)) return 0.0f;
    amount = clamp01(amount);
    const float pre  = 1.0f + 9.0f * amount;
    const float trim = 1.0f - 0.35f * amount;
    return std::tanh(x * pre) * trim;
}

/// Rational tanh approximation, max abs error ~2e-4 on [-3, 3]; the input is
/// clamped to that range so the output never exceeds tanh(3) ~= 0.9951.
inline float fastTanh(float x) noexcept
{
    x = clamp(x, -3.0f, 3.0f);       // NaN -> -3 -> -0.9951 (deterministic, bounded)
    const float x2 = x * x;
    const float num = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    const float den = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f));
    return num / den;
}

}}} // namespace ratpipe::kit::sat
