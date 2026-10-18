#include "engine.hpp"
#include "rtguard.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace rp {

float snapDiscrete(const RatPipeParamDesc& p, float v)
{
    if ((p.flags & RATPIPE_PARAM_DISCRETE) == 0 || p.steps < 2) return v;
    const float span = p.max_value - p.min_value;
    int k = (int) std::lround((v - p.min_value) / span * (float) (p.steps - 1));
    k = std::max(0, std::min(p.steps - 1, k));
    return p.min_value + span * (float) k / (float) (p.steps - 1);
}

float randomParamValue(const RatPipeParamDesc& p, Rng& rng)
{
    const float v = p.min_value + (p.max_value - p.min_value) * (float) rng.uniform();
    return snapDiscrete(p, v);
}

void applyParams(Instance& inst, ParamMode mode, Rng& rng)
{
    const RatPipeDescriptor* d = inst.desc();
    for (uint32_t i = 0; i < d->param_count; ++i)
    {
        const RatPipeParamDesc& p = d->params[i];
        float v = p.default_value;
        switch (mode)
        {
            case ParamMode::Default: break;
            case ParamMode::Min:     v = p.min_value; break;
            case ParamMode::Max:     v = p.max_value; break;
            case ParamMode::Random:  v = randomParamValue(p, rng); break;
            case ParamMode::Extremes: v = rng.chance(0.5) ? p.min_value : p.max_value; break;
        }
        inst.setParam((int) i, v);
    }
}

int raggedBlockSize(uint64_t seed, int64_t idx, int maxBlock)
{
    static const int kSizes[] = { 1, 2, 3, 7, 13, 31, 33, 64, 100, 127, 255, 256, 333, 500, 777, 1000, 1023, 1024 };
    Rng r(seed + (uint64_t) idx * 0x9E3779B97F4A7C15ull);
    int n = kSizes[r.range(0, (int) (sizeof(kSizes) / sizeof(kSizes[0])) - 1)];
    return std::min(n, maxBlock);
}

RunResult runPipe(Instance& inst, const RunSpec& spec)
{
    RunResult res;
    const float kSentinel = 0.31415927f;
    const int G = 16;                                    // guard samples each side
    const int cap = spec.maxBlock;
    std::vector<float> bufL((size_t) (cap + 2 * G)), bufR((size_t) (cap + 2 * G));
    std::vector<RatPipeEvent> evs;
    evs.reserve(4096);

    if (spec.keepAudio) { res.l.assign((size_t) spec.frames, 0.0f); res.r.assign((size_t) spec.frames, 0.0f); }

    const std::vector<TimedEvent> none;
    const std::vector<TimedEvent>& ev = spec.events ? *spec.events : none;
    size_t evPos = 0;

    Rng jit(spec.jitterSeed ^ 0xABCDEF1234567ull);
    const RatPipeDescriptor* d = inst.desc();

    int64_t pos = 0;
    int64_t blockIdx = 0;
    double dcAccL = 0.0, dcAccR = 0.0;
    int64_t dcCount = 0;

    while (pos < spec.frames)
    {
        int n = spec.block > 0 ? spec.block : raggedBlockSize(spec.raggedSeed, blockIdx, spec.maxBlock);
        n = (int) std::min<int64_t>(n, spec.frames - pos);
        n = std::min(n, cap);

        // Events whose frame falls inside this block.
        evs.clear();
        while (evPos < ev.size() && ev[evPos].frame < pos) ++evPos;       // late events (before this block) are dropped
        size_t e = evPos;
        while (e < ev.size() && ev[e].frame < pos + n)
        {
            RatPipeEvent x = ev[e].ev;
            x.sample_offset = (int32_t) (ev[e].frame - pos);
            evs.push_back(x);
            ++e;
        }
        evPos = e;

        if (spec.jitterEveryBlocks > 0 && d->param_count > 0 && (blockIdx % spec.jitterEveryBlocks) == 0)
        {
            const int pi = jit.range(0, (int) d->param_count - 1);
            const RatPipeParamDesc& p = d->params[pi];
            const int how = jit.range(0, 2);
            const float v = how == 0 ? p.min_value : (how == 1 ? p.max_value : randomParamValue(p, jit));
            inst.setParam(pi, v);
        }

        // Output buffers: guards = sentinel; [0,n) = prefill or silence; (n, cap] = sentinel.
        std::fill(bufL.begin(), bufL.end(), kSentinel);
        std::fill(bufR.begin(), bufR.end(), kSentinel);
        float* L = bufL.data() + G;
        float* R = bufR.data() + G;
        for (int i = 0; i < n; ++i)
        {
            L[i] = spec.prefillL ? (*spec.prefillL)[(size_t) (pos + i)] : 0.0f;
            R[i] = spec.prefillR ? (*spec.prefillR)[(size_t) (pos + i)] : 0.0f;
        }

        RatPipeTransport tr;
        std::memset(&tr, 0, sizeof tr);
        tr.struct_size     = (uint32_t) sizeof(RatPipeTransport);
        tr.playing         = 1;
        tr.bpm             = spec.bpm;
        tr.sample_rate     = spec.sr;
        tr.sample_position = pos;
        tr.ticks_per_beat  = 480;
        tr.tick            = (int64_t) ((double) pos * 480.0 * spec.bpm / (60.0 * spec.sr));

        const auto t0 = std::chrono::steady_clock::now();
        rtguard::enter();
        inst.render(L, R, n, evs.data(), (int) evs.size(), &tr);
        rtguard::leave();
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (inst.threw) { res.threw = true; break; }

        const double ratio = dt / ((double) n / spec.sr);
        if (blockIdx >= 2)
        {
            res.worstRatioAny = std::max(res.worstRatioAny, ratio);
            if (n == 64) res.worstRatio64 = std::max(res.worstRatio64, ratio);
        }

        // Guard samples (M10: never write outside [0, n)).
        auto guardOk = [&](const std::vector<float>& b)
        {
            for (int i = 0; i < G; ++i) if (b[(size_t) i] != kSentinel) return false;
            for (int i = G + n; i < (int) b.size(); ++i) if (b[(size_t) i] != kSentinel) return false;
            return true;
        };
        if (! guardOk(bufL) || ! guardOk(bufR))
        {
            res.guardViolated = true;
            res.guardWhere = fmt("block %lld (n=%d) at frame %lld", (long long) blockIdx, n, (long long) pos);
            break;
        }

        for (int i = 0; i < n; ++i)
        {
            const float a = L[i], b = R[i];
            if (! std::isfinite(a) || ! std::isfinite(b))
            {
                res.nonFinite = true;
                res.nonFiniteFrame = pos + i;
                break;
            }
            res.peak = std::max(res.peak, std::max(std::fabs(a), std::fabs(b)));
            if (spec.dcFrom >= 0 && pos + i >= spec.dcFrom) { dcAccL += a; dcAccR += b; ++dcCount; }
            if (spec.keepAudio) { res.l[(size_t) (pos + i)] = a; res.r[(size_t) (pos + i)] = b; }
        }
        if (res.nonFinite) break;

        pos += n;
        ++blockIdx;
    }

    res.blocks = blockIdx;
    if (dcCount > 0) { res.dcL = dcAccL / (double) dcCount; res.dcR = dcAccR / (double) dcCount; }
    return res;
}

} // namespace rp
