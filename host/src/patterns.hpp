// Deterministic note / trigger patterns for the offline host.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <ratpipe/ratpipe_abi.h>

namespace rp {

struct TimedEvent
{
    int64_t      frame = 0;      // absolute frame from the start of the run
    RatPipeEvent ev{};
};

enum class Pattern { Drum16, Acid16, Chords, Silence, Hold, Fuzz };

bool parsePattern(const std::string& name, Pattern& out);
const char* patternName(Pattern p);

// Builds `bars` bars (4/4) at `bpm`. Events are sorted by frame (stable).
//   Drum16  one bar of 16ths: kick-ish hits, a few semitone offsets (TRIGGER for DRUM
//           kinds; the same rhythm as short notes for MONO/POLY)
//   Acid16  16 steps, notes + rests, three slides, accents (NOTE_ON/OFF with
//           SLIDE_FROM_PREV / ACCENT / SLIDE_SOURCE; TRIGGERs with semitone
//           offsets for DRUM kinds)
//   Chords  one root note per bar over four bars of cycle (root only: M1 hosts send no chord)
//   Silence no events
//   Hold    one event at frame 0, never released (for the silence test)
//   Fuzz    dense seeded random events incl. unmatched offs and control events
std::vector<TimedEvent> makePattern(Pattern p, int partKind, double sampleRate, double bpm, int bars, uint64_t seed);

int64_t barFrames(double sampleRate, double bpm);

// A control event (ALL_OFF / RELEASE_HELD / FLUSH) at an absolute frame.
TimedEvent makeControl(int type, int64_t frame);

// True when the pattern leaves no note held at its end (so DC/silence checks are meaningful).
bool patternReleasesAll(Pattern p);

} // namespace rp
