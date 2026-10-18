// ratpipe kit — denormal.hpp
//
// Denormal (subnormal) float handling. Subnormals make recursive filters and
// decaying envelopes 10-100x slower on x86 and leak CPU on the audio thread.
//
//   FlushDenormalsScope   RAII: enables flush-to-zero (+ denormals-are-zero on
//                         x86) for the current thread while alive, restores the
//                         previous control word on exit. Inline intrinsics /
//                         inline asm only — no library import, no syscall.
//                         Use one per render() call.
//   flushTiny(x)          Portable fallback: |x| below ~1e-15 -> 0. Branch-free
//                         on the common path; no libm.
//
// Guarantees: no allocation, no locks, no libm. Safe to construct on the audio
// thread. Unknown architectures compile to a no-op scope (flushTiny still works).
#pragma once
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
  #include <xmmintrin.h>
  #define RATPIPE_KIT_DENORMAL_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
  #define RATPIPE_KIT_DENORMAL_ARM64 1
#endif

namespace ratpipe { namespace kit {

class FlushDenormalsScope
{
public:
    FlushDenormalsScope() noexcept
    {
#if defined(RATPIPE_KIT_DENORMAL_X86)
        saved_ = _mm_getcsr();
        _mm_setcsr(static_cast<unsigned int>(saved_) | 0x8040u);   // FTZ (bit 15) | DAZ (bit 6)
#elif defined(RATPIPE_KIT_DENORMAL_ARM64)
        uint64_t fpcr;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
        saved_ = fpcr;
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));  // FZ
#endif
    }

    ~FlushDenormalsScope() noexcept
    {
#if defined(RATPIPE_KIT_DENORMAL_X86)
        _mm_setcsr(static_cast<unsigned int>(saved_));
#elif defined(RATPIPE_KIT_DENORMAL_ARM64)
        __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_));
#endif
    }

    FlushDenormalsScope(const FlushDenormalsScope&) = delete;
    FlushDenormalsScope& operator=(const FlushDenormalsScope&) = delete;

private:
    uint64_t saved_ = 0;
};

/// Portable denormal flush for a recursive state variable: values whose
/// magnitude is below ~1e-15 become exactly 0.
inline float flushTiny(float x) noexcept
{
    const float ax = x < 0.0f ? -x : x;
    return ax < 1.0e-15f ? 0.0f : x;
}

}} // namespace ratpipe::kit
