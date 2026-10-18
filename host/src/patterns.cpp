#include "patterns.hpp"
#include "util.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace rp {

bool parsePattern(const std::string& n, Pattern& out)
{
    if (n == "drum16")  { out = Pattern::Drum16;  return true; }
    if (n == "acid16")  { out = Pattern::Acid16;  return true; }
    if (n == "chords")  { out = Pattern::Chords;  return true; }
    if (n == "silence") { out = Pattern::Silence; return true; }
    if (n == "hold")    { out = Pattern::Hold;    return true; }
    if (n == "fuzz")    { out = Pattern::Fuzz;    return true; }
    return false;
}

const char* patternName(Pattern p)
{
    switch (p)
    {
        case Pattern::Drum16:  return "drum16";
        case Pattern::Acid16:  return "acid16";
        case Pattern::Chords:  return "chords";
        case Pattern::Silence: return "silence";
        case Pattern::Hold:    return "hold";
        case Pattern::Fuzz:    return "fuzz";
    }
    return "?";
}

bool patternReleasesAll(Pattern p) { return p != Pattern::Hold && p != Pattern::Fuzz; }

int64_t barFrames(double sr, double bpm) { return (int64_t) std::llround(4.0 * 60.0 / bpm * sr); }

static RatPipeEvent blank(int type)
{
    RatPipeEvent e;
    std::memset(&e, 0, sizeof e);
    e.struct_size = (uint32_t) sizeof(RatPipeEvent);
    e.type = type;
    e.step_index = -1;
    return e;
}

TimedEvent makeControl(int type, int64_t frame)
{
    TimedEvent t;
    t.frame = frame;
    t.ev = blank(type);
    return t;
}

namespace {

struct Out
{
    std::vector<TimedEvent> v;
    int kind;

    void trigger(int64_t f, float vel, int semis, int step, uint32_t flags = 0)
    {
        TimedEvent t; t.frame = f; t.ev = blank(RATPIPE_EV_TRIGGER);
        t.ev.velocity = vel; t.ev.semitone_offset = semis; t.ev.step_index = step; t.ev.flags = flags;
        v.push_back(t);
    }
    void on(int64_t f, int note, float vel, int step, uint32_t flags = 0)
    {
        TimedEvent t; t.frame = f; t.ev = blank(RATPIPE_EV_NOTE_ON);
        t.ev.note = std::max(0, std::min(127, note)); t.ev.velocity = vel; t.ev.step_index = step; t.ev.flags = flags;
        v.push_back(t);
    }
    void off(int64_t f, int note, uint32_t flags = 0)
    {
        TimedEvent t; t.frame = f; t.ev = blank(RATPIPE_EV_NOTE_OFF);
        t.ev.note = std::max(0, std::min(127, note)); t.ev.flags = flags;
        v.push_back(t);
    }
    bool drum() const { return kind == RATPIPE_DRUM; }
};

} // namespace

std::vector<TimedEvent> makePattern(Pattern p, int kind, double sr, double bpm, int bars, uint64_t seed)
{
    Out o; o.kind = kind;
    const double stepF = 60.0 / bpm / 4.0 * sr;               // one 16th in frames
    auto stepAt = [&](int64_t absStep) { return (int64_t) std::llround((double) absStep * stepF); };
    const int64_t nSteps = (int64_t) bars * 16;

    switch (p)
    {
        case Pattern::Silence:
            break;

        case Pattern::Hold:
            if (o.drum()) o.trigger(0, 1.0f, 0, 0);
            else          o.on(0, 48, 1.0f, 0);
            break;

        case Pattern::Drum16:
        {
            static const float vel[16]  = { 1.0f, 0, 0, 0,  0.8f, 0, 0.6f, 0,  1.0f, 0, 0, 0.5f,  0.9f, 0, 0.7f, 0 };
            static const int   semi[16] = { 0, 0, 0, 0,  0, 0, 7, 0,  0, 0, 0, -5,  0, 0, 12, 0 };
            for (int64_t s = 0; s < nSteps; ++s)
            {
                const int k = (int) (s % 16);
                if (vel[k] <= 0.0f) continue;
                const int64_t f = stepAt(s);
                if (o.drum()) o.trigger(f, vel[k], semi[k], k);
                else
                {
                    o.on(f, 36 + semi[k], vel[k], k);
                    o.off(stepAt(s) + (int64_t) (stepF * 0.5), 36 + semi[k]);
                }
            }
            break;
        }

        case Pattern::Acid16:
        {
            static const int  note[16]   = { 36, 36, 48, 36, 39, 36, 43, 36,  41, 36, 48, 46, 36, 43, 39, 36 };
            static const bool rest[16]   = { 0, 0, 0, 1, 0, 0, 0, 1,  0, 1, 0, 0, 0, 0, 0, 1 };
            static const bool accent[16] = { 1, 0, 0, 0, 1, 0, 0, 0,  0, 0, 1, 0, 0, 1, 0, 0 };
            static const bool slideTo[16]= { 0, 0, 1, 0, 0, 0, 0, 1,  0, 0, 0, 0, 1, 0, 0, 0 };   // this step slides FROM the previous
            for (int64_t s = 0; s < nSteps; ++s)
            {
                const int k = (int) (s % 16);
                if (rest[k]) continue;
                const int64_t f = stepAt(s);
                const bool acc = accent[k];
                if (o.drum())
                {
                    o.trigger(f, acc ? 1.0f : 0.75f, note[k] - 36, k, acc ? RATPIPE_EVF_ACCENT : 0u);
                    continue;
                }
                // Does the NEXT non-rest step slide from this one?
                const int kn = (k + 1) % 16;
                const bool slidesOut = ! rest[kn] && slideTo[kn];
                uint32_t fl = (acc ? RATPIPE_EVF_ACCENT : 0u);
                const bool prevSlid = (k > 0 || s > 0) && slideTo[k] && ! rest[(k + 15) % 16];
                if (prevSlid) fl |= RATPIPE_EVF_SLIDE_FROM_PREV;
                o.on(f, note[k], acc ? 1.0f : 0.8f, k, fl);
                if (slidesOut)
                {
                    // The delayed off of a slide source lands right after the next note-on.
                    o.off(stepAt(s + 1) + 1, note[k], RATPIPE_EVF_SLIDE_SOURCE);
                }
                else
                    o.off(f + (int64_t) (stepF * 0.6), note[k]);
            }
            break;
        }

        case Pattern::Chords:
        {
            static const int root[4] = { 48, 53, 55, 57 };
            const int64_t bf = barFrames(sr, bpm);
            for (int b = 0; b < bars; ++b)
            {
                const int64_t f = (int64_t) b * bf;
                const int r = root[b % 4];
                if (o.drum()) o.trigger(f, 0.9f, r - 48, 0);
                else { o.on(f, r, 0.85f, 0); o.off(f + (int64_t) ((double) bf * 0.9), r); }
            }
            break;
        }

        case Pattern::Fuzz:
        {
            Rng r(seed ^ 0xF00DF00Dull);
            for (int64_t s = 0; s < nSteps; ++s)
            {
                const int k = (int) (s % 16);
                const int64_t f = stepAt(s) + (int64_t) r.range(0, (int) std::max<double>(1.0, stepF - 1.0));
                if (! r.chance(0.55)) continue;
                const float vel = r.chance(0.1) ? (r.chance(0.5) ? 0.0f : 1.0f) : (float) r.uniform();
                if (o.drum())
                {
                    o.trigger(f, vel, r.range(-24, 24), k, r.chance(0.3) ? RATPIPE_EVF_ACCENT : 0u);
                }
                else
                {
                    const int n = r.chance(0.15) ? (r.chance(0.5) ? 0 : 127) : r.range(24, 84);
                    uint32_t fl = 0;
                    if (r.chance(0.25)) fl |= RATPIPE_EVF_SLIDE_FROM_PREV;
                    if (r.chance(0.3))  fl |= RATPIPE_EVF_ACCENT;
                    if (r.chance(0.2)) o.off(f, r.range(24, 84), r.chance(0.3) ? RATPIPE_EVF_SLIDE_SOURCE : 0u);   // possibly unmatched
                    o.on(f, n, vel, k, fl);
                    if (r.chance(0.6)) o.off(f + (int64_t) (stepF * (0.2 + r.uniform() * 1.5)), n);
                }
                if (r.chance(0.04)) { TimedEvent c = makeControl(RATPIPE_EV_ALL_OFF + r.range(0, 2), f); o.v.push_back(c); }
            }
            break;
        }
    }

    std::stable_sort(o.v.begin(), o.v.end(), [](const TimedEvent& a, const TimedEvent& b) { return a.frame < b.frame; });
    return o.v;
}

} // namespace rp
