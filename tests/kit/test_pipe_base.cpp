// pipe_base tests through the C ABI only (the way a host sees a pipe):
// descriptor shape, param atomics round-trip, exception containment, NaN /
// Inf / loud output sanitising, sample-accurate event splitting.
#include <cmath>
#include <cstring>
#include <vector>
#include "harness.hpp"
#include "test_pipe.hpp"
#include "smoke_pipe.hpp"

extern "C" const RatPipeDescriptor* ratpipe_entry(const RatPipeHostInfo* host);   // defined in smoke_pipe.cpp

namespace {

RatPipeHostInfo hostInfo(uint32_t abiMajor = RATPIPE_ABI_MAJOR, uint64_t seed = 1)
{
    RatPipeHostInfo h;
    std::memset(&h, 0, sizeof h);
    h.struct_size = sizeof(RatPipeHostInfo);
    h.abi_major = abiMajor;
    h.abi_minor = RATPIPE_ABI_MINOR;
    h.host_name = "kit-tests";
    h.host_version = "0.0.0";
    h.sample_rate = 48000.0;
    h.max_block = 512;
    h.seed = seed;
    return h;
}

RatPipeEvent ev(int type, int offset, int note = 60, float vel = 1.0f, uint32_t flags = 0)
{
    RatPipeEvent e;
    std::memset(&e, 0, sizeof e);
    e.struct_size = sizeof(RatPipeEvent);
    e.type = type;
    e.sample_offset = offset;
    e.note = note;
    e.velocity = vel;
    e.step_index = -1;
    e.flags = flags;
    return e;
}

struct Buffers
{
    std::vector<float> L, R;
    explicit Buffers(int n, float fill = 0.0f) : L(static_cast<size_t>(n) + 16, fill), R(static_cast<size_t>(n) + 16, fill) {}
    // guard zones: 8 samples before and after the audible range
    float* l() { return L.data() + 8; }
    float* r() { return R.data() + 8; }
    bool guardsIntact(float fill) const
    {
        for (int i = 0; i < 8; ++i)
            if (L[static_cast<size_t>(i)] != fill || R[static_cast<size_t>(i)] != fill
                || L[L.size() - 1 - static_cast<size_t>(i)] != fill || R[R.size() - 1 - static_cast<size_t>(i)] != fill) return false;
        return true;
    }
};

const RatPipeDescriptor* testDesc() { return ratpipe::Entry<TestPipe>::descriptor(); }

} // namespace

TEST(descriptor_matches_abi_rules)
{
    const RatPipeDescriptor* d = testDesc();
    CHECK(d != nullptr);
    CHECK(d->struct_size == sizeof(RatPipeDescriptor));
    CHECK(d->abi_major == RATPIPE_ABI_MAJOR && d->abi_minor == RATPIPE_ABI_MINOR);
    CHECK(std::strcmp(d->id, "dev.ratmode.sdk_test") == 0);
    CHECK(std::strcmp(d->macro_prefix, "tst") == 0);
    CHECK(d->part_kind == RATPIPE_DRUM);
    CHECK(d->param_count == TP_COUNT);
    for (uint32_t i = 0; i < d->param_count; ++i)
    {
        CHECK(d->params[i].struct_size == sizeof(RatPipeParamDesc));
        for (uint32_t r : d->params[i].reserved) CHECK(r == 0u);
        CHECK(std::strncmp(d->params[i].id, "tst.", 4) == 0);
    }
    CHECK(d->params[TP_MODE].flags == RATPIPE_PARAM_DISCRETE && d->params[TP_MODE].steps == 6);
    CHECK(d->params[TP_GAIN].flags == RATPIPE_PARAM_GAIN_STYLE && d->params[TP_GAIN].max_value == 1.5f);
    CHECK(d->perform_knobs[0] == TP_MODE && d->perform_knobs[2] == RATPIPE_PERFORM_KNOB_NONE);
    CHECK(d->create && d->destroy && d->prepare && d->render && d->set_param && d->get_param);
    CHECK(d->theme == nullptr);
    for (void* r : d->reserved) CHECK(r == nullptr);
    CHECK(std::strlen(d->sdk_version) > 0);
    // built once: same pointer every time
    CHECK(testDesc() == d);

    // the exported entry of the smoke pipe
    const RatPipeHostInfo good = hostInfo();
    const RatPipeDescriptor* s = ratpipe_entry(&good);
    CHECK(s != nullptr && std::strcmp(s->id, "dev.ratmode.sdk_smoke") == 0 && s->param_count == SP_COUNT);
    CHECK(s->params[SP_WAVE].choices != nullptr && std::strcmp(s->params[SP_WAVE].choices[1], "Square") == 0);
    CHECK(s->params[SP_CUT].readout_kind == RATPIPE_READOUT_HZ && s->params[SP_CUT].readout_max == 11000.0f);
    CHECK(s->params[SP_CUT].mutate_lo == 0.15f && s->params[SP_CUT].mutate_hi == 0.95f);
    CHECK((s->params[SP_LEVEL].flags & RATPIPE_PARAM_NOT_MUTABLE) != 0);
    // refusals
    const RatPipeHostInfo badMajor = hostInfo(RATPIPE_ABI_MAJOR + 7);
    CHECK(ratpipe_entry(&badMajor) == nullptr);
    RatPipeHostInfo small = hostInfo(); small.struct_size = 8;
    CHECK(ratpipe_entry(&small) == nullptr);
    CHECK(ratpipe_entry(nullptr) == nullptr);
}

TEST(create_prepare_params_destroy)
{
    const RatPipeDescriptor* d = testDesc();
    const RatPipeHostInfo h = hostInfo(RATPIPE_ABI_MAJOR, 77);
    void* inst = d->create(48000.0, 256, &h);
    CHECK(inst != nullptr);
    CHECK(TestPipe::instances == 1);
    TestPipe* tp = static_cast<TestPipe*>(inst);
    CHECK(tp->hostSeed() == 77 && tp->sampleRate() == 48000.0 && tp->maxBlock() == 256);
    CHECK(std::strcmp(tp->hostInfo().host_name, "kit-tests") == 0);

    // defaults
    CHECK(d->get_param(inst, TP_MODE) == 0.0f);
    CHECK(d->get_param(inst, TP_GAIN) == 1.0f);
    // round trip + clamp + NaN ignore + bad index ignore
    d->set_param(inst, TP_GAIN, 0.25f);  CHECK(d->get_param(inst, TP_GAIN) == 0.25f);
    d->set_param(inst, TP_GAIN, 9.0f);   CHECK(d->get_param(inst, TP_GAIN) == 1.5f);
    d->set_param(inst, TP_GAIN, -1.0f);  CHECK(d->get_param(inst, TP_GAIN) == 0.0f);
    d->set_param(inst, TP_GAIN, 0.5f);
    d->set_param(inst, TP_GAIN, std::nanf("")); CHECK(d->get_param(inst, TP_GAIN) == 0.5f);
    d->set_param(inst, 99, 0.5f); d->set_param(inst, -1, 0.5f);
    CHECK(d->get_param(inst, 99) == 0.0f && d->get_param(inst, -1) == 0.0f);
    // discrete step mapping
    d->set_param(inst, TP_MODE, 0.0f);  CHECK(tp->paramStep(TP_MODE) == 0);
    d->set_param(inst, TP_MODE, 1.0f);  CHECK(tp->paramStep(TP_MODE) == 5);
    d->set_param(inst, TP_MODE, 0.2f);  CHECK(tp->paramStep(TP_MODE) == 1);

    d->prepare(inst, 96000.0, 1024);
    CHECK(tp->sampleRate() == 96000.0 && tp->maxBlock() == 1024);
    d->prepare(inst, 0.0, 0);                                 // ignored, not applied
    CHECK(tp->sampleRate() == 96000.0 && tp->maxBlock() == 1024);

    // null / zero host info: create still works with defaults
    void* inst2 = d->create(0.0, 0, nullptr);
    CHECK(inst2 != nullptr);
    CHECK(static_cast<TestPipe*>(inst2)->sampleRate() == 44100.0 && static_cast<TestPipe*>(inst2)->maxBlock() == 1024);
    d->destroy(inst2);
    // constructor throws -> create returns NULL, nothing leaks
    const RatPipeHostInfo bad = hostInfo(RATPIPE_ABI_MAJOR, 0xDEADull);
    CHECK(d->create(48000.0, 256, &bad) == nullptr);
    CHECK(TestPipe::instances == 1);

    d->destroy(inst);
    CHECK(TestPipe::instances == 0);
    d->destroy(nullptr);
    d->prepare(nullptr, 1.0, 1);
    d->set_param(nullptr, 0, 0.0f);
    CHECK(d->get_param(nullptr, 0) == 0.0f);
    d->render(nullptr, nullptr, nullptr, 0, nullptr, 0, nullptr);
}

TEST(render_is_additive_and_bounded_to_n)
{
    const RatPipeDescriptor* d = testDesc();
    void* inst = d->create(48000.0, 512, nullptr);
    Buffers b(300, 0.125f);
    d->set_param(inst, TP_GAIN, 0.5f);
    d->render(inst, b.l(), b.r(), 300, nullptr, 0, nullptr);
    for (int i = 0; i < 300; ++i) { CHECK(b.l()[i] == 0.625f); CHECK(b.r()[i] == -0.375f); }
    CHECK(b.guardsIntact(0.125f));
    // n larger than kChunk and than max_block is chunked, never overruns
    Buffers big(5000);
    d->render(inst, big.l(), big.r(), 5000, nullptr, 0, nullptr);
    for (int i = 0; i < 5000; ++i) CHECK(big.l()[i] == 0.5f);
    CHECK(big.guardsIntact(0.0f));
    CHECK(static_cast<TestPipe*>(inst)->renderCalls > 1);
    // n <= 0 and null buffers are no-ops
    d->render(inst, nullptr, big.r(), 10, nullptr, 0, nullptr);
    d->render(inst, big.l(), big.r(), 0, nullptr, 0, nullptr);
    d->render(inst, big.l(), big.r(), -5, nullptr, 0, nullptr);
    d->destroy(inst);
}

TEST(exception_in_render_gives_silence_and_does_not_propagate)
{
    const RatPipeDescriptor* d = testDesc();
    void* inst = d->create(48000.0, 512, nullptr);
    TestPipe* tp = static_cast<TestPipe*>(inst);
    d->set_param(inst, TP_MODE, static_cast<float>(TestPipe::Throw) / 5.0f);
    CHECK(tp->mode() == TestPipe::Throw);
    Buffers b(256, 0.0f);
    bool propagated = false;
    try { d->render(inst, b.l(), b.r(), 256, nullptr, 0, nullptr); } catch (...) { propagated = true; }
    CHECK(!propagated);
    for (int i = 0; i < 256; ++i) { CHECK(b.l()[i] == 0.0f); CHECK(b.r()[i] == 0.0f); }
    CHECK(tp->faults() == 1);
    CHECK(b.guardsIntact(0.0f));

    // throw from an event hook: also contained, counted
    d->set_param(inst, TP_MODE, 1.0f);   // ThrowInNoteOn
    CHECK(tp->mode() == TestPipe::ThrowInNoteOn);
    RatPipeEvent e = ev(RATPIPE_EV_NOTE_ON, 10);
    try { d->render(inst, b.l(), b.r(), 256, &e, 1, nullptr); } catch (...) { propagated = true; }
    CHECK(!propagated);
    CHECK(tp->faults() == 2);
    // the first 10 samples (before the throwing event) were rendered normally (gain 1)
    CHECK(b.l()[0] == 1.0f && b.l()[9] == 1.0f && b.l()[10] == 0.0f && b.l()[255] == 0.0f);

    // recovers: back to Normal mode renders again
    d->set_param(inst, TP_MODE, 0.0f);
    Buffers c(64);
    d->render(inst, c.l(), c.r(), 64, nullptr, 0, nullptr);
    CHECK(c.l()[63] == 1.0f);
    // prepare throwing path is also safe (TestPipe never throws there; just exercise null)
    d->destroy(inst);
}

TEST(non_finite_and_loud_output_is_sanitised)
{
    const RatPipeDescriptor* d = testDesc();
    void* inst = d->create(48000.0, 512, nullptr);
    TestPipe* tp = static_cast<TestPipe*>(inst);

    d->set_param(inst, TP_MODE, static_cast<float>(TestPipe::Nan) / 5.0f);
    CHECK(tp->mode() == TestPipe::Nan);
    Buffers b(100, 0.5f);
    d->render(inst, b.l(), b.r(), 100, nullptr, 0, nullptr);
    for (int i = 0; i < 100; ++i) { CHECK(b.l()[i] == 0.5f); CHECK(b.r()[i] == 0.75f); }   // NaN -> 0 added; R fine
    CHECK(tp->nonFiniteSamples() == 100);

    d->set_param(inst, TP_MODE, static_cast<float>(TestPipe::Inf) / 5.0f);
    CHECK(tp->mode() == TestPipe::Inf);
    Buffers c(100);
    d->render(inst, c.l(), c.r(), 100, nullptr, 0, nullptr);
    for (int i = 0; i < 100; ++i) { CHECK(c.l()[i] == 0.0f); CHECK(c.r()[i] == 0.0f); }
    CHECK(tp->nonFiniteSamples() == 300);

    d->set_param(inst, TP_MODE, static_cast<float>(TestPipe::Loud) / 5.0f);
    CHECK(tp->mode() == TestPipe::Loud);
    Buffers l(100);
    d->render(inst, l.l(), l.r(), 100, nullptr, 0, nullptr);
    for (int i = 0; i < 100; ++i) { CHECK(l.l()[i] == 4.0f); CHECK(l.r()[i] == -4.0f); }   // hard-clamped to +/-4 (RatMode's ExtPipe limit)
    CHECK(tp->clampedSamples() == 200);
    CHECK(tp->nonFiniteSamples() == 300);                                                // clamps are counted separately
    d->destroy(inst);
}

TEST(events_are_split_sample_accurately)
{
    const RatPipeDescriptor* d = testDesc();
    void* inst = d->create(48000.0, 512, nullptr);
    TestPipe* tp = static_cast<TestPipe*>(inst);

    RatPipeEvent evs[8] = {
        ev(RATPIPE_EV_NOTE_ON, 0, 48, 0.8f, RATPIPE_EVF_ACCENT),
        ev(RATPIPE_EV_TRIGGER, 0, 0, 0.5f),
        ev(RATPIPE_EV_NOTE_OFF, 17, 48, 0.0f, RATPIPE_EVF_SLIDE_SOURCE),
        ev(RATPIPE_EV_NOTE_ON, 17, 50, 0.6f, RATPIPE_EVF_SLIDE_FROM_PREV),
        ev(RATPIPE_EV_RELEASE_HELD, 255),
        ev(RATPIPE_EV_ALL_OFF, 256),
        ev(RATPIPE_EV_FLUSH, 300),
        ev(RATPIPE_EV_NOTE_ON, 599, 52, 2.0f),           // velocity clamped to 1
    };
    Buffers b(600);
    d->render(inst, b.l(), b.r(), 600, evs, 8, nullptr);
    CHECK(tp->seenCount == 8);
    const int expectPos[8] = { 0, 0, 17, 17, 255, 256, 300, 599 };
    for (int i = 0; i < 8 && i < tp->seenCount; ++i)
    {
        CHECK(tp->seen[i].type == evs[i].type);
        CHECK(tp->seen[i].position == expectPos[i]);
    }
    CHECK(tp->seen[0].accent && !tp->seen[0].slide && tp->seen[0].note == 48 && tp->seen[0].velocity == 0.8f);
    CHECK(tp->seen[2].slideSource);
    CHECK(tp->seen[3].slide && !tp->seen[3].accent && tp->seen[3].note == 50);
    CHECK(tp->seen[7].velocity == 1.0f);
    for (int i = 0; i < 600; ++i) CHECK(b.l()[i] == 1.0f);   // every sample rendered exactly once
    CHECK(b.guardsIntact(0.0f));

    // offsets out of range / unsorted / past the end: clamped, all delivered, every sample rendered once
    tp->seenCount = 0;
    RatPipeEvent odd[4] = { ev(RATPIPE_EV_TRIGGER, 40), ev(RATPIPE_EV_TRIGGER, 5), ev(RATPIPE_EV_TRIGGER, 64), ev(RATPIPE_EV_TRIGGER, 1000) };
    Buffers o(64);
    d->render(inst, o.l(), o.r(), 64, odd, 4, nullptr);
    CHECK(tp->seenCount == 4);
    CHECK(tp->seen[0].position == 40 && tp->seen[1].position == 40 && tp->seen[2].position == 64 && tp->seen[3].position == 64);
    for (int i = 0; i < 64; ++i) CHECK(o.l()[i] == 1.0f);

    // wrong struct_size: events ignored ("not my ABI")
    tp->seenCount = 0;
    RatPipeEvent alien = ev(RATPIPE_EV_TRIGGER, 0); alien.struct_size = sizeof(RatPipeEvent) + 8;
    d->render(inst, o.l(), o.r(), 64, &alien, 1, nullptr);
    CHECK(tp->seenCount == 0);
    // count > 0 with null array: ignored
    d->render(inst, o.l(), o.r(), 64, nullptr, 3, nullptr);
    CHECK(tp->seenCount == 0);

    // transport is copied when its struct_size is sane, zeroed otherwise
    RatPipeTransport tr; std::memset(&tr, 0, sizeof tr);
    tr.struct_size = sizeof tr; tr.playing = 1; tr.bpm = 133.0; tr.sample_rate = 48000.0; tr.tick = 960; tr.ticks_per_beat = 96;
    d->render(inst, o.l(), o.r(), 64, nullptr, 0, &tr);
    CHECK(tp->hasTransport() && tp->transport().bpm == 133.0 && tp->transport().tick == 960);
    tr.struct_size = 4;
    d->render(inst, o.l(), o.r(), 64, nullptr, 0, &tr);
    CHECK(!tp->hasTransport() && tp->transport().bpm == 0.0);
    d->destroy(inst);
}

TEST(smoke_pipe_plays_a_phrase_and_goes_silent)
{
    const RatPipeHostInfo h = hostInfo();
    const RatPipeDescriptor* d = ratpipe_entry(&h);
    CHECK(d != nullptr);
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        void* inst = d->create(sr, 256, &h);
        CHECK(inst != nullptr);
        // extreme params
        d->set_param(inst, SP_CUT, 1.0f); d->set_param(inst, SP_RES, 1.0f); d->set_param(inst, SP_DRIVE, 1.0f); d->set_param(inst, SP_LEVEL, 1.5f);
        float peak = 0.0f; double energy = 0.0;
        std::vector<float> L(256), R(256);
        for (int block = 0; block < 400; ++block)
        {
            std::fill(L.begin(), L.end(), 0.0f); std::fill(R.begin(), R.end(), 0.0f);
            RatPipeEvent e[2];
            int count = 0;
            if (block % 16 == 0 && block < 200) e[count++] = ev(RATPIPE_EV_NOTE_ON, 3, 36 + (block / 16) % 12, 1.0f, (block % 32) ? RATPIPE_EVF_SLIDE_FROM_PREV : RATPIPE_EVF_ACCENT);
            if (block % 16 == 12 && block < 200) e[count++] = ev(RATPIPE_EV_NOTE_OFF, 100, 36 + (block / 16) % 12);
            if (block == 200) e[count++] = ev(RATPIPE_EV_ALL_OFF, 0);
            if (block == 201) e[count++] = ev(RATPIPE_EV_FLUSH, 0);
            if (block % 3 == 0) d->set_param(inst, SP_WAVE, (block / 3) % 2 ? 1.0f : 0.0f);
            d->render(inst, L.data(), R.data(), 256, e, count, nullptr);
            for (int i = 0; i < 256; ++i)
            {
                CHECK(ratpipe::kit::isFinite(L[static_cast<size_t>(i)]) && L[static_cast<size_t>(i)] == R[static_cast<size_t>(i)]);
                const float a = std::fabs(L[static_cast<size_t>(i)]);
                if (a > peak) peak = a;
                if (block < 200) energy += a;
                else if (block > 210) CHECK(a == 0.0f);          // silent after ALL_OFF + FLUSH
            }
        }
        std::printf("    smoke @ %.0f Hz: peak %.3f\n", sr, peak);
        CHECK(energy > 10.0);
        CHECK(peak <= 2.0f);
        CHECK(static_cast<SmokePipe*>(inst)->faults() == 0 && static_cast<SmokePipe*>(inst)->nonFiniteSamples() == 0);
        d->destroy(inst);
    }
}
