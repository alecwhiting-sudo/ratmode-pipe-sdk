// Offline render driver: feeds a timed event list to a pipe block by block,
// with guard samples around the buffers, finite / peak accounting, timing and
// the real-time guard (V5 / V6) wrapped around every render call.
#pragma once
#include "patterns.hpp"
#include "plugin.hpp"
#include "util.hpp"
#include <cstdint>
#include <vector>

namespace rp {

enum class ParamMode { Default, Min, Max, Random, Extremes };

// Sets every parameter. Discrete params snap to their evenly spaced positions.
void applyParams(Instance& inst, ParamMode mode, Rng& rng);
float randomParamValue(const RatPipeParamDesc& p, Rng& rng);
float snapDiscrete(const RatPipeParamDesc& p, float v);

struct RunSpec
{
    double sr      = 48000.0;
    int    block   = 256;          // 0 = ragged (deterministic pseudo-random sizes <= maxBlock)
    int    maxBlock = 1024;
    double bpm     = 130.0;
    const std::vector<TimedEvent>* events = nullptr;
    int64_t frames = 0;            // total frames to render
    bool   keepAudio = false;      // fill RunResult::l / r
    uint64_t raggedSeed = 1;

    // Parameter jitter while rendering (stress): every `jitterEveryBlocks`
    // blocks one random parameter gets a random value.
    int      jitterEveryBlocks = 0;
    uint64_t jitterSeed = 0;

    // DC measurement window [dcFrom, frames); skipped when dcFrom < 0.
    int64_t dcFrom = -1;

    // Optional prefill of the output buffers (M10 additivity); size >= frames. null = zeros.
    const std::vector<float>* prefillL = nullptr;
    const std::vector<float>* prefillR = nullptr;
};

struct RunResult
{
    std::vector<float> l, r;
    bool    nonFinite = false;
    int64_t nonFiniteFrame = -1;
    float   peak = 0.0f;
    bool    guardViolated = false;     // wrote outside [0, n)
    std::string guardWhere;
    bool    threw = false;             // exception escaped render
    double  worstRatio64 = 0.0;        // worst (render time / block budget) over n == 64 blocks
    double  worstRatioAny = 0.0;
    double  dcL = 0.0, dcR = 0.0;      // mean over the DC window
    int64_t blocks = 0;
};

RunResult runPipe(Instance& inst, const RunSpec& spec);

// Sizes used for "ragged" blocks, in order.
int raggedBlockSize(uint64_t seed, int64_t blockIndex, int maxBlock);

} // namespace rp
