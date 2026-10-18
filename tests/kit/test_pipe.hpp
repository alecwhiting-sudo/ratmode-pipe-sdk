// Misbehaving test pipe used to prove pipe_base's guards: it can throw from
// render, emit NaN/Inf, emit out-of-range samples, and it records every event
// it receives together with the exact sample position at which the event was
// dispatched (so event-splitting offsets can be checked to the sample).
#pragma once
#include <stdexcept>
#include "ratpipe/pipe_base.hpp"

enum { TP_MODE = 0, TP_GAIN, TP_COUNT };

struct Seen
{
    int type;
    int note;
    int position;    // absolute sample position within the block when dispatched
    bool slide, accent, slideSource;
    float velocity;
};

class TestPipe : public ratpipe::Pipe<TestPipe, TP_COUNT>
{
public:
    enum Mode { Normal = 0, Throw, Nan, Inf, Loud, ThrowInNoteOn };

    static constexpr ratpipe::PipeInfo kInfo {
        "dev.ratmode.sdk_test", "SDK Test", "RatMode SDK", "0.0.1", RATPIPE_DRUM, "tst",
        { TP_MODE, TP_GAIN, RATPIPE_PERFORM_KNOB_NONE, RATPIPE_PERFORM_KNOB_NONE }, 0u };

    static constexpr ratpipe::ParamSpec kParams[TP_COUNT] = {
        ratpipe::ParamSpec("tst.mode", "Mode", "Test", 0.0f).discrete(6),
        ratpipe::ParamSpec("tst.gain", "Gain", "Test", 1.0f).gain(),
    };

    explicit TestPipe(const ratpipe::CreateArgs& a) : Pipe(a)
    {
        if (a.host.seed == 0xDEADull) throw std::runtime_error("ctor refuses this seed");
        instances++;
    }
    ~TestPipe() { instances--; }

    static inline int instances = 0;

    Mode mode() const { return static_cast<Mode>(paramStep(TP_MODE)); }

    void onBlockStart(int) { rendered_ = 0; }

    void onTrigger(const ratpipe::TriggerEvent& e) { record(RATPIPE_EV_TRIGGER, 0, e.velocity, false, false, false); }
    void onNoteOn(const ratpipe::NoteEvent& e)
    {
        if (mode() == ThrowInNoteOn) throw std::runtime_error("noteOn");
        record(RATPIPE_EV_NOTE_ON, e.note, e.velocity, e.slide, e.accent, false);
    }
    void onNoteOff(const ratpipe::NoteEvent& e) { record(RATPIPE_EV_NOTE_OFF, e.note, 0.0f, false, false, e.slideSource); }
    void onAllOff()      { record(RATPIPE_EV_ALL_OFF, 0, 0.0f, false, false, false); }
    void onReleaseHeld() { record(RATPIPE_EV_RELEASE_HELD, 0, 0.0f, false, false, false); }
    void onFlush()       { record(RATPIPE_EV_FLUSH, 0, 0.0f, false, false, false); }

    void render(float* L, float* R, int n)
    {
        const float g = param(TP_GAIN);
        switch (mode())
        {
            case Throw: throw std::runtime_error("render");
            case Nan:   for (int i = 0; i < n; ++i) { L[i] = nanValue(); R[i] = 0.25f; } break;
            case Inf:   for (int i = 0; i < n; ++i) { L[i] = 1.0e30f * 1.0e30f; R[i] = -1.0e30f * 1.0e30f; } break;
            case Loud:  for (int i = 0; i < n; ++i) { L[i] = 50.0f; R[i] = -50.0f; } break;
            default:    for (int i = 0; i < n; ++i) { L[i] = g; R[i] = -g; } break;
        }
        rendered_ += n;
        renderCalls++;
    }

    int seenCount = 0;
    Seen seen[64];
    int renderCalls = 0;

private:
    static float nanValue()
    {
        volatile float z = 0.0f;
        return z / z;
    }
    void record(int type, int note, float vel, bool slide, bool accent, bool slideSource)
    {
        if (seenCount < 64) seen[seenCount++] = Seen{ type, note, rendered_, slide, accent, slideSource, vel };
    }
    int rendered_ = 0;   // samples rendered so far in this block == dispatch position
};
