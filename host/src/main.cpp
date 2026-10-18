// ratpipe-host — the SDK's offline host and validator (Arch38 §7.3).
//
//   ratpipe-host validate <bundle|lib> [options]     V1-V13
//   ratpipe-host render   <bundle|lib> [options]     deterministic audition WAV
//   ratpipe-host info     <bundle|lib>               descriptor summary
//   ratpipe-host --dump-manifest <bundle|lib> [--out FILE] [--expect-id ID] [--min-ratmode-version V]
//   ratpipe-host --dump-plist    <bundle|lib> [--out FILE] [--expect-id ID]
//
// Exit codes: 0 ok, 1 validation failed (or the pipe crashed), 2 usage / load error.
#include "engine.hpp"
#include "manifest.hpp"
#include "plugin.hpp"
#include "rtguard.hpp"
#include "util.hpp"
#include "validator.hpp"
#include "wav.hpp"
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace rp;

namespace {

void usage(std::FILE* f)
{
    std::fputs(
        "ratpipe-host " "0.1.0" " — offline host and validator for RatMode pipes\n"
        "\n"
        "  ratpipe-host validate <bundle|lib> [--quick] [--seeds N] [--json] [--src DIR]... [--only V4,V9]\n"
        "                                     [--allow-single-arch] [--timing-slack X] [--seed N] [--no-fork]\n"
        "                                     [--timeout SECONDS]   (watchdog, default 300, 0 = none)\n"
        "  ratpipe-host render   <bundle|lib> --out FILE.wav [--pattern drum16|acid16|chords|silence] [--bpm 130]\n"
        "                                     [--seconds 8] [--tail 1.5] [--sr 48000] [--block 256] [--seed N]\n"
        "                                     [--params default|random|min|max] [--param id=value]... [--format 16|24|32f]\n"
        "  ratpipe-host info     <bundle|lib>\n"
        "  ratpipe-host --dump-manifest <bundle|lib> [--out FILE] [--expect-id ID] [--min-ratmode-version V]\n"
        "  ratpipe-host --dump-plist    <bundle|lib> [--out FILE] [--expect-id ID]\n"
        "  ratpipe-host --check-macho   <dylib|bundle>...     RatMode's load-time Mach-O check, any platform\n"
        "\n"
        "exit codes: 0 ok, 1 validation failed / pipe crashed or hung, 2 usage or load error\n", f);
}

struct Args
{
    std::vector<std::string> pos;
    std::vector<std::pair<std::string, std::string>> opts;     // option -> value ("" for flags), in order

    bool has(const std::string& k) const { for (auto& o : opts) if (o.first == k) return true; return false; }
    std::string get(const std::string& k, const std::string& def = "") const { for (auto& o : opts) if (o.first == k) return o.second; return def; }
    std::vector<std::string> all(const std::string& k) const { std::vector<std::string> r; for (auto& o : opts) if (o.first == k) r.push_back(o.second); return r; }
};

bool parseArgs(int argc, char** argv, int from, const std::vector<std::string>& valued, Args& a, std::string& err)
{
    for (int i = from; i < argc; ++i)
    {
        std::string s = argv[i];
        if (s.size() > 2 && s[0] == '-' && s[1] == '-')
        {
            const std::string k = s.substr(2);
            if (std::find(valued.begin(), valued.end(), k) != valued.end())
            {
                if (i + 1 >= argc) { err = "option --" + k + " needs a value"; return false; }
                a.opts.emplace_back(k, argv[++i]);
            }
            else a.opts.emplace_back(k, "");
        }
        else a.pos.push_back(s);
    }
    return true;
}

bool writeFileOrStdout(const std::string& path, const std::string& text)
{
    if (path.empty()) { std::fwrite(text.data(), 1, text.size(), stdout); return true; }
    std::ofstream f(path, std::ios::binary);
    if (! f) { std::fprintf(stderr, "ratpipe-host: cannot write %s\n", path.c_str()); return false; }
    f << text;
    return true;
}

// Loads and lints; prints findings to stderr. Returns false on error.
bool loadAndLint(const std::string& path, Plugin& pl, bool quietWarnings)
{
    if (! loadPlugin(path, 48000.0, 1024, 0x52617450697065ull, pl))
    {
        std::fprintf(stderr, "ratpipe-host: %s: %s\n", path.c_str(), pl.error.c_str());
        return false;
    }
    std::vector<Finding> fs;
    const bool safe = lintDescriptor(pl.desc, fs);
    bool bad = ! safe;
    for (const auto& f : fs)
    {
        if (f.warning && quietWarnings) continue;
        std::fprintf(stderr, "ratpipe-host: %s %s: %s\n", f.check.c_str(), f.warning ? "warning" : "error", f.message.c_str());
        if (! f.warning) bad = true;
    }
    return ! bad;
}

// ── dump-manifest / dump-plist ───────────────────────────────────────────────

int cmdDump(bool plist, const Args& a)
{
    if (a.pos.size() != 1) { usage(stderr); return 2; }
    Plugin pl;
    if (! loadAndLint(a.pos[0], pl, true)) return 2;
    const std::string want = a.get("expect-id");
    if (! want.empty() && want != pl.desc->id)
    {
        std::fprintf(stderr, "ratpipe-host: descriptor id '%s' does not match the id '%s' given to add_ratpipe()/--expect-id\n", pl.desc->id, want.c_str());
        return 2;
    }
    const std::string text = plist ? buildPlist(pl.desc, pl.binary.substr(pl.binary.rfind('/') + 1)) : buildManifest(pl.desc, a.get("min-ratmode-version", "0.1.8"));
    return writeFileOrStdout(a.get("out"), text) ? 0 : 2;
}

// ── check-macho ──────────────────────────────────────────────────────────────

// Runs RatMode's vendored ExtMachOCheck on each file (a .ratpipe bundle resolves to
// its binary). Pure C++ over the bytes, so a Linux CI job can check a cross-built
// dylib exactly as RatMode will at load. Exit 0 all accepted, 1 any refused, 2 a
// file is unreadable or not Mach-O.
int cmdCheckMachO(const Args& a)
{
    if (a.pos.empty()) { usage(stderr); return 2; }
    int rc = 0;
    for (const std::string& given : a.pos)
    {
        std::string binary = given, bundleDir, err;
        struct stat st;
        if (::stat(given.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && ! resolveBinary(given, binary, bundleDir, err))
        { std::printf("%s: REFUSED  %s\n", given.c_str(), err.c_str()); rc = std::max(rc, 2); continue; }
        std::ifstream f(binary, std::ios::binary);
        if (! f) { std::printf("%s: UNREADABLE\n", binary.c_str()); rc = std::max(rc, 2); continue; }
        std::ostringstream ss; ss << f.rdbuf();
        MachOVerdict v;
        if (! checkMachOBytes(ss.str(), v))
        { std::printf("%s: NOT MACH-O  (not a macOS binary; nothing for RatMode's check to say)\n", binary.c_str()); rc = std::max(rc, 2); continue; }
        if (v.ok) std::printf("%s: OK  %d slice(s), %d distinct import(s), all allowlisted — RatMode will load it\n", binary.c_str(), v.slices, v.imports);
        else { std::printf("%s: REFUSED  %s — RatMode would not load this bundle\n", binary.c_str(), v.reason.c_str()); rc = std::max(rc, 1); }
    }
    return rc;
}

// ── info ─────────────────────────────────────────────────────────────────────

int cmdInfo(const Args& a)
{
    if (a.pos.size() != 1) { usage(stderr); return 2; }
    Plugin pl;
    if (! loadAndLint(a.pos[0], pl, false)) return 2;
    const RatPipeDescriptor* d = pl.desc;
    std::printf("%s  \"%s\"  v%s  by %s\n", d->id, d->display_name, d->version, d->author);
    std::printf("  kind %s, prefix '%s.', ABI %u.%u, sdk '%s', %u param(s), flags 0x%x\n",
                partKindName(d->part_kind), d->macro_prefix, d->abi_major, d->abi_minor, d->sdk_version, d->param_count, d->flags);
    std::printf("  binary: %s\n", pl.binary.c_str());
    std::printf("  perform knobs:");
    for (int k = 0; k < RATPIPE_PERFORM_KNOBS; ++k)
    {
        const int i = d->perform_knobs[k];
        if (i == RATPIPE_PERFORM_KNOB_NONE) std::printf(" [-]");
        else std::printf(" [%s]", d->params[i].label);
    }
    std::printf("\n");
    for (uint32_t i = 0; i < d->param_count; ++i)
    {
        const RatPipeParamDesc& p = d->params[i];
        std::printf("  %2u  %-24s %-8s %-10s default %-6g range [%g, %g]%s%s%s%s\n", i, p.id, p.label, p.group, p.default_value,
                    p.min_value, p.max_value,
                    (p.flags & RATPIPE_PARAM_DISCRETE) ? fmt(" discrete(%d)", p.steps).c_str() : "",
                    (p.flags & RATPIPE_PARAM_BIPOLAR) ? " bipolar" : "",
                    (p.flags & RATPIPE_PARAM_GAIN_STYLE) ? " gain" : "",
                    (p.flags & RATPIPE_PARAM_NOT_MUTABLE) ? " not-mutable" : "");
    }
    return 0;
}

// ── render ───────────────────────────────────────────────────────────────────

int cmdRender(const Args& a)
{
    if (a.pos.size() != 1) { usage(stderr); return 2; }
    const std::string out = a.get("out");
    if (out.empty()) { std::fprintf(stderr, "ratpipe-host render: --out FILE.wav is required\n"); return 2; }
    Plugin pl;
    if (! loadAndLint(a.pos[0], pl, true)) return 2;
    const RatPipeDescriptor* d = pl.desc;

    const double sr = std::atof(a.get("sr", "48000").c_str());
    const double bpm = std::atof(a.get("bpm", "130").c_str());
    const double seconds = std::atof(a.get("seconds", "8").c_str());
    const double tail = std::atof(a.get("tail", "1.5").c_str());
    const int block = std::atoi(a.get("block", "256").c_str());
    const uint64_t seed = std::strtoull(a.get("seed", "1").c_str(), nullptr, 10);
    if (sr < 8000 || sr > 384000 || bpm < 20 || bpm > 400 || seconds <= 0 || seconds > 600 || block < 1 || block > 1024 || tail < 0)
    { std::fprintf(stderr, "ratpipe-host render: option out of range (sr 8000-384000, bpm 20-400, seconds 0-600, block 1-1024)\n"); return 2; }

    Pattern pat;
    const std::string pname = a.get("pattern", d->part_kind == RATPIPE_DRUM ? "drum16" : "acid16");
    if (! parsePattern(pname, pat) || pat == Pattern::Hold || pat == Pattern::Fuzz)
    { std::fprintf(stderr, "ratpipe-host render: unknown pattern '%s' (drum16, acid16, chords, silence)\n", pname.c_str()); return 2; }

    WavFormat wf = WavFormat::Pcm16;
    const std::string fmtName = a.get("format", "16");
    if (fmtName == "24") wf = WavFormat::Pcm24; else if (fmtName == "32f") wf = WavFormat::Float32; else if (fmtName != "16")
    { std::fprintf(stderr, "ratpipe-host render: --format must be 16, 24 or 32f\n"); return 2; }

    Instance inst(pl, sr, 1024, seed);
    if (! inst.ok()) { std::fprintf(stderr, "ratpipe-host render: create() returned NULL\n"); return 2; }
    inst.prepare(sr, 1024);

    Rng rng(seed);
    const std::string pm = a.get("params", "default");
    if (pm == "default") applyParams(inst, ParamMode::Default, rng);
    else if (pm == "random") applyParams(inst, ParamMode::Random, rng);
    else if (pm == "min") applyParams(inst, ParamMode::Min, rng);
    else if (pm == "max") applyParams(inst, ParamMode::Max, rng);
    else { std::fprintf(stderr, "ratpipe-host render: --params must be default, random, min or max\n"); return 2; }
    for (const auto& kv : a.all("param"))
    {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "ratpipe-host render: --param wants id=value\n"); return 2; }
        const std::string id = kv.substr(0, eq);
        const float v = (float) std::atof(kv.c_str() + eq + 1);
        bool found = false;
        for (uint32_t i = 0; i < d->param_count; ++i)
            if (id == d->params[i].id)
            {
                found = true;
                if (v < d->params[i].min_value || v > d->params[i].max_value)
                    std::fprintf(stderr, "ratpipe-host render: warning: %s=%g is outside [%g, %g]\n", id.c_str(), v, d->params[i].min_value, d->params[i].max_value);
                inst.setParam((int) i, v);
            }
        if (! found) { std::fprintf(stderr, "ratpipe-host render: no parameter '%s' (see `ratpipe-host info`)\n", id.c_str()); return 2; }
    }

    const int bars = (int) std::ceil(seconds * bpm / 240.0) + 1;
    auto evs = makePattern(pat, d->part_kind, sr, bpm, bars, seed);
    const int64_t endFrame = (int64_t) std::llround(seconds * sr);
    evs.erase(std::remove_if(evs.begin(), evs.end(), [&](const TimedEvent& e) { return e.frame >= endFrame; }), evs.end());
    evs.push_back(makeControl(RATPIPE_EV_RELEASE_HELD, endFrame));

    RunSpec s; s.sr = sr; s.block = block; s.maxBlock = 1024; s.bpm = bpm; s.events = &evs;
    s.frames = endFrame + (int64_t) std::llround(tail * sr); s.keepAudio = true;
    const RunResult res = runPipe(inst, s);

    if (res.threw)         { std::fprintf(stderr, "ratpipe-host render: a C++ exception escaped render\n"); return 1; }
    if (res.guardViolated) { std::fprintf(stderr, "ratpipe-host render: render wrote outside [0, n) (%s)\n", res.guardWhere.c_str()); return 1; }
    if (res.nonFinite)     { std::fprintf(stderr, "ratpipe-host render: non-finite sample at %.3f s — not writing the file\n", (double) res.nonFiniteFrame / sr); return 1; }

    std::string err;
    if (! writeWav(out, res.l, res.r, sr, wf, err)) { std::fprintf(stderr, "ratpipe-host render: %s\n", err.c_str()); return 2; }
    double ss = 0; for (size_t i = 0; i < res.l.size(); ++i) ss += (double) res.l[i] * res.l[i] + (double) res.r[i] * res.r[i];
    const double rms = std::sqrt(ss / (2.0 * (double) std::max<size_t>(1, res.l.size())));
    std::printf("wrote %s  (%s, %.1f s, %.0f Hz, pattern %s @ %.0f bpm)  peak %.2f (%.1f dBFS)  rms %.1f dBFS\n", out.c_str(), d->display_name,
                (double) res.l.size() / sr, sr, patternName(pat), bpm, res.peak, toDb(res.peak), toDb(rms));
    if (res.peak > 1.0f) std::printf("note: peak is above 0 dBFS; the %s WAV is clipped at full scale\n", fmtName == "32f" ? "(32f is not)" : "integer");
    if (res.peak < 1e-4f) std::printf("note: the render is silent — check the pattern suits the part kind and that the defaults are audible\n");
    return 0;
}

// ── validate ─────────────────────────────────────────────────────────────────

const char* signalName(int s)
{
    switch (s) { case SIGSEGV: return "SIGSEGV"; case SIGBUS: return "SIGBUS"; case SIGABRT: return "SIGABRT"; case SIGFPE: return "SIGFPE";
                 case SIGILL: return "SIGILL"; case SIGTRAP: return "SIGTRAP"; case SIGKILL: return "SIGKILL"; default: return "signal"; }
}

int cmdValidate(const Args& a)
{
    if (a.pos.size() != 1) { usage(stderr); return 2; }
    ValidateOptions o;
    o.quick = a.has("quick");
    if (a.has("seeds")) o.seeds = std::max(0, std::atoi(a.get("seeds").c_str()));
    o.srcDirs = a.all("src");
    o.allowSingleArch = a.has("allow-single-arch");
    if (a.has("timing-slack")) o.timingSlack = std::max(0.1, std::atof(a.get("timing-slack").c_str()));
    else if (const char* e = std::getenv("RATPIPE_TIMING_SLACK")) o.timingSlack = std::max(0.1, std::atof(e));
    if (a.has("seed")) o.hostSeed = std::strtoull(a.get("seed").c_str(), nullptr, 0);
    for (const auto& v : a.all("only"))
    {
        size_t p = 0;
        while (p <= v.size())
        {
            const size_t c = v.find(',', p);
            std::string t = v.substr(p, c == std::string::npos ? std::string::npos : c - p);
            for (auto& ch : t) ch = (char) std::toupper((unsigned char) ch);
            if (! t.empty()) o.only.insert(t);
            if (c == std::string::npos) break;
            p = c + 1;
        }
    }
    const bool json = a.has("json");
    double timeoutS = 300.0;                       // a pipe that spins in render would otherwise hang the validator for ever
    if (a.has("timeout")) timeoutS = std::atof(a.get("timeout").c_str());
    else if (const char* e = std::getenv("RATPIPE_VALIDATE_TIMEOUT")) timeoutS = std::atof(e);
    if (! (timeoutS >= 0.0)) { std::fprintf(stderr, "ratpipe-host validate: --timeout wants a number of seconds (0 disables)\n"); return 2; }
    const bool color = ! json && ! a.has("no-color") && isatty(STDOUT_FILENO) && std::getenv("NO_COLOR") == nullptr;
    const bool chatty = a.has("verbose") || (! json && isatty(STDERR_FILENO));

    // The pipe is untrusted native code: run the whole validation in a child so a
    // crash in the pipe is reported as a failure instead of taking the host down.
    char* stage = (char*) mmap(nullptr, 256, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (stage == MAP_FAILED) stage = nullptr;
    o.progress = [&](const std::string& s)
    {
        if (stage) { std::strncpy(stage, s.c_str(), 255); stage[255] = 0; }
        if (chatty) std::fprintf(stderr, "  ... %s\n", s.c_str());
    };

    if (a.has("no-fork"))
    {
        const ValidationReport rep = validatePipe(a.pos[0], o);
        const std::string text = json ? reportToJson(rep) : reportToText(rep, color);
        std::fwrite(text.data(), 1, text.size(), stdout);
        return rep.failed() ? 1 : 0;
    }

    int fds[2];
    if (pipe(fds) != 0) { std::perror("pipe"); return 2; }
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 2; }
    if (pid == 0)
    {
        close(fds[0]);
        const ValidationReport rep = validatePipe(a.pos[0], o);
        const std::string text = json ? reportToJson(rep) : reportToText(rep, color);
        size_t off = 0;
        while (off < text.size())
        {
            const ssize_t w = write(fds[1], text.data() + off, text.size() - off);
            if (w <= 0) break;
            off += (size_t) w;
        }
        close(fds[1]);
        _exit(rep.failed() ? 1 : 0);
    }
    close(fds[1]);
    // Watchdog: the child writes its whole report at the end, so "no bytes yet" is normal;
    // the deadline is on the whole validation. On expiry the child is SIGKILLed (a spinning
    // render loop ignores anything gentler) and the stage it was in is reported.
    std::string text;
    char buf[4096];
    bool hung = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timeoutS));
    for (;;)
    {
        int waitMs = -1;
        if (timeoutS > 0.0)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) { hung = true; break; }
            waitMs = (int) std::min<long long>(left, 1000);
        }
        pollfd pf; pf.fd = fds[0]; pf.events = POLLIN; pf.revents = 0;
        const int pr = poll(&pf, 1, waitMs);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;                       // tick: loop re-checks the deadline
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;                           // EOF: the child is done (or died)
        text.append(buf, (size_t) n);
    }
    close(fds[0]);
    int status = 0;
    if (hung)
    {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        const std::string during = stage ? stage : "";
        if (json)
            std::printf("{\n  \"path\": %s,\n  \"result\": \"hang\",\n  \"timeout_s\": %g,\n  \"during\": %s\n}\n", jsonEscape(a.pos[0]).c_str(), timeoutS, jsonEscape(during).c_str());
        else
            std::printf("ratpipe-host %s validating %s\nHANG: the validator did not finish within %g s and was killed while running: %s\n"
                        "A render that never returns (or runs far slower than real time) is always a failure: RatMode's audio thread would stall. "
                        "Raise --timeout only if the machine is genuinely that slow.\nRESULT: FAIL\n",
                        kHostVersion, a.pos[0].c_str(), timeoutS, during.empty() ? "(loading)" : during.c_str());
        return 1;
    }
    waitpid(pid, &status, 0);

    if (WIFSIGNALED(status))
    {
        const int sig = WTERMSIG(status);
        const std::string during = stage ? stage : "";
        if (json)
            std::printf("{\n  \"path\": %s,\n  \"result\": \"crash\",\n  \"signal\": %d,\n  \"during\": %s\n}\n", jsonEscape(a.pos[0]).c_str(), sig, jsonEscape(during).c_str());
        else
            std::printf("ratpipe-host %s validating %s\nCRASH: the pipe killed the validator with %s (signal %d) while running: %s\n"
                        "A crash is always a failure: RatMode would quarantine this bundle after one. Run it under a debugger/ASan build to find the fault.\nRESULT: FAIL\n",
                        kHostVersion, a.pos[0].c_str(), signalName(sig), sig, during.empty() ? "(loading)" : during.c_str());
        return 1;
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { usage(stderr); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") { usage(stdout); return 0; }
    if (cmd == "--version" || cmd == "version") { std::printf("ratpipe-host %s (ABI %u.%u)\n", kHostVersion, RATPIPE_ABI_MAJOR, RATPIPE_ABI_MINOR); return 0; }

    Args a; std::string err;
    const std::vector<std::string> valued = { "seeds", "src", "only", "timing-slack", "seed", "out", "pattern", "bpm", "seconds", "tail",
                                              "sr", "block", "params", "param", "format", "expect-id", "min-ratmode-version", "timeout" };
    if (! parseArgs(argc, argv, 2, valued, a, err)) { std::fprintf(stderr, "ratpipe-host: %s\n", err.c_str()); return 2; }

    if (cmd == "validate") { rtguard::ensurePreloaded(argv); return cmdValidate(a); }
    if (cmd == "render")   { return cmdRender(a); }
    if (cmd == "info")     { return cmdInfo(a); }
    if (cmd == "--dump-manifest") return cmdDump(false, a);
    if (cmd == "--dump-plist")    return cmdDump(true, a);
    if (cmd == "--check-macho" || cmd == "check-macho") return cmdCheckMachO(a);

    std::fprintf(stderr, "ratpipe-host: unknown command '%s'\n", cmd.c_str());
    usage(stderr);
    return 2;
}
