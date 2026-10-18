#include "validator.hpp"
#include "engine.hpp"
#include "exports.hpp"
#include "json.hpp"
#include "manifest.hpp"
#include "patterns.hpp"
#include "plugin.hpp"
#include "rtguard.hpp"
#include "util.hpp"
#include <algorithm>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <thread>

namespace rp {

bool ValidationReport::failed() const
{
    for (const auto& c : checks) if (c.status == CheckResult::Fail) return true;
    return false;
}
bool ValidationReport::hasWarnings() const
{
    for (const auto& c : checks) if (c.status == CheckResult::Warn) return true;
    return false;
}

namespace {

constexpr double kBpm = 130.0;
constexpr int    kMaxBlock = 1024;

struct Ctx
{
    const Plugin&          pl;
    const ValidateOptions& opt;
    const RatPipeDescriptor* d;
    Pattern primary;
    bool deterministic;
};

bool wanted(const ValidateOptions& o, const char* id) { return o.only.empty() || o.only.count(id) != 0; }

Pattern primaryPattern(int kind) { return kind == RATPIPE_DRUM ? Pattern::Drum16 : Pattern::Acid16; }

bool slurp(const std::string& p, std::string& out)
{
    std::ifstream f(p, std::ios::binary);
    if (! f) return false;
    std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}

std::string baseName(const std::string& p) { const size_t s = p.rfind('/'); return s == std::string::npos ? p : p.substr(s + 1); }

// Capped failure list: keeps the report readable when every run fails.
struct Failures
{
    std::vector<std::string> list;
    int total = 0;
    void add(const std::string& m) { ++total; if (list.size() < 8) list.push_back(m); }
    void flush(CheckResult& r, const char* what)
    {
        for (const auto& m : list) r.fail(m);
        if (total > (int) list.size()) r.fail(fmt("... and %d more failing %s", total - (int) list.size(), what));
    }
};

// ── V4 single render ─────────────────────────────────────────────────────────

struct Fuzz1
{
    std::string label;
    double sr = 48000.0;
    int block = 256;
    Pattern pat = Pattern::Drum16;
    ParamMode pm = ParamMode::Default;
    uint64_t seed = 1;
    int bars = 2;
    bool jitter = false;
    bool tail = true;
};

struct Fuzz1Result { bool ok = true; float peak = 0; double worst64 = 0; };

Fuzz1Result runFuzz1(Ctx& c, const Fuzz1& f, Failures& fails)
{
    Fuzz1Result out;
    Instance inst(c.pl, f.sr, kMaxBlock, c.opt.hostSeed);
    if (! inst.ok()) { fails.add(f.label + ": create() returned NULL"); out.ok = false; return out; }
    inst.prepare(f.sr, kMaxBlock);
    Rng rng(f.seed * 7919u + 13u);
    applyParams(inst, f.pm, rng);

    const auto evs = makePattern(f.pat, c.d->part_kind, f.sr, kBpm, f.bars, f.seed);
    RunSpec s;
    s.sr = f.sr; s.block = f.block; s.maxBlock = kMaxBlock; s.bpm = kBpm; s.events = &evs;
    const int64_t patLen = (int64_t) f.bars * barFrames(f.sr, kBpm);
    s.frames = patLen + (f.tail ? (int64_t) (2.0 * f.sr) : 0);
    s.raggedSeed = f.seed;
    if (f.jitter) { s.jitterEveryBlocks = 3; s.jitterSeed = f.seed; }
    if (f.tail && patternReleasesAll(f.pat)) s.dcFrom = s.frames - (int64_t) f.sr;   // last 1 s of >= 2 s with no events

    const RunResult r = runPipe(inst, s);
    out.peak = r.peak; out.worst64 = r.worstRatio64;
    if (r.threw || inst.threw) { fails.add(f.label + ": a C++ exception escaped across the ABI"); out.ok = false; }
    if (r.nonFinite)  { fails.add(f.label + fmt(": non-finite sample at frame %lld (%.3f s)", (long long) r.nonFiniteFrame, (double) r.nonFiniteFrame / f.sr)); out.ok = false; }
    if (r.guardViolated) { fails.add(f.label + ": render wrote outside [0, n) (" + r.guardWhere + ")"); out.ok = false; }
    if (! r.nonFinite && r.peak > 2.0f) { fails.add(f.label + fmt(": peak %.3f exceeds 2.0 (%.1f dBFS)", r.peak, toDb(r.peak))); out.ok = false; }
    if (s.dcFrom >= 0)
    {
        const double dc = std::max(std::fabs(r.dcL), std::fabs(r.dcR));
        if (dc > 0.01) { fails.add(f.label + fmt(": DC offset %.1f dB after 1 s without events (limit -40 dB)", toDb(dc))); out.ok = false; }
    }
    return out;
}

std::string describe(const char* tag, double sr, int block, Pattern p, ParamMode pm)
{
    static const char* pmn[] = { "default", "min", "max", "random", "extremes" };
    return fmt("%s %.0f Hz, block %s, %s, params %s", tag, sr,
               block == 0 ? "ragged" : std::to_string(block).c_str(), patternName(p), pmn[(int) pm]);
}

// ── V1 ───────────────────────────────────────────────────────────────────────

void checkExports(Ctx& c, CheckResult& v1, const ValidateOptions& o)
{
    (void) o;
    std::vector<std::string> names; std::string err;
    if (! listExportedSymbols(c.pl.binary, names, err))
    {
        v1.warn("could not inspect exported symbols (" + err + ")");
        return;
    }
    std::set<std::string> extra; bool entry = false;
    for (const auto& n0 : names)
    {
        std::string n = n0;
        if (! n.empty() && n[0] == '_' && n != "_init" && n != "_fini" && n != "_edata" && n != "_end")
        {
           #if defined(__APPLE__)
            n = n.substr(1);            // Mach-O C symbols carry a leading underscore
           #endif
        }
        if (n == RATPIPE_ENTRY_SYMBOL) { entry = true; continue; }
        if (isToolchainSymbol(n0) || isToolchainSymbol(n)) continue;
        extra.insert(n);
    }
    if (! entry) v1.fail("library does not export ratpipe_entry");
    if (! extra.empty())
    {
        std::string l;
        int k = 0;
        for (const auto& n : extra) { if (k++ < 6) l += (l.empty() ? "" : ", ") + n; }
        v1.fail(fmt("%zu extra exported symbol(s): ", extra.size()) + l + (extra.size() > 6 ? ", ..." : "")
                + " (only ratpipe_entry may be exported: -fvisibility=hidden / version script; contract M12)");
    }
}

// ── V4 ───────────────────────────────────────────────────────────────────────

void checkV4(Ctx& c, CheckResult& r)
{
    Failures fails;
    int runs = 0;
    float peak = 0;
    const ValidateOptions& o = c.opt;
    auto run = [&](Fuzz1 f)
    {
        ++runs;
        const Fuzz1Result x = runFuzz1(c, f, fails);
        peak = std::max(peak, x.peak);
    };

    // A: sample rates x block sizes at default parameters, primary pattern.
    const std::vector<double> rates = o.quick ? std::vector<double>{ 48000.0 } : std::vector<double>{ 44100.0, 48000.0, 96000.0 };
    const int blocks[] = { 32, 64, 128, 256, 512, 1024, 0 };
    for (double sr : rates)
        for (int b : blocks)
        {
            Fuzz1 f; f.sr = sr; f.block = b; f.pat = c.primary; f.pm = ParamMode::Default; f.seed = (uint64_t) b + 1;
            f.label = describe("grid", sr, b, f.pat, f.pm);
            run(f);
        }

    // B: every pattern x {default, min, max}.
    for (Pattern p : { Pattern::Drum16, Pattern::Acid16, Pattern::Chords, Pattern::Silence })
        for (ParamMode pm : { ParamMode::Default, ParamMode::Min, ParamMode::Max })
            for (int b : o.quick ? std::vector<int>{ 0 } : std::vector<int>{ 0, 256 })
            {
                Fuzz1 f; f.sr = 48000.0; f.block = b; f.pat = p; f.pm = pm; f.seed = 99;
                f.label = describe("pattern", f.sr, b, p, pm);
                run(f);
            }

    // C: seeded fuzz — random rate / block / pattern / parameters, with live parameter jitter.
    const int seeds = o.quick ? std::min(o.seeds, 20) : o.seeds;
    static const double kRates[] = { 44100.0, 48000.0, 96000.0 };
    static const int    kBlocks[] = { 32, 64, 128, 256, 512, 1024, 0, 0 };
    for (int s = 0; s < seeds; ++s)
    {
        Rng r(1000u + (uint64_t) s);
        Fuzz1 f;
        f.sr = kRates[r.range(0, 2)];
        f.block = kBlocks[r.range(0, 7)];
        const int pk = r.range(0, 4);
        f.pat = pk <= 1 ? Pattern::Fuzz : (pk == 2 ? c.primary : (pk == 3 ? Pattern::Chords : Pattern::Drum16));
        f.pm = (s % 2) ? ParamMode::Random : ParamMode::Extremes;
        f.seed = 5000u + (uint64_t) s;
        f.bars = 2;
        f.jitter = (s % 3) != 0;
        f.tail = false;
        f.label = describe(fmt("seed %d:", s).c_str(), f.sr, f.block, f.pat, f.pm) + (f.jitter ? ", live param jitter" : "");
        run(f);
    }

    // D: sample-rate change through prepare().
    {
        ++runs;
        Instance inst(c.pl, 44100.0, kMaxBlock, o.hostSeed);
        if (! inst.ok()) fails.add("prepare path: create() returned NULL");
        else
        {
            inst.prepare(96000.0, kMaxBlock);
            const auto evs = makePattern(c.primary, c.d->part_kind, 96000.0, kBpm, 2, 3);
            RunSpec s; s.sr = 96000.0; s.block = 0; s.maxBlock = kMaxBlock; s.bpm = kBpm; s.events = &evs;
            s.frames = 2 * barFrames(96000.0, kBpm);
            const RunResult res = runPipe(inst, s);
            if (res.nonFinite || res.peak > 2.0f || res.threw || res.guardViolated)
                fails.add("create@44.1k then prepare(96k): " + std::string(res.nonFinite ? "non-finite output" : res.threw ? "exception escaped" : res.guardViolated ? "out-of-bounds write" : fmt("peak %.2f", res.peak)));
        }
    }

    // E: M10 additive — silent pattern into prefilled buffers must leave them untouched.
    {
        ++runs;
        const double sr = 48000.0;
        Instance inst(c.pl, sr, kMaxBlock, o.hostSeed);
        if (inst.ok())
        {
            const int64_t frames = 8192;
            std::vector<float> pre((size_t) frames);
            Rng rr(42);
            for (auto& v : pre) v = (float) (rr.uniform() - 0.5);
            RunSpec s; s.sr = sr; s.block = 256; s.maxBlock = kMaxBlock; s.frames = frames; s.keepAudio = true;
            s.prefillL = &pre; s.prefillR = &pre;
            const RunResult res = runPipe(inst, s);
            for (int64_t i = 0; i < frames && ! res.nonFinite; ++i)
                if (std::fabs(res.l[(size_t) i] - pre[(size_t) i]) > 1e-6f || std::fabs(res.r[(size_t) i] - pre[(size_t) i]) > 1e-6f)
                {
                    fails.add(fmt("render is not additive (M10): an idle pipe changed the buffer at frame %lld (it must ADD into L/R, never clear or overwrite)", (long long) i));
                    break;
                }
        }
    }

    // F: set_param from another thread while render runs (M11). Races are only
    // visible under ThreadSanitizer; without it this still catches crashes.
    {
        ++runs;
        const double sr = 48000.0;
        Instance inst(c.pl, sr, kMaxBlock, o.hostSeed);
        if (inst.ok() && c.d->param_count > 0)
        {
            std::atomic<bool> stop{ false };
            std::thread t([&]
            {
                Rng r(77);
                while (! stop.load())
                {
                    const int i = r.range(0, (int) c.d->param_count - 1);
                    inst.setParam(i, randomParamValue(c.d->params[i], r));
                    std::this_thread::sleep_for(std::chrono::microseconds(40));
                }
            });
            const auto evs = makePattern(c.primary, c.d->part_kind, sr, kBpm, 2, 7);
            RunSpec s; s.sr = sr; s.block = 64; s.maxBlock = kMaxBlock; s.bpm = kBpm; s.events = &evs;
            s.frames = 2 * barFrames(sr, kBpm);
            const RunResult res = runPipe(inst, s);
            stop = true; t.join();
            if (res.nonFinite || res.peak > 2.0f || res.threw || res.guardViolated)
                fails.add("set_param from a second thread during render: " + std::string(res.nonFinite ? "non-finite output" : res.threw ? "exception escaped" : res.guardViolated ? "out-of-bounds write" : fmt("peak %.2f", res.peak)));
        }
    }

    fails.flush(r, "renders");
    if (r.status != CheckResult::Fail)
    {
        r.status = CheckResult::Pass;
        r.summary = fmt("%d renders finite, peak %.2f (limit 2.0), no DC, no out-of-bounds writes", runs, peak);
    }
}

// ── V7 ───────────────────────────────────────────────────────────────────────

void checkV7(Ctx& c, CheckResult& r)
{
    const double slack = c.opt.timingSlack;
    const double entryMs = c.pl.entrySeconds * 1000.0;
    if (entryMs > 5.0 * slack) r.fail(fmt("ratpipe_entry took %.2f ms (limit 5 ms): keep it cheap and side-effect free (M15)", entryMs));

    const double sr = 48000.0;
    const int attempts = c.opt.quick ? 2 : 3;
    double best = 1e9;
    for (int a = 0; a < attempts; ++a)
    {
        double worst = 0.0;
        for (ParamMode pm : { ParamMode::Default, ParamMode::Max })
        {
            Instance inst(c.pl, sr, kMaxBlock, c.opt.hostSeed);
            if (! inst.ok()) { r.fail("create() returned NULL"); return; }
            Rng rng(5);
            applyParams(inst, pm, rng);
            const auto evs = makePattern(c.primary, c.d->part_kind, sr, kBpm, 2, 11);
            RunSpec s; s.sr = sr; s.block = 64; s.maxBlock = kMaxBlock; s.bpm = kBpm; s.events = &evs;
            s.frames = 2 * barFrames(sr, kBpm);
            const RunResult res = runPipe(inst, s);
            worst = std::max(worst, res.worstRatio64);
        }
        best = std::min(best, worst);
    }
    const double pct = best * 100.0 / slack;
    if (pct > 50.0) r.fail(fmt("worst 64-sample block used %.0f%% of its real-time budget (limit 50%%)", pct));
    else if (pct > 25.0) r.warn(fmt("worst 64-sample block used %.0f%% of its real-time budget (warn above 25%%)", pct));
    if (r.status != CheckResult::Fail && r.status != CheckResult::Warn)
    {
        r.status = CheckResult::Pass;
        r.summary = fmt("worst 64-sample block %.1f%% of budget; ratpipe_entry %.3f ms", pct, entryMs);
    }
}

// ── V8 ───────────────────────────────────────────────────────────────────────

void checkV8(Ctx& c, CheckResult& r)
{
    Instance inst(c.pl, 48000.0, kMaxBlock, c.opt.hostSeed);
    if (! inst.ok()) { r.fail("create() returned NULL"); return; }
    const RatPipeDescriptor* d = c.d;
    auto close = [](float a, float b) { return std::fabs(a - b) <= 1e-5f + 1e-5f * std::fabs(b); };
    int bad = 0;
    for (uint32_t i = 0; i < d->param_count && bad < 8; ++i)
    {
        const RatPipeParamDesc& p = d->params[i];
        const float g0 = inst.getParam((int) i);
        if (! close(g0, p.default_value)) { r.fail(fmt("'%s': get_param right after create = %g, descriptor default = %g", p.id, g0, p.default_value)); ++bad; }
        std::vector<float> vals = { p.min_value, p.max_value, 0.5f * (p.min_value + p.max_value), p.default_value };
        if (p.flags & RATPIPE_PARAM_DISCRETE) for (auto& v : vals) v = snapDiscrete(p, v);
        for (float v : vals)
        {
            inst.setParam((int) i, v);
            const float g = inst.getParam((int) i);
            if (! close(g, v)) { r.fail(fmt("'%s': set_param(%g) then get_param = %g (round trip must be exact, contract M2)", p.id, v, g)); ++bad; break; }
        }
    }
    if (inst.threw) r.fail("a C++ exception escaped set_param / get_param");

    // manifest <-> descriptor
    if (! c.pl.bundleDir.empty())
    {
        std::string text;
        if (slurp(c.pl.bundleDir + "/Contents/Resources/manifest.json", text))
        {
            Json m; std::string perr;
            if (! parseJson(text, m, perr)) r.fail("manifest.json does not parse: " + perr);
            else for (const auto& dsc : diffManifest(m, d)) r.fail(dsc + " (regenerate the manifest: it is built from the descriptor)");
        }
    }
    if (r.status != CheckResult::Fail)
    {
        r.status = CheckResult::Pass;
        r.summary = fmt("%u params round-trip exactly (min / max / mid / default)%s", d->param_count,
                        c.pl.bundleDir.empty() ? "" : "; manifest matches descriptor");
    }
}

// ── V9 / V10 / V11 ───────────────────────────────────────────────────────────

std::vector<double> windowRms(const std::vector<float>& x, int64_t from, int win)
{
    std::vector<double> out;
    for (int64_t s = from; s + win <= (int64_t) x.size(); s += win)
    {
        double a = 0;
        for (int i = 0; i < win; ++i) a += (double) x[(size_t) (s + i)] * x[(size_t) (s + i)];
        out.push_back(std::sqrt(a / win));
    }
    return out;
}

void checkV9(Ctx& c, CheckResult& r)
{
    const double sr = 48000.0;
    const int bars = 4;
    const auto evs = makePattern(c.primary, c.d->part_kind, sr, kBpm, bars, 21);
    const int64_t frames = (int64_t) bars * barFrames(sr, kBpm);

    // B: fresh instance, defaults.
    Instance b(c.pl, sr, kMaxBlock, c.opt.hostSeed);
    // A: dirtied instance.
    Instance a(c.pl, sr, kMaxBlock, c.opt.hostSeed);
    if (! a.ok() || ! b.ok()) { r.fail("create() returned NULL"); return; }

    RunSpec s; s.sr = sr; s.block = 256; s.maxBlock = kMaxBlock; s.bpm = kBpm; s.events = &evs; s.frames = frames; s.keepAudio = true;

    Rng rng(1234);
    applyParams(a, ParamMode::Random, rng);
    const RunResult dirty = runPipe(a, s);
    if (dirty.nonFinite || dirty.threw) { r.fail("instance A produced non-finite output / threw with randomised parameters (see V4)"); return; }

    // "Song switch": all notes off + flush, then every parameter back to its default.
    std::vector<TimedEvent> ctl = { makeControl(RATPIPE_EV_ALL_OFF, 0), makeControl(RATPIPE_EV_RELEASE_HELD, 0), makeControl(RATPIPE_EV_FLUSH, 0) };
    RunSpec fs; fs.sr = sr; fs.block = 256; fs.maxBlock = kMaxBlock; fs.events = &ctl; fs.frames = 256;
    runPipe(a, fs);
    Rng unused(0);
    applyParams(a, ParamMode::Default, unused);
    applyParams(b, ParamMode::Default, unused);

    const RunResult ra = runPipe(a, s);
    const RunResult rb = runPipe(b, s);
    if (ra.nonFinite || rb.nonFinite) { r.fail("non-finite output while comparing A and B"); return; }

    const int64_t skip = (int64_t) (0.1 * sr);
    if (c.deterministic)
    {
        double worst = 0; int64_t at = -1;
        for (int64_t i = skip; i < frames; ++i)
        {
            const double dl = std::fabs((double) ra.l[(size_t) i] - rb.l[(size_t) i]);
            const double dr = std::fabs((double) ra.r[(size_t) i] - rb.r[(size_t) i]);
            const double dm = std::max(dl, dr);
            if (dm > worst) { worst = dm; at = i; }
        }
        if (worst > 1e-3)
            r.fail(fmt("a dirtied instance differs from a fresh one by %.1f dBFS at %.3f s after ALL_OFF + FLUSH + default params (limit -60 dB): "
                       "hidden state that parameters do not own (contract M7)", toDb(worst), (double) at / sr));
        else { r.status = CheckResult::Pass; r.summary = fmt("dirtied vs fresh instance: max difference %.1f dBFS (sample-exact comparison, limit -60)", toDb(worst)); }
        return;
    }

    // Free-running noise is allowed when the pipe does not declare DETERMINISTIC_WITH_SEED:
    // compare 100 ms RMS envelopes instead of samples. Two bounds, both must hold:
    //   * ratio: the two RMS levels agree within 1 dB (each clamped to a -60 dBFS floor so
    //     a window that is silent in both does not divide by zero);
    //   * absolute: in any window where either instance is quieter than -40 dBFS,
    //     |rmsA - rmsB| <= 1e-3 (-60 dBFS), so the floor clamp cannot hide a tail that one
    //     instance has and the other does not (e.g. a -38 dBFS ring a dirtied instance keeps
    //     after FLUSH). In windows where both are louder than -40 dBFS the ratio governs:
    //     free-running oscillator phase alone moves a 100 ms RMS by a few tenths of a dB,
    //     which is far more than 1e-3 absolute at -10 dBFS and is not hidden state.
    // A pipe whose tail rings differently with oscillator phase should reset that phase on
    // FLUSH / ALL_OFF (it is state the song does not own) or declare DETERMINISTIC_WITH_SEED
    // and get the sample-exact comparison; the validator does not relax for it.
    const int win = (int) (0.1 * sr);   // long enough that oscillator phase cannot move the RMS
    const double fl = 1e-3, absLimit = 1e-3, absWhenBelow = 0.01, dbLimit = 1.0;
    double worstDb = 0, worstAbs = 0, worstA = 0, worstB = 0; int64_t worstAt = -1, worstAbsAt = -1;
    double worstAbsA = 0, worstAbsB = 0;
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto ea = windowRms(ch ? ra.r : ra.l, skip, win);
        const auto eb = windowRms(ch ? rb.r : rb.l, skip, win);
        for (size_t i = 0; i < ea.size() && i < eb.size(); ++i)
        {
            const double ad = std::fabs(ea[i] - eb[i]);
            if (std::min(ea[i], eb[i]) < absWhenBelow && ad > worstAbs) { worstAbs = ad; worstAbsAt = skip + (int64_t) i * win; worstAbsA = ea[i]; worstAbsB = eb[i]; }
            if (ea[i] < fl && eb[i] < fl) continue;
            const double db = std::fabs(toDb(std::max(ea[i], fl)) - toDb(std::max(eb[i], fl)));
            if (db > worstDb) { worstDb = db; worstAt = skip + (int64_t) i * win; worstA = ea[i]; worstB = eb[i]; }
        }
    }
    if (worstDb > dbLimit)
        r.fail(fmt("a dirtied instance's output envelope differs from a fresh one by %.1f dB at %.3f s (dirtied %.1f dBFS RMS, fresh %.1f dBFS RMS) "
                   "after ALL_OFF + FLUSH + default params (limit %.0f dB in 100 ms windows): hidden state that parameters do not own (contract M7)",
                   worstDb, (double) worstAt / sr, toDb(worstA), toDb(worstB), dbLimit));
    if (worstAbs > absLimit)
        r.fail(fmt("a dirtied instance's output level differs from a fresh one by %.1f dBFS RMS at %.3f s (dirtied %.1f dBFS, fresh %.1f dBFS) "
                   "after ALL_OFF + FLUSH + default params (limit -60 dBFS absolute where either side is below -40 dBFS): a tail or offset the song does not own (contract M7)",
                   toDb(worstAbs), (double) worstAbsAt / sr, toDb(worstAbsA), toDb(worstAbsB)));
    if (r.status != CheckResult::Fail)
    {
        r.status = CheckResult::Pass;
        r.summary = fmt("dirtied vs fresh instance: envelopes agree within %.2f dB and %.1f dBFS absolute (100 ms RMS windows; pipe is not DETERMINISTIC_WITH_SEED)", worstDb, toDb(worstAbs));
    }
}

void checkV10(Ctx& c, CheckResult& r)
{
    const double sr = 48000.0;
    double worstTail = 0;
    for (ParamMode pm : { ParamMode::Default, ParamMode::Max })
    {
        Instance inst(c.pl, sr, kMaxBlock, c.opt.hostSeed);
        if (! inst.ok()) { r.fail("create() returned NULL"); return; }
        Rng rng(3);
        applyParams(inst, pm, rng);
        auto evs = makePattern(c.primary, c.d->part_kind, sr, kBpm, 1, 5);
        const auto hold = makePattern(Pattern::Hold, c.d->part_kind, sr, kBpm, 1, 5);
        evs.insert(evs.end(), hold.begin(), hold.end());                    // a note that is never released
        std::stable_sort(evs.begin(), evs.end(), [](const TimedEvent& x, const TimedEvent& y) { return x.frame < y.frame; });
        const int64_t kill = (int64_t) (0.35 * sr);
        evs.push_back(makeControl(RATPIPE_EV_ALL_OFF, kill));
        evs.push_back(makeControl(RATPIPE_EV_FLUSH, kill));
        std::stable_sort(evs.begin(), evs.end(), [](const TimedEvent& x, const TimedEvent& y) { return x.frame < y.frame; });
        // drop everything after the kill so only the tail is measured
        evs.erase(std::remove_if(evs.begin(), evs.end(), [&](const TimedEvent& e) { return e.frame > kill; }), evs.end());

        RunSpec s; s.sr = sr; s.block = 128; s.maxBlock = kMaxBlock; s.bpm = kBpm; s.events = &evs;
        s.frames = kill + (int64_t) (2.5 * sr); s.keepAudio = true;
        const RunResult res = runPipe(inst, s);
        if (res.nonFinite) { r.fail("non-finite output while measuring the tail"); return; }
        for (int64_t i = kill + (int64_t) (2.0 * sr); i < s.frames; ++i)
            worstTail = std::max<double>(worstTail, std::max(std::fabs(res.l[(size_t) i]), std::fabs(res.r[(size_t) i])));
        if (worstTail > 1e-4)
        {
            r.fail(fmt("output is still %.1f dBFS between 2.0 s and 2.5 s after ALL_OFF + FLUSH with %s parameters (limit -80 dBFS): droning voice or tail the flush does not clear",
                       toDb(worstTail), pm == ParamMode::Default ? "default" : "maximum"));
            return;
        }
    }
    r.status = CheckResult::Pass;
    r.summary = "after ALL_OFF + FLUSH (note held; default and max params) output is below -80 dBFS within 2 s (worst "
              + (worstTail < 1e-9 ? std::string("exactly silent") : fmt("%.0f dBFS", toDb(worstTail))) + ")";
}

void checkV11(Ctx& c, CheckResult& r)
{
    if (! c.deterministic)
    {
        r.skip("pipe does not declare RATPIPE_FLAG_DETERMINISTIC_WITH_SEED (free-running noise is allowed)");
        return;
    }
    const double sr = 48000.0;
    std::vector<float> outs[2];
    for (int k = 0; k < 2; ++k)
    {
        Instance inst(c.pl, sr, kMaxBlock, c.opt.hostSeed);
        if (! inst.ok()) { r.fail("create() returned NULL"); return; }
        Rng rng(8);
        applyParams(inst, ParamMode::Random, rng);
        const auto evs = makePattern(Pattern::Fuzz, c.d->part_kind, sr, kBpm, 2, 31);
        RunSpec s; s.sr = sr; s.block = 0; s.raggedSeed = 9; s.maxBlock = kMaxBlock; s.bpm = kBpm; s.events = &evs;
        s.frames = 2 * barFrames(sr, kBpm); s.keepAudio = true;
        const RunResult res = runPipe(inst, s);
        if (res.nonFinite) { r.fail("non-finite output"); return; }
        outs[k] = res.l; outs[k].insert(outs[k].end(), res.r.begin(), res.r.end());
    }
    if (outs[0].size() != outs[1].size() || std::memcmp(outs[0].data(), outs[1].data(), outs[0].size() * sizeof(float)) != 0)
        r.fail("two instances with the same host seed, parameters and events are not bit-identical although DETERMINISTIC_WITH_SEED is declared");
    else { r.status = CheckResult::Pass; r.summary = "same host seed -> bit-identical output twice"; }
}

// ── V12 ──────────────────────────────────────────────────────────────────────

[[maybe_unused]] std::string runCapture(const std::string& cmd)
{
    std::string out;
    if (std::FILE* p = popen(cmd.c_str(), "r"))
    {
        char buf[512];
        while (std::fgets(buf, sizeof buf, p)) out += buf;
        pclose(p);
    }
    return out;
}

std::string plistString(const std::string& plist, const std::string& key)
{
    const size_t k = plist.find("<key>" + key + "</key>");
    if (k == std::string::npos) return "";
    const size_t a = plist.find("<string>", k);
    const size_t b = plist.find("</string>", k);
    if (a == std::string::npos || b == std::string::npos || b < a) return "";
    return plist.substr(a + 8, b - a - 8);
}

void checkV12(Ctx& c, CheckResult& r)
{
    if (c.pl.bundleDir.empty()) { r.skip("not a .ratpipe bundle (raw library given)"); return; }
    const std::string id = c.d->id;
    const std::string dir = c.pl.bundleDir;

    std::string plist;
    if (! slurp(dir + "/Contents/Info.plist", plist)) r.fail("Contents/Info.plist is missing");
    else
    {
        if (plistString(plist, "CFBundleIdentifier") != id)
            r.fail("Info.plist CFBundleIdentifier '" + plistString(plist, "CFBundleIdentifier") + "' != descriptor id '" + id + "' (contract M14)");
        const std::string exe = plistString(plist, "CFBundleExecutable");
        if (exe != baseName(c.pl.binary))
            r.fail("Info.plist CFBundleExecutable '" + exe + "' != '" + baseName(c.pl.binary) + "'");
    }

    std::string text;
    if (! slurp(dir + "/Contents/Resources/manifest.json", text)) r.fail("Contents/Resources/manifest.json is missing");
    else
    {
        Json m; std::string perr;
        if (! parseJson(text, m, perr)) r.fail("manifest.json does not parse: " + perr);
        else
        {
            for (const auto& p : lintManifest(m)) r.fail(p);
            if (m.str("id") != id) r.fail("manifest id '" + m.str("id") + "' != descriptor id '" + id + "'");
        }
    }
    if (baseName(c.pl.binary) != id + ".dylib" && baseName(c.pl.binary) != id + ".so")
        r.warn("binary is named '" + baseName(c.pl.binary) + "'; by convention it is <id>.dylib");

   #if defined(__APPLE__)
    {
        const std::string archs = runCapture("lipo -archs '" + c.pl.binary + "' 2>/dev/null");
        const bool x86 = archs.find("x86_64") != std::string::npos, arm = archs.find("arm64") != std::string::npos;
        if (! x86 || ! arm)
        {
            const std::string m = "binary slices: '" + (archs.empty() ? std::string("?") : archs.substr(0, archs.find('\n'))) + "' — RatMode is universal; ship x86_64 + arm64 (contract M14)";
            if (c.opt.allowSingleArch) r.warn(m); else r.fail(m);
        }
        const std::string libs = runCapture("otool -L '" + c.pl.binary + "' 2>/dev/null");
        std::istringstream ls(libs);
        std::string line; bool first = true;
        while (std::getline(ls, line))
        {
            if (first) { first = false; continue; }                 // header line
            const size_t a = line.find_first_not_of(" \t"), b = line.find(" (");
            if (a == std::string::npos || b == std::string::npos) continue;
            const std::string lib = line.substr(a, b - a);
            if (lib.find("@loader_path") == 0 && lib.find(baseName(c.pl.binary)) != std::string::npos) continue;   // its own install name (LC_ID_DYLIB)
            if (lib != "/usr/lib/libSystem.B.dylib" && lib != "/usr/lib/libc++.1.dylib" && lib != "/usr/lib/libc++abi.dylib")
                r.fail("links disallowed library '" + lib + "' (RatMode loads only libSystem and libc++)");
        }
        if (runCapture("otool -l '" + c.pl.binary + "' 2>/dev/null | grep -c LC_RPATH").find_first_not_of("0 \n") != std::string::npos)
            r.fail("binary has an LC_RPATH load command (RatMode refuses it)");
    }
   #endif

    if (r.status != CheckResult::Fail)
    {
        r.status = CheckResult::Pass;
        r.summary = "Info.plist, manifest.json and descriptor agree";
       #if defined(__APPLE__)
        r.summary += "; universal slices, allowed dylibs, no LC_RPATH";
       #else
        r.info("lipo / otool checks (universal slices, allowed dylibs, LC_RPATH) only run on macOS");
       #endif
    }
}

// ── V13 ──────────────────────────────────────────────────────────────────────

void collectSources(const std::string& dir, std::vector<std::string>& files, int depth = 0)
{
    if (depth > 6) return;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return;
    while (dirent* e = ::readdir(d))
    {
        const std::string n = e->d_name;
        if (n == "." || n == ".." || n[0] == '.') continue;
        const std::string p = dir + "/" + n;
        struct stat st;
        if (::stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { collectSources(p, files, depth + 1); continue; }
        static const char* const exts[] = { ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".mm", ".m" };
        for (const char* x : exts)
        {
            const size_t xl = std::strlen(x);
            if (n.size() > xl && n.compare(n.size() - xl, xl, x) == 0) { files.push_back(p); break; }
        }
    }
    ::closedir(d);
}

void checkV13(Ctx& c, CheckResult& r)
{
    if (c.opt.srcDirs.empty()) { r.skip("no --src directory given"); return; }
    std::vector<std::string> files;
    for (const auto& d : c.opt.srcDirs) collectSources(d, files);
    if (files.empty()) { r.skip("no source files found under the --src directories"); return; }
    struct Rule { std::regex re; const char* why; };
    const std::vector<Rule> rules = {
        { std::regex(R"(\b([A-Za-z_]\w*)\s*\*\s*\1\s*\*\s*\1\b)"), "x*x*x: cubic term (polynomial soft clip?)" },
        { std::regex(R"(\bpowf?\s*\([^,()]+,\s*3(\.0f?)?\s*\))"), "pow(x, 3): cubic term (polynomial soft clip?)" },
        { std::regex(R"(1\.5f?\s*\*\s*\w+\s*-\s*0\.5f?\s*\*)"), "1.5*x - 0.5*x^3 cubic soft-clip idiom" },
        { std::regex(R"(-\s*\w+\s*\*\s*\w+\s*\*\s*\w+\s*/\s*3(\.0f?)?)"), "x - x^3/3 cubic soft-clip idiom" },
    };
    int hits = 0;
    for (const auto& f : files)
    {
        std::ifstream in(f);
        std::string line; int ln = 0;
        while (std::getline(in, line))
        {
            ++ln;
            const size_t s = line.find_first_not_of(" \t");
            if (s == std::string::npos || line.compare(s, 2, "//") == 0 || line[s] == '*') continue;
            const size_t cm = line.find("//");
            const std::string code = cm == std::string::npos ? line : line.substr(0, cm);
            for (const auto& rule : rules)
                if (std::regex_search(code, rule.re))
                {
                    if (hits++ < 12) r.warn(fmt("%s:%d: %s", f.c_str(), ln, rule.why) + " — a cubic/polynomial clipper in a feedback path crashed RatMode before (see docs/CONTRACT.md M4)");
                    break;
                }
        }
    }
    if (hits > 12) r.warn(fmt("... and %d more", hits - 12));
    if (r.status != CheckResult::Warn)
    {
        r.status = CheckResult::Pass;
        r.summary = fmt("%zu source files, no polynomial-clip idioms", files.size());
    }
}

// ── V5 / V6 ──────────────────────────────────────────────────────────────────

void finishRt(CheckResult& v5, CheckResult& v6)
{
    std::string why;
    if (! rtguard::available(why))
    {
        v5.skip("interposition unavailable: " + why);
        v6.skip("interposition unavailable: " + why);
        return;
    }
    const rtguard::Report rep = rtguard::snapshot();
    if (rep.allocs + rep.frees > 0)
        v5.fail(fmt("%u allocation(s) and %u free(s) inside render, first: %s() — allocate in create/prepare, never in render (contract M1)",
                    rep.allocs, rep.frees, rep.first.c_str()));
    else { v5.status = CheckResult::Pass; v5.summary = "no malloc/free/operator new during render (global allocator interposed)"; }
    if (rep.io + rep.locks + rep.sleeps > 0)
    {
        v6.fail(fmt("%u IO, %u lock and %u sleep/yield call(s) inside render%s — render must not block, lock or touch files (contract M1)",
                    rep.io, rep.locks, rep.sleeps, (rep.allocs + rep.frees > 0) ? "" : fmt(", first: %s()", rep.first.c_str()).c_str()));
    }
    else { v6.status = CheckResult::Pass; v6.summary = "no file IO, printf, mutex/cond/semaphore waits or sleeps during render"; }
}

} // namespace

// ── driver ───────────────────────────────────────────────────────────────────

ValidationReport validatePipe(const std::string& path, const ValidateOptions& opt)
{
    ValidationReport rep;
    rep.path = path;
    const std::pair<const char*, const char*> kNames[] = {
        { "V1", "ABI" }, { "V2", "Identity" }, { "V3", "Bounds" }, { "V4", "Render fuzz" }, { "V5", "RT allocation" },
        { "V6", "RT IO / locks" }, { "V7", "Timing" }, { "V8", "Param round-trip" }, { "V9", "Song-switch re-derivation" },
        { "V10", "Silence" }, { "V11", "Determinism" }, { "V12", "Bundle" }, { "V13", "Saturation audit" } };
    for (const auto& n : kNames) { CheckResult c; c.id = n.first; c.name = n.second; rep.checks.push_back(c); }
    auto R = [&](const char* id) -> CheckResult& { for (auto& c : rep.checks) if (c.id == id) return c; return rep.checks[0]; };
    auto note = [&](const std::string& s) { if (opt.progress) opt.progress(s); };
    auto skipRest = [&](const std::string& why)
    {
        for (auto& c : rep.checks) if (c.status == CheckResult::Skip && c.messages.empty()) c.skip(why);
    };

    rtguard::reset();

    Plugin pl;
    note("V1 loading");
    if (! loadPlugin(path, 48000.0, kMaxBlock, opt.hostSeed, pl))
    {
        rep.binary = pl.binary;
        R("V1").fail(pl.error);
        skipRest("pipe did not load (fix V1 first)");
        return rep;
    }
    rep.binary = pl.binary;

    std::vector<Finding> findings;
    const bool safe = lintDescriptor(pl.desc, findings);
    if (pl.desc != nullptr && pl.desc->struct_size >= sizeof(RatPipeDescriptor) && pl.desc->abi_major == RATPIPE_ABI_MAJOR && safe)
    {
        rep.id = pl.desc->id ? pl.desc->id : "";
        rep.name = pl.desc->display_name ? pl.desc->display_name : "";
        rep.kind = partKindName(pl.desc->part_kind);
    }
    for (const auto& f : findings)
    {
        CheckResult& cr = R(f.check.c_str());
        if (f.warning) cr.warn(f.message); else cr.fail(f.message);
    }

    Ctx ctx{ pl, opt, pl.desc, primaryPattern(pl.desc ? pl.desc->part_kind : 0), false };
    if (wanted(opt, "V1")) checkExports(ctx, R("V1"), opt);
    for (const char* id : { "V1", "V2", "V3" })
    {
        CheckResult& cr = R(id);
        if (cr.status == CheckResult::Skip)
        {
            cr.status = CheckResult::Pass;
            if (! wanted(opt, id)) { cr.status = CheckResult::Skip; cr.skip("not selected"); }
        }
    }
    R("V1").summary = "ratpipe_entry exported (only symbol), abi_major " + std::to_string(pl.desc->abi_major) + ", struct sizes match";
    R("V2").summary = "id, name, prefix and all param ids well-formed";
    R("V3").summary = "bounds, defaults, mutate bands and perform knobs valid";
    R("V1").status = (R("V1").status == CheckResult::Skip) ? CheckResult::Skip : R("V1").status;

    if (! safe) { skipRest("descriptor is not safe to run (fix V1-V3 first)"); return rep; }
    // A V1-V3 failure that keeps the descriptor readable still blocks running the pipe:
    // parameter sweeps assume sane bounds.
    for (const char* id : { "V1", "V2", "V3" })
        if (R(id).status == CheckResult::Fail) { skipRest(std::string("fix ") + id + " first (the remaining checks rely on a sane descriptor)"); return rep; }

    ctx.deterministic = (pl.desc->flags & RATPIPE_FLAG_DETERMINISTIC_WITH_SEED) != 0;

    struct Step { const char* id; void (*fn)(Ctx&, CheckResult&); const char* label; };
    const Step steps[] = {
        { "V4",  checkV4,  "V4 render fuzz (sample rates x blocks x patterns x params x seeds)" },
        { "V7",  checkV7,  "V7 timing" },
        { "V8",  checkV8,  "V8 parameter round-trip" },
        { "V9",  checkV9,  "V9 song-switch re-derivation" },
        { "V10", checkV10, "V10 silence after flush" },
        { "V11", checkV11, "V11 determinism" },
        { "V12", checkV12, "V12 bundle" },
        { "V13", checkV13, "V13 saturation audit" },
    };
    for (const Step& st : steps)
    {
        if (! wanted(opt, st.id)) { R(st.id).skip("not selected"); continue; }
        note(st.label);
        st.fn(ctx, R(st.id));
    }
    if (wanted(opt, "V5") || wanted(opt, "V6")) finishRt(R("V5"), R("V6"));
    else { R("V5").skip("not selected"); R("V6").skip("not selected"); }
    return rep;
}

// ── output ───────────────────────────────────────────────────────────────────

std::string reportToText(const ValidationReport& r, bool color)
{
    auto paint = [&](const char* c, const std::string& s) { return color ? std::string("\033[") + c + "m" + s + "\033[0m" : s; };
    std::string o;
    o += "ratpipe-host " + std::string(kHostVersion) + " validating " + r.path + "\n";
    if (! r.id.empty()) o += "  " + r.id + "  \"" + r.name + "\"  (" + r.kind + ")\n";
    for (const auto& c : r.checks)
    {
        std::string st, col;
        switch (c.status) { case CheckResult::Pass: st = "PASS"; col = "32"; break; case CheckResult::Warn: st = "WARN"; col = "33"; break;
                            case CheckResult::Fail: st = "FAIL"; col = "31;1"; break; case CheckResult::Skip: st = "SKIP"; col = "90"; break; }
        o += fmt("%-4s %-26s ", c.id.c_str(), c.name.c_str()) + paint(col.c_str(), st);
        if (c.status == CheckResult::Pass && ! c.summary.empty()) o += "  " + c.summary;
        o += "\n";
        for (const auto& m : c.messages) o += "       " + m + "\n";
    }
    o += r.failed() ? paint("31;1", "RESULT: FAIL") : (r.hasWarnings() ? paint("33", "RESULT: PASS (with warnings)") : paint("32", "RESULT: PASS"));
    o += "\n";
    return o;
}

std::string reportToJson(const ValidationReport& r)
{
    std::string o = "{\n";
    o += "  \"path\": " + jsonEscape(r.path) + ",\n  \"binary\": " + jsonEscape(r.binary) + ",\n";
    o += "  \"id\": " + jsonEscape(r.id) + ",\n  \"name\": " + jsonEscape(r.name) + ",\n  \"part_kind\": " + jsonEscape(r.kind) + ",\n";
    o += std::string("  \"result\": ") + (r.failed() ? "\"fail\"" : "\"pass\"") + ",\n  \"checks\": [\n";
    for (size_t i = 0; i < r.checks.size(); ++i)
    {
        const auto& c = r.checks[i];
        const char* st = c.status == CheckResult::Pass ? "pass" : c.status == CheckResult::Warn ? "warn" : c.status == CheckResult::Fail ? "fail" : "skip";
        o += "    {\"id\": " + jsonEscape(c.id) + ", \"name\": " + jsonEscape(c.name) + ", \"status\": \"" + st + "\", \"summary\": " + jsonEscape(c.summary) + ", \"messages\": [";
        for (size_t k = 0; k < c.messages.size(); ++k) o += (k ? ", " : "") + jsonEscape(c.messages[k]);
        o += "]}";
        o += (i + 1 < r.checks.size()) ? ",\n" : "\n";
    }
    o += "  ]\n}\n";
    return o;
}

} // namespace rp
