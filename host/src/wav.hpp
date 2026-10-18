// Tiny WAV writer: 16-bit / 24-bit PCM or 32-bit float, stereo.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rp {

enum class WavFormat { Pcm16, Pcm24, Float32 };

inline bool writeWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R,
                     double sampleRate, WavFormat fmt, std::string& err)
{
    const size_t n = L.size() < R.size() ? L.size() : R.size();
    const int bytes = fmt == WavFormat::Pcm16 ? 2 : (fmt == WavFormat::Pcm24 ? 3 : 4);
    const uint32_t dataBytes = (uint32_t) (n * 2 * (size_t) bytes);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) { err = "cannot open " + path + " for writing"; return false; }

    auto w32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) std::fputc((int) ((v >> (8 * i)) & 0xFF), f); };
    auto w16 = [&](uint16_t v) { std::fputc(v & 0xFF, f); std::fputc((v >> 8) & 0xFF, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16);
    w16(fmt == WavFormat::Float32 ? 3 : 1); w16(2);
    w32((uint32_t) std::lround(sampleRate)); w32((uint32_t) std::lround(sampleRate) * 2u * (uint32_t) bytes);
    w16((uint16_t) (2 * bytes)); w16((uint16_t) (bytes * 8));
    std::fwrite("data", 1, 4, f); w32(dataBytes);

    auto put = [&](float x)
    {
        if (! std::isfinite(x)) x = 0.0f;
        if (fmt == WavFormat::Float32)
        {
            uint32_t u; static_assert(sizeof(u) == sizeof(x), "float size");
            std::memcpy(&u, &x, 4); w32(u);
            return;
        }
        if (x > 1.0f) x = 1.0f;
        if (x < -1.0f) x = -1.0f;
        if (fmt == WavFormat::Pcm16)
        {
            int v = (int) std::lround(x * 32767.0f);
            w16((uint16_t) (int16_t) v);
        }
        else
        {
            int v = (int) std::lround(x * 8388607.0f);
            std::fputc(v & 0xFF, f); std::fputc((v >> 8) & 0xFF, f); std::fputc((v >> 16) & 0xFF, f);
        }
    };
    for (size_t i = 0; i < n; ++i) { put(L[i]); put(R[i]); }
    const bool ok = std::fflush(f) == 0;
    std::fclose(f);
    if (! ok) { err = "write error on " + path; return false; }
    return true;
}

} // namespace rp
