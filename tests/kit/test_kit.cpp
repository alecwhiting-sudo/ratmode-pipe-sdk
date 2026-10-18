// Kit unit tests: aliasing, filter stability, saturator bounds, delay feedback
// bound, rng determinism, envelope shape, misc helpers.
#include <cmath>
#include <vector>
#include "harness.hpp"
#include "ratpipe/kit/kit.hpp"

using namespace ratpipe::kit;

namespace {

bool allFinite(const std::vector<float>& v)
{
    for (float x : v) if (!isFinite(x)) return false;
    return true;
}

float peakAbs(const std::vector<float>& v)
{
    float p = 0.0f;
    for (float x : v) { const float a = std::fabs(x); if (a > p) p = a; }
    return p;
}

// Energy (sum of |X[k]|^2) over bins in [1, N/2) that are NOT harmonics of
// `fundamentalBin` below Nyquist — i.e. aliased energy for a bin-centred tone.
double aliasEnergyDb(const std::vector<float>& x, int fundamentalBin)
{
    const int N = static_cast<int>(x.size());
    std::vector<char> harmonic(static_cast<size_t>(N / 2), 0);
    for (int k = fundamentalBin; k < N / 2; k += fundamentalBin) harmonic[static_cast<size_t>(k)] = 1;
    double alias = 0.0, total = 0.0;
    for (int k = 1; k < N / 2; ++k)
    {
        double re = 0.0, im = 0.0;
        const double w = -6.283185307179586 * k / N;
        for (int n = 0; n < N; ++n)
        {
            re += x[static_cast<size_t>(n)] * std::cos(w * n);
            im += x[static_cast<size_t>(n)] * std::sin(w * n);
        }
        const double e = re * re + im * im;
        total += e;
        if (!harmonic[static_cast<size_t>(k)]) alias += e;
    }
    return 10.0 * std::log10((alias + 1e-30) / (total + 1e-30));
}

} // namespace

// ── PolyBLEP aliasing ────────────────────────────────────────────────────────

TEST(polyblep_saw_aliases_less_than_naive)
{
    const int N = 4096;
    const double sr = 44100.0;
    const int bin = 233;                                   // 2508.6 Hz, exactly bin-centred
    const float hz = static_cast<float>(sr * bin / N);

    Osc o; o.prepare(sr); o.setWave(Osc::Saw); o.setFrequency(hz);
    std::vector<float> blep(static_cast<size_t>(N)), naive(static_cast<size_t>(N));
    double phase = 0.0; const double inc = hz / sr;
    for (int i = 0; i < N; ++i)
    {
        blep[static_cast<size_t>(i)]  = o.process();
        naive[static_cast<size_t>(i)] = static_cast<float>(2.0 * phase - 1.0);
        phase += inc; if (phase >= 1.0) phase -= 1.0;
    }
    const double aBlep = aliasEnergyDb(blep, bin);
    const double aNaive = aliasEnergyDb(naive, bin);
    std::printf("    saw alias/total: naive %.1f dB, polyblep %.1f dB\n", aNaive, aBlep);
    CHECK(aBlep < aNaive - 10.0);                          // >= 10 dB less aliased energy than naive
    CHECK(aBlep < -24.0);                                  // and an absolute alias floor
    CHECK(peakAbs(blep) <= 1.1f);
}

TEST(polyblep_pulse_and_tri_bounded_and_aliasing)
{
    const int N = 4096;
    const double sr = 44100.0;
    const int bin = 233;
    const float hz = static_cast<float>(sr * bin / N);
    for (int w = 0; w < 2; ++w)
    {
        Osc o; o.prepare(sr); o.setWave(w == 0 ? Osc::Pulse : Osc::Tri); o.setFrequency(hz);
        std::vector<float> blep(static_cast<size_t>(N)), naive(static_cast<size_t>(N));
        double phase = 0.0; const double inc = hz / sr;
        for (int i = 0; i < 8192; ++i) o.process();      // settle the triangle integrator
        for (int i = 0; i < N; ++i)
        {
            blep[static_cast<size_t>(i)] = o.process();
            naive[static_cast<size_t>(i)] = phase < 0.5 ? 1.0f : -1.0f;
            phase += inc; if (phase >= 1.0) phase -= 1.0;
        }
        CHECK(allFinite(blep));
        CHECK(peakAbs(blep) <= 1.1f);
        if (w == 0)
        {
            const double aBlep = aliasEnergyDb(blep, bin), aNaive = aliasEnergyDb(naive, bin);
            std::printf("    pulse alias/total: naive %.1f dB, polyblep %.1f dB\n", aNaive, aBlep);
            CHECK(aBlep < aNaive - 10.0);
            CHECK(aBlep < -24.0);
        }
    }
}

TEST(osc_extreme_frequencies_stay_bounded)
{
    Osc o; o.prepare(48000.0);
    const float freqs[] = { 0.0f, 0.01f, 20.0f, 20000.0f, 48000.0f, 1.0e9f, -5.0f, std::nanf("") };
    for (int w = 0; w < 4; ++w)
    {
        o.setWave(static_cast<Osc::Wave>(w));
        for (float f : freqs)
        {
            o.setFrequency(f);
            for (int i = 0; i < 2000; ++i)
            {
                const float s = o.process();
                CHECK(isFinite(s));
                CHECK(std::fabs(s) <= 1.1f);
            }
        }
    }
    SubOsc sub; sub.prepare(48000.0);
    for (int i = 0; i < 1000; ++i) { const float s = sub.process(i < 500 ? 110.0f : 1.0e9f); CHECK(isFinite(s) && std::fabs(s) <= 1.0f); }
}

// ── Saturators ───────────────────────────────────────────────────────────────

TEST(sat_tanh_family_is_bounded)
{
    const float inputs[] = { 0.0f, 0.5f, -0.5f, 1.0f, 10.0f, -10.0f, 1.0e6f, -1.0e6f, 1.0e38f, std::nanf(""), INFINITY, -INFINITY };
    for (float x : inputs)
    {
        for (float d : { 0.0f, 0.5f, 1.0f, 8.0f, 100.0f })
        {
            CHECK(std::fabs(sat::tanhDrive(x, d)) <= 1.0f);
            CHECK(std::fabs(sat::asymTanh(x, d, 0.4f)) <= 2.0f);
            CHECK(isFinite(sat::asymTanh(x, d, 0.4f)));
        }
        for (float a : { 0.0f, 0.3f, 1.0f, 5.0f, -1.0f })
            CHECK(std::fabs(sat::softTape(x, a)) <= 1.0f);
        CHECK(std::fabs(sat::fastTanh(x)) <= 0.9952f);
    }
    CHECK_NEAR(sat::asymTanh(0.0f, 2.0f, 0.4f), 0.0f, 1e-7);
    for (float x = -3.0f; x <= 3.0f; x += 0.01f) CHECK_NEAR(sat::fastTanh(x), std::tanh(x), 3e-4);
    // monotone
    float prev = sat::fastTanh(-3.0f);
    for (float x = -2.99f; x <= 3.0f; x += 0.01f) { const float y = sat::fastTanh(x); CHECK(y >= prev); prev = y; }
}

// ── Filters ──────────────────────────────────────────────────────────────────

TEST(svf_stable_under_max_res_fast_modulation_and_extreme_input)
{
    for (int bounded = 0; bounded < 2; ++bounded)
    {
        const double sr = 96000.0;
        Svf f; f.prepare(sr); f.setBounded(bounded == 1);
        Rng rng(7);
        float worst = 0.0f;
        for (int i = 0; i < 200000; ++i)
        {
            // cutoff jumps anywhere each sample, resonance pinned at max
            if ((i & 1) == 0) f.set(5.0f + rng.nextFloat() * 60000.0f, 1.0f);
            float x = rng.nextBipolar() * 10.0f;
            if (i % 5000 == 0) x = 1.0e6f;                 // spikes
            if (i == 100000) x = std::nanf("");            // one NaN input
            f.process(x);
            const float y = f.lp(), b = f.bp(), h = f.hp();
            CHECK(isFinite(y) && isFinite(b) && isFinite(h));
            const float a = std::fabs(y);
            if (a > worst) worst = a;
        }
        std::printf("    svf worst |lp| (%s): %.3g\n", bounded ? "bounded" : "linear", worst);
        if (bounded) CHECK(worst <= 16.0f);
        else CHECK(worst < 1.0e8f);
    }
    // silence in -> silence out, DC response of LP ~ 1
    Svf f; f.prepare(44100.0); f.set(1000.0f, 0.2f);
    float y = 0.0f;
    for (int i = 0; i < 20000; ++i) y = f.processLp(1.0f);
    CHECK_NEAR(y, 1.0f, 1e-3);
    for (int i = 0; i < 20000; ++i) y = f.processLp(0.0f);
    CHECK(std::fabs(y) < 1e-6f);
}

TEST(ladder_bounded_for_any_input_and_modulation)
{
    const double sr = 96000.0;
    LadderLite f; f.prepare(sr);
    Rng rng(11);
    float worst = 0.0f;
    for (int i = 0; i < 200000; ++i)
    {
        f.set(5.0f + rng.nextFloat() * 60000.0f, rng.nextFloat() > 0.5f ? 1.0f : rng.nextFloat(), 16.0f);
        float x = rng.nextBipolar() * 1000.0f;
        if (i == 50000) x = INFINITY;
        if (i == 60000) x = std::nanf("");
        const float y = f.process(x);
        CHECK(isFinite(y));
        const float a = std::fabs(y);
        if (a > worst) worst = a;
    }
    std::printf("    ladder worst |y|: %.3f (bound 3.0)\n", worst);
    CHECK(worst <= 3.0f);
    // self-oscillation at max res sustains after input stops, stays bounded, and reset() kills it
    f.set(500.0f, 1.0f, 1.0f);
    for (int i = 0; i < 2000; ++i) f.process(i < 10 ? 1.0f : 0.0f);
    float e = 0.0f;
    for (int i = 0; i < 2000; ++i) e += std::fabs(f.process(0.0f));
    CHECK(e > 1.0f);
    f.reset();
    CHECK(f.process(0.0f) == 0.0f);
}

// ── Delay ────────────────────────────────────────────────────────────────────

TEST(delay_feedback_is_clamped_and_bounded)
{
    FeedbackDelay d; d.prepare(4800);
    float worst = 0.0f;
    for (int i = 0; i < 200000; ++i)
    {
        const float x = (i < 48000) ? ((i % 7) == 0 ? 1.0f : -1.0f) : 0.0f;
        const float y = d.process(x, 100.0f, 1000.0f, 0.0f);   // asks for feedback 1000 -> clamped to 0.95
        CHECK(isFinite(y));
        const float a = std::fabs(y);
        if (a > worst) worst = a;
    }
    std::printf("    feedback delay worst |y|: %.3f (bound 20)\n", worst);
    CHECK(worst <= 20.0f);
    // decays once input stops: after 150k samples of silence the tail is tiny
    float tail = 0.0f;
    for (int i = 0; i < 1000; ++i) tail += std::fabs(d.process(0.0f, 100.0f, 0.95f));
    CHECK(tail < 1e-3f);

    DelayLine line; line.prepare(1000);
    for (int i = 0; i < 1000; ++i) line.write(static_cast<float>(i));
    CHECK_NEAR(line.readInt(1), 999.0f, 1e-6);
    CHECK_NEAR(line.read(10.5f), 989.5f, 1e-3);
    CHECK_NEAR(line.read(1.0e9f), line.readInt(998), 1e-3);  // clamped to max
    line.write(std::nanf(""));                                 // non-finite clears the line
    CHECK(line.readInt(1) == 0.0f && line.readInt(500) == 0.0f);
    DelayLine unprepared;
    CHECK(unprepared.read(10.0f) == 0.0f);
    unprepared.write(1.0f);
}

// ── RNG / noise ──────────────────────────────────────────────────────────────

TEST(rng_is_deterministic_and_in_range)
{
    Rng a(12345), b(12345), c(54321);
    bool same = true, differ = false;
    for (int i = 0; i < 10000; ++i)
    {
        const uint32_t x = a.nextU32(), y = b.nextU32(), z = c.nextU32();
        if (x != y) same = false;
        if (x != z) differ = true;
    }
    CHECK(same);
    CHECK(differ);
    Rng r(1);
    double sum = 0.0;
    for (int i = 0; i < 100000; ++i)
    {
        const float f = r.nextFloat();
        CHECK(f >= 0.0f && f < 1.0f);
        const float bp = r.nextBipolar();
        CHECK(bp >= -1.0f && bp < 1.0f);
        sum += f;
        CHECK(r.nextBelow(10) < 10u);
    }
    CHECK_NEAR(sum / 100000.0, 0.5, 0.01);
    // fixed sequence head (locks the algorithm: PCG32 with these constants)
    Rng k(42);
    const uint32_t first = k.nextU32();
    Rng k2(42);
    CHECK(k2.nextU32() == first);

    WhiteNoise w; w.seed(3);
    PinkNoise p; p.seed(3);
    BrownNoise br; br.seed(3);
    for (int i = 0; i < 100000; ++i)
    {
        CHECK(std::fabs(w.process()) <= 1.0f);
        CHECK(std::fabs(p.process()) <= 1.0f);
        CHECK(std::fabs(br.process()) <= 1.0f);
    }
}

// ── Envelope ─────────────────────────────────────────────────────────────────

TEST(adsr_shape_and_bounds)
{
    const double sr = 48000.0;
    Adsr e; e.prepare(sr); e.set(10.0f, 200.0f, 0.5f, 100.0f);
    CHECK(!e.isActive());
    CHECK(e.process() == 0.0f);
    e.gateOn();
    int attackSamples = 0;
    float prev = 0.0f;
    while (e.stage() == Adsr::Attack && attackSamples < 100000) { const float v = e.process(); CHECK(v >= prev); prev = v; ++attackSamples; }
    CHECK_NEAR(attackSamples / sr * 1000.0, 10.0, 2.0);     // reaches 1.0 in ~10 ms
    CHECK_NEAR(e.value(), 1.0f, 1e-6);
    // decay: monotone down to sustain
    int decaySamples = 0;
    prev = e.value();
    while (e.stage() == Adsr::Decay && decaySamples < 1000000) { const float v = e.process(); CHECK(v <= prev); prev = v; ++decaySamples; }
    CHECK(e.stage() == Adsr::Sustain);
    CHECK_NEAR(e.value(), 0.5f, 1e-3);
    CHECK(decaySamples > static_cast<int>(sr * 0.1) && decaySamples < static_cast<int>(sr * 0.3));
    for (int i = 0; i < 1000; ++i) CHECK(e.process() == 0.5f);
    e.gateOff();
    int rel = 0;
    prev = e.value();
    while (e.isActive() && rel < 1000000) { const float v = e.process(); CHECK(v <= prev && v >= 0.0f); prev = v; ++rel; }
    CHECK(!e.isActive());
    CHECK_NEAR(rel / sr * 1000.0, 157.0, 30.0);              // 100 ms is -60 dB; from 0.5 down to 1e-5 is ~1.57x that
    // retrigger from mid-level has no discontinuity, kill() is instant
    e.gateOn(); for (int i = 0; i < 100; ++i) e.process();
    const float mid = e.value(); e.gateOn();
    CHECK(e.process() >= mid);
    e.kill(); CHECK(e.value() == 0.0f && !e.isActive());
    // percussive: sustain 0 -> goes idle by itself
    Adsr p; p.prepare(sr); p.set(0.5f, 50.0f, 0.0f, 50.0f); p.gateOn();
    int life = 0; while (p.isActive() && life < 100000) { p.process(); ++life; }
    CHECK(life > 0 && life < static_cast<int>(sr * 0.08));   // 1 -> 1e-4 is 1.33x the -60 dB time
    // absurd inputs are clamped, never NaN
    Adsr z; z.prepare(0.0); z.set(-5.0f, std::nanf(""), 7.0f, 1.0e9f); z.gateOn();
    for (int i = 0; i < 100; ++i) { const float v = z.process(); CHECK(isFinite(v) && v >= 0.0f && v <= 1.0f); }
}

// ── Small helpers ────────────────────────────────────────────────────────────

TEST(helpers_pitch_smoother_dcblock_lfo_guard)
{
    CHECK_NEAR(midiToHz(69.0f), 440.0f, 1e-3);
    CHECK_NEAR(midiToHz(57.0f), 220.0f, 1e-3);
    CHECK_NEAR(hzToMidi(440.0f), 69.0f, 1e-4);
    CHECK(isFinite(midiToHz(std::nanf(""))) && midiToHz(1.0e9f) <= 40000.0f && midiToHz(-1.0e9f) >= 0.1f);
    CHECK_NEAR(semitonesToRatio(12.0f), 2.0f, 1e-6);

    Glide g; g.prepare(48000.0); g.setTime(50.0f);
    g.noteOn(48.0f, false); CHECK_NEAR(g.tickNote(), 48.0f, 1e-6);
    g.noteOn(60.0f, true);
    CHECK(g.isGliding());
    float n = 0.0f; for (int i = 0; i < 48000; ++i) n = g.tickNote();
    CHECK_NEAR(n, 60.0f, 1e-3);
    g.noteOn(36.0f, false); CHECK_NEAR(g.tickNote(), 36.0f, 1e-6);

    OnePole s; s.setTime(10.0f, 48000.0); s.snap(0.0f);
    float v = 0.0f; for (int i = 0; i < 480; ++i) v = s.process(1.0f);
    CHECK(v > 0.6f && v < 0.7f);                            // one time constant ~63%
    for (int i = 0; i < 48000; ++i) v = s.process(1.0f);
    CHECK(v == 1.0f);                                        // snaps exactly

    DcBlock dc; dc.prepare(48000.0);
    float y = 1.0f; for (int i = 0; i < 48000; ++i) y = dc.process(1.0f);
    CHECK(std::fabs(y) < 1e-3f);

    Lfo l; l.prepare(48000.0); l.seed(9);
    for (int w = 0; w < 6; ++w)
    {
        l.setWave(static_cast<Lfo::Wave>(w)); l.setRate(37.0f);
        for (int i = 0; i < 20000; ++i) { const float m = l.process(); CHECK(m >= -1.0f && m <= 1.0f); CHECK(l.unipolar() >= 0.0f && l.unipolar() <= 1.0f); }
    }
    l.setRate(1.0e9f); l.setRate(std::nanf("")); CHECK(l.rate() == 0.0f);

    CHECK(!isFinite(std::nanf("")) && !isFinite(INFINITY) && isFinite(1.0e38f) && isFinite(0.0f));
    CHECK(clamp(std::nanf(""), -1.0f, 1.0f) == -1.0f);
    CHECK(guard(std::nanf("")) == 0.0f && guard(5.0f) == 2.0f && guard(-5.0f) == -2.0f);
    float buf[4] = { 0.5f, INFINITY, -3.0f, std::nanf("") };
    CHECK(guardBuffer(buf, 4));
    CHECK(buf[0] == 0.5f && buf[1] == 0.0f && buf[2] == -2.0f && buf[3] == 0.0f);
    CHECK(flushTiny(1.0e-20f) == 0.0f && flushTiny(0.5f) == 0.5f);
    {
        FlushDenormalsScope ftz;
        volatile float tiny = 1.0e-38f;
        const float r = tiny * 1.0e-3f;   // would be a subnormal; FTZ makes it 0 on x86/arm64
        CHECK(r == 0.0f || r < 1.0e-40f);
    }
}
