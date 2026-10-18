// ratpipe — pipe_base.hpp
//
// C++17 header-only base for a RatMode extension pipe. You write a class with
// a parameter table and a render() method; this file turns it into the C ABI
// in ratpipe_abi.h and enforces the parts of docs/CONTRACT.md that can be
// enforced by construction:
//
//   M2  every parameter is a std::atomic<float> (static_assert lock-free),
//       written by set_param on the message thread, read in render;
//   M3  output is finite (NaN/Inf -> 0, counted) and clamped to +/- kOutputClamp
//       (4.0, the same hard limit RatMode's ExtPipe applies; clamped samples
//       are counted too — the validator's 2.0 peak check still sees anything
//       between 2 and 4, and a clamp at 4 is still > 2);
//   M10 render is additive and never touches samples outside [0, n);
//   M12 nothing escapes across the ABI: every entry point is wrapped in
//       try/catch(...) — an exception leaves the rest of the block silent and
//       increments faults(); only `ratpipe_entry` is exported;
//   M15 ratpipe_entry builds the descriptor once, allocates nothing, and
//       refuses a host whose ABI major or struct sizes do not match.
//
// Events are delivered sample-accurately: render() is split at every event
// offset and the matching on*() hook runs between the pieces.
//
// A complete pipe (see tests/kit/smoke_pipe.cpp for a runnable one):
//
//   enum { P_CUT, P_RES, P_DECAY, P_LEVEL, P_COUNT };
//   class MyPipe : public ratpipe::Pipe<MyPipe, P_COUNT>
//   {
//   public:
//       static constexpr ratpipe::PipeInfo kInfo {
//           "com.me.mypipe", "My Pipe", "Me", "0.1.0", RATPIPE_MONO, "myp",
//           { P_CUT, P_RES, P_DECAY, P_LEVEL } };
//       static constexpr ratpipe::ParamSpec kParams[P_COUNT] = {
//           ratpipe::ParamSpec("myp.cut",   "Cut",   "Filter", 0.5f).hz(60.0f, 12000.0f),
//           ratpipe::ParamSpec("myp.res",   "Res",   "Filter", 0.3f).pct(),
//           ratpipe::ParamSpec("myp.decay", "Decay", "Env",    0.4f).ms(20.0f, 2000.0f),
//           ratpipe::ParamSpec("myp.level", "Level", "Out",    1.0f).gain().notMutable(),
//       };
//       explicit MyPipe(const ratpipe::CreateArgs& a) : Pipe(a) { osc_.prepare(a.sampleRate); }
//       void onPrepare(double sr, int) { osc_.prepare(sr); }
//       void onNoteOn(const ratpipe::NoteEvent& e) { osc_.setFrequency(ratpipe::kit::midiToHz((float) e.note)); env_ = e.velocity; }
//       void onAllOff() { env_ = 0.0f; }
//       void render(float* L, float* R, int n)            // L/R arrive zeroed; write or add
//       {
//           const float level = param(P_LEVEL);
//           for (int i = 0; i < n; ++i) { const float s = osc_.process() * env_ * level; L[i] = s; R[i] = s; env_ *= 0.9995f; }
//       }
//   private:
//       ratpipe::kit::Osc osc_; float env_ = 0.0f;
//   };
//   RATPIPE_DEFINE_ENTRY(MyPipe)
//
// Rules the base cannot check for you: no allocation / locks / IO inside
// render() or the on*() hooks (contract M1); every sound-affecting state must
// be a declared parameter (M7); ids `<prefix>.<path>` stable forever (M8).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "ratpipe_abi.h"
#include "kit/denormal.hpp"
#include "kit/finite_guard.hpp"

// The C function table lives in EntryImpl; it is marked hidden here so a bundle
// built without -fvisibility=hidden still exports nothing but ratpipe_entry
// from this header. (Your pipe class itself has no out-of-line symbols unless
// you add virtuals; build with hidden visibility anyway — cmake/RatPipe.cmake does.)
#if defined(__GNUC__) || defined(__clang__)
  #define RATPIPE_HIDDEN __attribute__((visibility("hidden")))
#else
  #define RATPIPE_HIDDEN
#endif

// Apple clang lowers a zero memset of runtime size — including one it forms
// itself from a `p[i] = 0` loop, std::fill or vector::assign — to `___bzero`
// on x86_64 (`_bzero` on arm64), and a constant fill loop to
// `_memset_pattern16`. RatMode builds released before its import allowlist
// learnt those names refuse such a bundle at load (ExtMachOCheck), so the base and the kit zero every
// runtime-length buffer with kit::zeroBuffer (kit/finite_guard.hpp), whose
// `volatile`-sourced loop the loop-idiom pass cannot turn into either call.
// Use it in your own pipe too: never memset / std::fill / vector::assign a
// runtime-length buffer, on any thread (the import exists whether or not the
// code runs). Constant-size memsets of small structs are fine (inlined).

namespace ratpipe {

static_assert(std::atomic<float>::is_always_lock_free, "std::atomic<float> must be lock-free for the audio thread");
static_assert(std::atomic<int32_t>::is_always_lock_free, "std::atomic<int32_t> must be lock-free for the audio thread");

// ── Declarative tables ───────────────────────────────────────────────────────

/// One parameter. Build with the constructor + chained setters (all constexpr):
///   ParamSpec("fmb.cutoff", "Cut", "Filter", 0.5f).hz(60.0f, 11000.0f).mutate(0.2f, 0.9f)
struct ParamSpec
{
    const char* id;
    const char* label;
    const char* group;
    float       def;
    float       min        = 0.0f;
    float       max        = 1.0f;
    float       mutateLo   = 0.0f;   // equal lo/hi means "use min..max"
    float       mutateHi   = 0.0f;
    uint32_t    flags      = RATPIPE_PARAM_CONTINUOUS;
    int32_t     steps      = 0;
    int32_t     readout    = RATPIPE_READOUT_NONE;
    float       readoutMin = 0.0f;
    float       readoutMax = 0.0f;
    const char* const* choices = nullptr;

    constexpr ParamSpec(const char* id_, const char* label_, const char* group_, float default_) noexcept
        : id(id_), label(label_), group(group_), def(default_) {}

    constexpr ParamSpec range(float lo, float hi) const noexcept   { ParamSpec p = *this; p.min = lo; p.max = hi; return p; }
    constexpr ParamSpec mutate(float lo, float hi) const noexcept  { ParamSpec p = *this; p.mutateLo = lo; p.mutateHi = hi; return p; }
    constexpr ParamSpec bipolar() const noexcept                   { ParamSpec p = *this; p.flags |= RATPIPE_PARAM_BIPOLAR; return p; }
    constexpr ParamSpec notMutable() const noexcept                { ParamSpec p = *this; p.flags |= RATPIPE_PARAM_NOT_MUTABLE; return p; }
    /// 0..1.5 range, 1.0 = unity (RATPIPE_PARAM_GAIN_STYLE).
    constexpr ParamSpec gain() const noexcept                      { ParamSpec p = *this; p.flags |= RATPIPE_PARAM_GAIN_STYLE; p.min = 0.0f; p.max = 1.5f; return p; }
    /// `n` evenly spaced positions, optional static label array of `n` strings.
    constexpr ParamSpec discrete(int32_t n, const char* const* labels = nullptr) const noexcept
    {
        ParamSpec p = *this; p.flags |= RATPIPE_PARAM_DISCRETE; p.steps = n; p.choices = labels; return p;
    }
    constexpr ParamSpec readoutAs(int32_t kind, float atMin, float atMax) const noexcept
    {
        ParamSpec p = *this; p.readout = kind; p.readoutMin = atMin; p.readoutMax = atMax; return p;
    }
    constexpr ParamSpec hz(float atMin, float atMax) const noexcept  { return readoutAs(RATPIPE_READOUT_HZ, atMin, atMax); }
    constexpr ParamSpec ms(float atMin, float atMax) const noexcept  { return readoutAs(RATPIPE_READOUT_MS, atMin, atMax); }
    constexpr ParamSpec st(float atMin, float atMax) const noexcept  { return readoutAs(RATPIPE_READOUT_ST, atMin, atMax); }
    constexpr ParamSpec pct() const noexcept                         { return readoutAs(RATPIPE_READOUT_PCT, 0.0f, 100.0f); }

    /// Index of the discrete step for a normalised value (DISCRETE params).
    static int32_t stepOf(float value, int32_t steps) noexcept
    {
        if (steps < 2) return 0;
        const float v = kit::clamp01(value);
        int32_t s = static_cast<int32_t>(v * static_cast<float>(steps - 1) + 0.5f);
        return s < 0 ? 0 : (s >= steps ? steps - 1 : s);
    }
};

/// Identity of the pipe (all strings static).
struct PipeInfo
{
    const char* id;                 // reverse-DNS, ^[a-z0-9]+(\.[a-z0-9_]+)+$
    const char* displayName;        // <= 24 chars
    const char* author;
    const char* version;            // semver
    int32_t     partKind;           // RATPIPE_DRUM / RATPIPE_MONO / RATPIPE_POLY
    const char* macroPrefix;        // 2..6 lowercase letters; every param id is "<prefix>.<path>"
    int32_t     performKnobs[RATPIPE_PERFORM_KNOBS] = { -1, -1, -1, -1 };
    uint32_t    flags = 0;          // RATPIPE_FLAG_*
};

/// What create() hands your constructor.
struct CreateArgs
{
    double          sampleRate;   // > 0 (44100 if the host gave 0)
    int             maxBlock;     // > 0 (1024 if the host gave 0)
    uint64_t        seed;         // host seed (RATPIPE_FLAG_DETERMINISTIC_WITH_SEED)
    RatPipeHostInfo host;         // copy of the host info (struct_size-aware)
};

// ── Event views ──────────────────────────────────────────────────────────────

struct TriggerEvent
{
    float velocity;        // 0..1
    int   subIndex;        // 0 for a plain lane
    int   semitoneOffset;  // -24..+24
    int   step;            // pattern step, -1 if none
};

struct NoteEvent
{
    int   note;            // 0..127 (clamped by the base)
    float velocity;        // 0..1 (NOTE_ON)
    int   step;            // pattern step, -1 if none
    bool  slide;           // NOTE_ON: glide from the previous note, do not retrigger
    bool  accent;          // NOTE_ON: 303-style accent
    bool  slideSource;     // NOTE_OFF: delayed off of a slide source
    int   chord;           // M2 chord id for POLY pipes, 0 = none
};

#ifndef RATPIPE_SDK_VERSION_STRING
  #define RATPIPE_SDK_VERSION_STRING "0.1.0"
#endif

// ── Compile-time table validation (the cheap half of validator V2/V3) ───────

namespace detail {

constexpr size_t cstrlen(const char* s) noexcept
{
    size_t n = 0;
    if (s != nullptr) while (s[n] != '\0') ++n;
    return n;
}

constexpr bool startsWithPrefixDot(const char* id, const char* prefix) noexcept
{
    if (id == nullptr || prefix == nullptr) return false;
    size_t i = 0;
    for (; prefix[i] != '\0'; ++i)
        if (id[i] != prefix[i]) return false;
    return id[i] == '.' && id[i + 1] != '\0';
}

constexpr bool validPrefix(const char* p) noexcept
{
    const size_t n = cstrlen(p);
    if (n < 2 || n > 6) return false;
    for (size_t i = 0; i < n; ++i)
        if (p[i] < 'a' || p[i] > 'z') return false;
    return true;
}

template <class Derived, int N>
constexpr bool validateTables() noexcept
{
    const PipeInfo& info = Derived::kInfo;
    if (info.id == nullptr || cstrlen(info.id) == 0) return false;
    if (info.displayName == nullptr || cstrlen(info.displayName) > 24) return false;
    if (!validPrefix(info.macroPrefix)) return false;
    if (N < 1 || N > 256) return false;
    if (sizeof(Derived::kParams) / sizeof(Derived::kParams[0]) != static_cast<size_t>(N)) return false;
    for (int k = 0; k < RATPIPE_PERFORM_KNOBS; ++k)
    {
        const int32_t pk = info.performKnobs[k];
        if (pk != RATPIPE_PERFORM_KNOB_NONE && (pk < 0 || pk >= N)) return false;
    }
    for (int i = 0; i < N; ++i)
    {
        const ParamSpec& p = Derived::kParams[i];
        if (!startsWithPrefixDot(p.id, info.macroPrefix)) return false;
        if (p.label == nullptr || cstrlen(p.label) > 8) return false;
        if (p.group == nullptr) return false;
        if (!(p.min < p.max)) return false;
        if (!(p.def >= p.min && p.def <= p.max)) return false;
        if (p.mutateLo != p.mutateHi && !(p.mutateLo >= p.min && p.mutateHi <= p.max && p.mutateLo < p.mutateHi)) return false;
        if ((p.flags & RATPIPE_PARAM_DISCRETE) != 0 && p.steps < 2) return false;
        for (int j = 0; j < i; ++j)       // unique ids
        {
            const char* a = p.id; const char* b = Derived::kParams[j].id;
            size_t t = 0;
            while (a[t] != '\0' && a[t] == b[t]) ++t;
            if (a[t] == '\0' && b[t] == '\0') return false;
        }
    }
    return true;
}

} // namespace detail

// ── The base ─────────────────────────────────────────────────────────────────

template <class Derived, int NumParams> struct EntryImpl;

template <class Derived, int NumParams>
class Pipe
{
public:
    static constexpr int kNumParams   = NumParams;
    static constexpr int kChunk       = 256;    // render() is never asked for more than this at once
    static constexpr float kOutputClamp = 4.0f;  // hard safety clamp per sample (= RatMode ExtPipe); the contract M3 bound is 2.0, checked by the validator

    static_assert(NumParams >= 1 && NumParams <= 256, "a pipe declares 1..256 parameters");

    explicit Pipe(const CreateArgs& args) noexcept
        : sampleRate_(args.sampleRate), maxBlock_(args.maxBlock), seed_(args.seed), host_(args.host)
    {
        for (int i = 0; i < NumParams; ++i)
            params_[i].store(Derived::kParams[i].def, std::memory_order_relaxed);
        std::memset(&transport_, 0, sizeof transport_);
    }

    // ── what your pipe reads ────────────────────────────────────────────────
    /// Current value of parameter i (normalised, already clamped to its range).
    float param(int i) const noexcept
    {
        return (i >= 0 && i < NumParams) ? params_[i].load(std::memory_order_relaxed) : 0.0f;
    }
    /// Step index of a DISCRETE parameter.
    int paramStep(int i) const noexcept
    {
        return (i >= 0 && i < NumParams) ? ParamSpec::stepOf(param(i), Derived::kParams[i].steps) : 0;
    }
    double sampleRate() const noexcept { return sampleRate_; }
    int    maxBlock() const noexcept { return maxBlock_; }
    uint64_t hostSeed() const noexcept { return seed_; }
    const RatPipeHostInfo& hostInfo() const noexcept { return host_; }
    /// Transport for the block being rendered (zeros if the host gave none).
    const RatPipeTransport& transport() const noexcept { return transport_; }
    bool hasTransport() const noexcept { return hasTransport_; }

    /// Diagnostics (not shared state: written and read on the audio thread only).
    uint32_t faults() const noexcept { return faults_; }
    uint32_t nonFiniteSamples() const noexcept { return nonFinite_; }
    /// Samples that hit the +/- kOutputClamp hard limit. Any count > 0 means
    /// the pipe already violated contract M3 (peak <= 2.0): fix the gain.
    uint32_t clampedSamples() const noexcept { return clamped_; }

    // ── hooks with defaults (define the same name in Derived to override) ──
    /// prepare() is called by every shipped host (RatMode included) only while
    /// render is NOT running — RatMode gates it through its render handshake,
    /// and the SDK host never overlaps the two — so onPrepare may re-size
    /// state and reset DSP blocks without any synchronisation of its own.
    /// It is still called after the constructor, possibly more than once.
    void onPrepare(double, int) noexcept {}
    void onTrigger(const TriggerEvent&) noexcept {}
    void onNoteOn(const NoteEvent&) noexcept {}
    void onNoteOff(const NoteEvent&) noexcept {}
    void onAllOff() noexcept {}
    void onReleaseHeld() noexcept {}
    void onFlush() noexcept { derived().onAllOff(); }
    /// Called once per ABI render call before any splitting; n is the full block.
    void onBlockStart(int) noexcept {}
    /// Return false from Derived::acceptHost to refuse a host (entry returns NULL).
    static bool acceptHost(const RatPipeHostInfo&) noexcept { return true; }

    /// Fallback that fires a readable error if Derived forgot render().
    void render(float*, float*, int) noexcept
    {
        static_assert(sizeof(Derived) == 0, "your pipe must define: void render(float* L, float* R, int n)");
    }

protected:
    Derived&       derived() noexcept       { return static_cast<Derived&>(*this); }
    const Derived& derived() const noexcept { return static_cast<const Derived&>(*this); }

private:
    template <class, int> friend struct EntryImpl;

    // ── ABI side ────────────────────────────────────────────────────────────
    void abiPrepare(double sr, int32_t maxBlock) noexcept
    {
        try
        {
            if (sr > 0.0) sampleRate_ = sr;
            if (maxBlock > 0) maxBlock_ = maxBlock;
            derived().onPrepare(sampleRate_, maxBlock_);
        }
        catch (...) { ++faults_; }
    }

    void abiSetParam(int32_t index, float value) noexcept
    {
        if (index < 0 || index >= NumParams) return;
        if (!kit::isFinite(value)) return;
        const ParamSpec& p = Derived::kParams[index];
        params_[index].store(kit::clamp(value, p.min, p.max), std::memory_order_relaxed);
    }

    float abiGetParam(int32_t index) const noexcept { return param(index); }

    void abiRender(float* L, float* R, int32_t n, const RatPipeEvent* ev, int32_t count,
                   const RatPipeTransport* tr) noexcept
    {
        if (L == nullptr || R == nullptr || n <= 0) return;
        try
        {
            kit::FlushDenormalsScope ftz;

            if (count > 0 && (ev == nullptr || ev[0].struct_size != sizeof(RatPipeEvent))) count = 0;  // not my ABI
            if (tr != nullptr && tr->struct_size >= sizeof(RatPipeTransport)) { transport_ = *tr; hasTransport_ = true; }
            else { std::memset(&transport_, 0, sizeof transport_); hasTransport_ = false; }

            derived().onBlockStart(static_cast<int>(n));

            int32_t pos = 0, e = 0;
            while (pos < n)
            {
                while (e < count && ev[e].sample_offset <= pos) dispatch(ev[e++]);
                int32_t next = n;
                if (e < count)
                {
                    next = ev[e].sample_offset;
                    if (next > n) next = n;
                    if (next <= pos) next = pos + 1;     // defensive: never a zero-length step
                }
                int32_t len = next - pos;
                if (len > kChunk) len = kChunk;
                renderChunk(L + pos, R + pos, len);
                pos += len;
            }
            while (e < count) dispatch(ev[e++]);        // offsets >= n (host bug): still honoured, at the end
        }
        catch (...)
        {
            ++faults_;                                  // rest of the block stays silent (additive: nothing added)
        }
    }

    void dispatch(const RatPipeEvent& x)
    {
        switch (x.type)
        {
            case RATPIPE_EV_TRIGGER:
                derived().onTrigger(TriggerEvent{ kit::clamp01(x.velocity), x.sub_index, x.semitone_offset, x.step_index });
                break;
            case RATPIPE_EV_NOTE_ON:
                derived().onNoteOn(NoteEvent{ clampNote(x.note), kit::clamp01(x.velocity), x.step_index,
                                              (x.flags & RATPIPE_EVF_SLIDE_FROM_PREV) != 0,
                                              (x.flags & RATPIPE_EVF_ACCENT) != 0,
                                              false, x.chord });
                break;
            case RATPIPE_EV_NOTE_OFF:
                derived().onNoteOff(NoteEvent{ clampNote(x.note), 0.0f, x.step_index, false, false,
                                               (x.flags & RATPIPE_EVF_SLIDE_SOURCE) != 0, x.chord });
                break;
            case RATPIPE_EV_ALL_OFF:      derived().onAllOff(); break;
            case RATPIPE_EV_RELEASE_HELD: derived().onReleaseHeld(); break;
            case RATPIPE_EV_FLUSH:        derived().onFlush(); break;
            default: break;
        }
    }

    static int clampNote(int32_t n) noexcept { return n < 0 ? 0 : (n > 127 ? 127 : static_cast<int>(n)); }

    void renderChunk(float* L, float* R, int32_t len)
    {
        kit::zeroBuffer(scratchL_, static_cast<size_t>(len));   // see the note above RATPIPE_HIDDEN
        kit::zeroBuffer(scratchR_, static_cast<size_t>(len));
        derived().render(scratchL_, scratchR_, static_cast<int>(len));
        for (int32_t i = 0; i < len; ++i)
        {
            float l = scratchL_[i], r = scratchR_[i];
            if (!kit::isFinite(l)) { l = 0.0f; ++nonFinite_; }
            if (!kit::isFinite(r)) { r = 0.0f; ++nonFinite_; }
            if (l > kOutputClamp) { l = kOutputClamp; ++clamped_; } else if (l < -kOutputClamp) { l = -kOutputClamp; ++clamped_; }
            if (r > kOutputClamp) { r = kOutputClamp; ++clamped_; } else if (r < -kOutputClamp) { r = -kOutputClamp; ++clamped_; }
            L[i] += l;
            R[i] += r;
        }
    }

    std::atomic<float> params_[NumParams];
    double   sampleRate_;
    int      maxBlock_;
    uint64_t seed_;
    RatPipeHostInfo  host_;
    RatPipeTransport transport_;
    bool     hasTransport_ = false;
    uint32_t faults_ = 0;
    uint32_t nonFinite_ = 0;
    uint32_t clamped_ = 0;
    float    scratchL_[kChunk];
    float    scratchR_[kChunk];
};

// ── Entry: the C function table ──────────────────────────────────────────────

template <class Derived, int NumParams>
struct RATPIPE_HIDDEN EntryImpl
{
    using Base = Pipe<Derived, NumParams>;
    static_assert(std::is_base_of<Base, Derived>::value, "MyPipe must derive from ratpipe::Pipe<MyPipe, N>");
    static_assert(detail::validateTables<Derived, NumParams>(),
                  "pipe tables invalid: check id/prefix ('<prefix>.<path>', prefix 2-6 lowercase letters), "
                  "label <= 8 chars, display name <= 24, min < max, default in range, mutate band inside range, "
                  "DISCRETE steps >= 2, unique ids, perform knob indices, and that kParams has exactly N entries");

    static void* create(double sr, int32_t maxBlock, const RatPipeHostInfo* host) noexcept
    {
        try
        {
            CreateArgs a;
            a.sampleRate = sr > 0.0 ? sr : 44100.0;
            a.maxBlock   = maxBlock > 0 ? maxBlock : 1024;
            std::memset(&a.host, 0, sizeof a.host);
            if (host != nullptr && host->struct_size >= 4)
            {
                const size_t take = host->struct_size < sizeof(RatPipeHostInfo) ? host->struct_size : sizeof(RatPipeHostInfo);
                std::memcpy(&a.host, host, take);
            }
            a.seed = a.host.seed;
            return new (std::nothrow) Derived(a);
        }
        catch (...) { return nullptr; }
    }

    static void destroy(void* inst) noexcept
    {
        try { delete static_cast<Derived*>(inst); } catch (...) {}
    }

    static void prepare(void* inst, double sr, int32_t maxBlock) noexcept
    {
        if (inst != nullptr) static_cast<Derived*>(inst)->abiPrepare(sr, maxBlock);
    }

    static void render(void* inst, float* L, float* R, int32_t n,
                       const RatPipeEvent* ev, int32_t count, const RatPipeTransport* tr) noexcept
    {
        if (inst != nullptr) static_cast<Derived*>(inst)->abiRender(L, R, n, ev, count, tr);
    }

    static void setParam(void* inst, int32_t index, float value) noexcept
    {
        if (inst != nullptr) static_cast<Derived*>(inst)->abiSetParam(index, value);
    }

    static float getParam(void* inst, int32_t index) noexcept
    {
        return inst != nullptr ? static_cast<const Derived*>(inst)->abiGetParam(index) : 0.0f;
    }

    /// Builds (once) and returns the static descriptor; NULL refuses the host.
    static const RatPipeDescriptor* entry(const RatPipeHostInfo* host) noexcept
    {
        try
        {
            if (host == nullptr || host->struct_size < sizeof(RatPipeHostInfo)) return nullptr;
            if (host->abi_major != RATPIPE_ABI_MAJOR) return nullptr;
            if (!Derived::acceptHost(*host)) return nullptr;
            return descriptor();
        }
        catch (...) { return nullptr; }
    }

    /// The descriptor without the host check (for tests / the offline host).
    static const RatPipeDescriptor* descriptor() noexcept
    {
        // Zero-initialised statics: constant-initialised, no guard, live in
        // __DATA/__bss (never __TEXT), built exactly once on the message thread.
        static RatPipeParamDesc  paramDescs[NumParams];
        static RatPipeDescriptor desc;
        static bool built = false;
        if (!built)
        {
            for (int i = 0; i < NumParams; ++i)
            {
                const ParamSpec& s = Derived::kParams[i];
                RatPipeParamDesc& d = paramDescs[i];
                std::memset(&d, 0, sizeof d);
                d.struct_size   = sizeof(RatPipeParamDesc);
                d.id            = s.id;
                d.label         = s.label;
                d.group         = s.group;
                d.default_value = s.def;
                d.min_value     = s.min;
                d.max_value     = s.max;
                d.mutate_lo     = s.mutateLo;
                d.mutate_hi     = s.mutateHi;
                d.flags         = s.flags;
                d.steps         = s.steps;
                d.readout_kind  = s.readout;
                d.readout_min   = s.readoutMin;
                d.readout_max   = s.readoutMax;
                d.choices       = s.choices;
            }
            const PipeInfo& info = Derived::kInfo;
            std::memset(&desc, 0, sizeof desc);
            desc.struct_size  = sizeof(RatPipeDescriptor);
            desc.abi_major    = RATPIPE_ABI_MAJOR;
            desc.abi_minor    = RATPIPE_ABI_MINOR;
            desc.flags        = info.flags;
            desc.id           = info.id;
            desc.display_name = info.displayName;
            desc.author       = info.author != nullptr ? info.author : "";
            desc.version      = info.version != nullptr ? info.version : "0.0.0";
            desc.sdk_version  = RATPIPE_SDK_VERSION_STRING;
            desc.part_kind    = info.partKind;
            desc.macro_prefix = info.macroPrefix;
            desc.params       = paramDescs;
            desc.param_count  = static_cast<uint32_t>(NumParams);
            for (int k = 0; k < RATPIPE_PERFORM_KNOBS; ++k) desc.perform_knobs[k] = info.performKnobs[k];
            desc.create    = &create;
            desc.destroy   = &destroy;
            desc.prepare   = &prepare;
            desc.render    = &render;
            desc.set_param = &setParam;
            desc.get_param = &getParam;
            desc.theme     = nullptr;
            built = true;
        }
        return &desc;
    }
};

template <class Derived>
using Entry = EntryImpl<Derived, Derived::kNumParams>;

} // namespace ratpipe

/// Put this once, at namespace scope, in exactly one translation unit of the
/// bundle. It defines the single exported symbol `ratpipe_entry`; everything
/// else in the bundle must be built with -fvisibility=hidden
/// -fvisibility-inlines-hidden (cmake/RatPipe.cmake does this).
#define RATPIPE_DEFINE_ENTRY(PipeClass)                                                              \
    extern "C" RATPIPE_EXPORT const RatPipeDescriptor* ratpipe_entry(const RatPipeHostInfo* host)   \
    {                                                                                                \
        return ::ratpipe::Entry<PipeClass>::entry(host);                                             \
    }
