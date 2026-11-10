// The Delay Designer, measured in rendered audio rather than in arithmetic.
//
// A test of the division maths passes while the clock never reaches the engine,
// which is exactly how nine effects units shipped deaf to the transport with
// every test green. So every timing check here pushes an impulse through the
// real block and finds the echo, and every one of them runs at THREE tempos -
// because the check that catches a dead clock is a tempo change, never a
// division change.
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include "dsp/DelayDesigner.h"

static int failures = 0;
static void check (bool ok, const char* what, double got = 0, double want = 0)
{
    if (ok) { std::printf ("  ok    %s\n", what); return; }
    std::printf ("  FAIL  %s   (got %g, wanted %g)\n", what, got, want);
    ++failures;
}

namespace {

constexpr double kSr = 48000.0;

struct Render { std::vector<float> L, R; };

/** Render `n` samples with a single unit impulse at sample 0. */
Render impulse (dsp::DelayDesigner& d, int n)
{
    Render r;
    r.L.assign ((size_t) n, 0.0f);
    r.R.assign ((size_t) n, 0.0f);
    r.L[0] = 1.0f;
    r.R[0] = 1.0f;
    d.process (r.L.data(), r.R.data(), r.L.data(), r.R.data(), n);
    return r;
}

/** A deterministic pseudo-noise source: the same bits every time, so "two
    identical renders match" is a statement about the BLOCK and not about the
    input. No std::rand, whose state is global and survives a reset. */
struct Noise
{
    uint32_t s = 0x13579bdfu;
    float next() { s = s * 1664525u + 1013904223u; return (float) ((int32_t) s) / 2147483648.0f; }
};

struct Peak { int at = 0; float v = 0.0f; };

Peak peakIn (const std::vector<float>& x, int from, int to)
{
    Peak p;
    from = from < 0 ? 0 : from;
    to = to > (int) x.size() ? (int) x.size() : to;
    for (int i = from; i < to; ++i)
        if (std::fabs (x[(size_t) i]) > p.v) { p.v = std::fabs (x[(size_t) i]); p.at = i; }
    return p;
}

double energyIn (const std::vector<float>& x, int from, int to)
{
    double e = 0.0;
    from = from < 0 ? 0 : from;
    to = to > (int) x.size() ? (int) x.size() : to;
    for (int i = from; i < to; ++i) e += (double) x[(size_t) i] * x[(size_t) i];
    return e;
}

/** A crude but honest high-frequency measure: the energy of the first
    difference over the energy of the signal. An impulse reads about 2; a blob
    that has been through a low-pass reads near zero. It answers "did the
    SPECTRUM move", which is the question that separates decay from feedback. */
double hfRatio (const std::vector<float>& x, int from, int to)
{
    double e = 0.0, d = 0.0;
    from = from < 1 ? 1 : from;
    to = to > (int) x.size() ? (int) x.size() : to;
    for (int i = from; i < to; ++i)
    {
        const double v = x[(size_t) i], p = x[(size_t) (i - 1)];
        e += v * v;
        d += (v - p) * (v - p);
    }
    return e > 0.0 ? d / e : 0.0;
}

/** One tap on lane 0, everything else out of the way. */
void oneTap (dsp::DelayDesigner& d, double beats, float vol, float fb,
             float decay, float pan)
{
    d.setTapCount (0, 1);
    d.setTapCount (1, 0);
    d.setTap (0, 0, beats, vol, fb, decay, pan);
}

} // namespace

int main()
{
    // ---- DETERMINISM FIRST -----------------------------------------------
    // Believed before anything else, because every measurement below is only
    // worth what this is worth. A `static` in a DSP function survives every
    // reset and cost a day on the D-50.
    std::printf ("determinism\n");
    {
        dsp::DelayDesigner d;
        d.prepare (kSr, 4.0);
        d.setTempo (120.0);
        d.setMix (0.5f);
        d.setTapCount (0, 3);
        d.setTap (0, 0, 0.5,  0.8f, 0.35f, 0.7f, -0.8f);
        d.setTap (0, 1, 1.0,  0.6f, 0.25f, 0.4f,  0.0f);
        d.setTap (0, 2, 1.75, 0.5f, 0.15f, 0.9f,  0.6f);
        d.setTapCount (1, 2);
        d.setTap (1, 0, 0.6666666666666666, 0.7f, 0.30f, 0.5f,  0.9f);
        d.setTap (1, 1, 1.3333333333333333, 0.4f, 0.20f, 0.2f,  0.3f);

        auto run = [&d] (int n)
        {
            Noise g;
            std::vector<float> L ((size_t) n), R ((size_t) n);
            for (int i = 0; i < n; ++i) { L[(size_t) i] = g.next() * 0.25f;
                                          R[(size_t) i] = g.next() * 0.25f; }
            d.process (L.data(), R.data(), L.data(), R.data(), n);
            return std::pair<std::vector<float>, std::vector<float>> { L, R };
        };

        const int n = 96000;
        auto a = run (n);
        d.reset();
        auto b = run (n);
        const bool sameL = std::memcmp (a.first.data(), b.first.data(),
                                        (size_t) n * sizeof (float)) == 0;
        const bool sameR = std::memcmp (a.second.data(), b.second.data(),
                                        (size_t) n * sizeof (float)) == 0;
        check (sameL && sameR, "two renders after reset() are BIT-identical");

        // And a fresh prepare must land in the same place as a reset, or
        // prepareToPlay is not actually resetting what reset() resets.
        dsp::DelayDesigner e;
        e.prepare (kSr, 4.0);
        e.setTempo (120.0);
        e.setMix (0.5f);
        e.setTapCount (0, 3);
        e.setTap (0, 0, 0.5,  0.8f, 0.35f, 0.7f, -0.8f);
        e.setTap (0, 1, 1.0,  0.6f, 0.25f, 0.4f,  0.0f);
        e.setTap (0, 2, 1.75, 0.5f, 0.15f, 0.9f,  0.6f);
        e.setTapCount (1, 2);
        e.setTap (1, 0, 0.6666666666666666, 0.7f, 0.30f, 0.5f,  0.9f);
        e.setTap (1, 1, 1.3333333333333333, 0.4f, 0.20f, 0.2f,  0.3f);
        Noise g;
        std::vector<float> L ((size_t) n), R ((size_t) n);
        for (int i = 0; i < n; ++i) { L[(size_t) i] = g.next() * 0.25f;
                                      R[(size_t) i] = g.next() * 0.25f; }
        e.process (L.data(), R.data(), L.data(), R.data(), n);
        check (std::memcmp (a.first.data(), L.data(), (size_t) n * sizeof (float)) == 0,
               "a fresh prepare() renders identically to a reset()");
    }

    // ---- a tap lands where the grid says, at THREE tempos ------------------
    std::printf ("tap timing follows the tempo\n");
    {
        const double tempos[3] = { 120.0, 90.0, 140.0 };
        const char*  names[3]  = { "quarter at 120 BPM lands at 500.0 ms",
                                   "quarter at 90 BPM lands at 666.7 ms",
                                   "quarter at 140 BPM lands at 428.6 ms" };
        for (int k = 0; k < 3; ++k)
        {
            dsp::DelayDesigner d;
            d.prepare (kSr, 4.0);
            d.setMix (1.0f);                       // wet only, so the peak IS the tap
            d.setTempo (tempos[k]);
            oneTap (d, 1.0, 1.0f, 0.0f, 1.0f, 0.0f);   // one beat, centre

            const auto r = impulse (d, 96000);
            const auto p = peakIn (r.L, 100, 96000);
            const double want = 60.0 / tempos[k] * kSr;
            check (std::fabs (p.at - want) <= 2.0, names[k], p.at, want);
        }

        // The same designer, tempo MOVED while it runs. This is the check that
        // catches a dead clock; a division change never will.
        dsp::DelayDesigner d;
        d.prepare (kSr, 4.0);
        d.setMix (1.0f);
        d.setTempo (120.0);
        oneTap (d, 1.0, 1.0f, 0.0f, 1.0f, 0.0f);
        check (std::fabs (d.tapSamples (0, 0) - 24000.0) < 0.5,
               "120 BPM: a quarter is 24000 samples", d.tapSamples (0, 0), 24000.0);
        d.setTempo (90.0);
        check (std::fabs (d.tapSamples (0, 0) - 32000.0) < 0.5,
               "moved to 90 BPM: the same tap is now 32000 samples",
               d.tapSamples (0, 0), 32000.0);
        d.setTempo (140.0);
        check (std::fabs (d.tapSamples (0, 0) - 20571.43) < 1.0,
               "moved to 140 BPM: 20571 samples", d.tapSamples (0, 0), 20571.43);
    }

    // ---- two lanes on DIFFERENT shapes ------------------------------------
    // The feature. Nothing may assume the two lanes share a step count or a
    // step length, so this runs straight quarters against quarter triplets and
    // hard-pans them apart so each can be measured on its own channel.
    std::printf ("two lanes, two grids\n");
    {
        for (double bpm : { 120.0, 90.0, 140.0 })
        {
            dsp::DelayDesigner d;
            d.prepare (kSr, 8.0);
            d.setMix (1.0f);
            d.setTempo (bpm);
            d.setTapCount (0, 1);
            d.setTap (0, 0, 1.0, 1.0f, 0.0f, 1.0f, -1.0f);              // 1/4 hard left
            d.setTapCount (1, 1);
            d.setTap (1, 0, 2.0 / 3.0, 1.0f, 0.0f, 1.0f, 1.0f);         // 1/4T hard right

            const auto r = impulse (d, 96000);
            const double wantL = 60.0 / bpm * kSr;
            const double wantR = wantL * 2.0 / 3.0;
            const auto pl = peakIn (r.L, 100, 96000);
            const auto pr = peakIn (r.R, 100, 96000);
            char buf[128];
            std::snprintf (buf, sizeof buf, "%g BPM: the 1/4 lane lands at %.0f", bpm, wantL);
            check (std::fabs (pl.at - wantL) <= 2.0, buf, pl.at, wantL);
            std::snprintf (buf, sizeof buf, "%g BPM: the 1/4T lane lands at %.0f", bpm, wantR);
            check (std::fabs (pr.at - wantR) <= 2.0, buf, pr.at, wantR);
            check (pl.at != pr.at, "the two lanes are NOT on the same grid", pl.at, pr.at);
        }
    }

    // ---- pan puts the energy where it says, at constant power --------------
    std::printf ("pan is constant power\n");
    {
        auto run = [] (float pan)
        {
            dsp::DelayDesigner d;
            d.prepare (kSr, 4.0);
            d.setMix (1.0f);
            d.setTempo (120.0);
            oneTap (d, 1.0, 1.0f, 0.0f, 1.0f, pan);
            return impulse (d, 48000);
        };

        const auto left = run (-1.0f);
        const auto mid  = run (0.0f);
        const auto right = run (1.0f);

        const double lL = peakIn (left.L, 100, 48000).v, lR = peakIn (left.R, 100, 48000).v;
        const double mL = peakIn (mid.L, 100, 48000).v,  mR = peakIn (mid.R, 100, 48000).v;
        const double rL = peakIn (right.L, 100, 48000).v, rR = peakIn (right.R, 100, 48000).v;

        check (lL > 0.9 && lR < 1e-5, "hard left puts everything in L", lR, 0.0);
        check (rR > 0.9 && rL < 1e-5, "hard right puts everything in R", rL, 0.0);
        check (std::fabs (mL - mR) < 1e-5, "centre is equal in both", mL - mR, 0.0);

        // The law itself: gL^2 + gR^2 is 1 everywhere, so a tap does not get
        // quieter as it crosses the middle. A LINEAR fade would read 0.5 here.
        const double powerMid = mL * mL + mR * mR, powerLeft = lL * lL + lR * lR;
        check (std::fabs (powerMid - powerLeft) < 1e-4,
               "centre carries the same POWER as hard left", powerMid, powerLeft);
        check (std::fabs (mL - 0.70710678) < 1e-3,
               "centre gain is 1/sqrt(2), not 0.5", mL, 0.70710678);
    }

    // ---- feedback and decay are different things --------------------------
    // They are the two controls users confuse, so the distinction is measured
    // rather than asserted: feedback moves the SECOND repeat's amplitude and
    // leaves its spectrum; decay moves its SPECTRUM and leaves the FIRST repeat
    // untouched entirely.
    std::printf ("feedback and decay are not the same knob\n");
    {
        auto run = [] (float fb, float decay)
        {
            dsp::DelayDesigner d;
            d.prepare (kSr, 4.0);
            d.setMix (1.0f);
            d.setTempo (120.0);
            oneTap (d, 1.0, 1.0f, fb, decay, 0.0f);
            return impulse (d, 144000);        // three beats of room
        };
        // windows around repeat 1 (24000) and repeat 2 (48000)
        const int r1a = 23000, r1b = 25000, r2a = 47000, r2b = 49000;

        const auto noFb   = run (0.0f, 1.0f);
        const auto lowFb  = run (0.3f, 1.0f);
        const auto highFb = run (0.6f, 1.0f);

        check (peakIn (noFb.L, r2a, r2b).v < 1e-6,
               "no feedback, no second repeat", peakIn (noFb.L, r2a, r2b).v, 0.0);
        const double p1lo = peakIn (lowFb.L, r1a, r1b).v, p1hi = peakIn (highFb.L, r1a, r1b).v;
        check (std::fabs (p1lo - p1hi) < 1e-5,
               "feedback does not touch the FIRST repeat", p1lo, p1hi);
        const double p2lo = peakIn (lowFb.L, r2a, r2b).v, p2hi = peakIn (highFb.L, r2a, r2b).v;
        check (std::fabs (p2hi / p2lo - 2.0) < 0.02,
               "doubling feedback doubles the second repeat", p2hi / p2lo, 2.0);
        check (std::fabs (hfRatio (highFb.L, r2a, r2b) - hfRatio (lowFb.L, r2a, r2b)) < 0.05,
               "feedback leaves the SPECTRUM alone",
               hfRatio (highFb.L, r2a, r2b), hfRatio (lowFb.L, r2a, r2b));

        const auto bright = run (0.6f, 1.0f);
        const auto dark   = run (0.6f, 0.2f);
        check (std::fabs (peakIn (bright.L, r1a, r1b).v - peakIn (dark.L, r1a, r1b).v) < 1e-6,
               "decay does not touch the FIRST repeat either",
               peakIn (dark.L, r1a, r1b).v, peakIn (bright.L, r1a, r1b).v);
        const double hfB = hfRatio (bright.L, r2a, r2b), hfD = hfRatio (dark.L, r2a, r2b);
        check (hfD < hfB * 0.25, "a dark decay takes the top off the tail", hfD, hfB * 0.25);
        // and, because a repeatedly low-passed repeat also loses energy, the
        // tail genuinely dies faster rather than merely getting duller
        const double eB = energyIn (bright.L, r2a, r2b), eD = energyIn (dark.L, r2a, r2b);
        check (eD < eB * 0.5, "and shortens it: less energy in the second repeat", eD, eB * 0.5);
    }

    // ---- a tap past the line is SILENT, not moved -------------------------
    std::printf ("a tap that cannot be reached says so\n");
    {
        dsp::DelayDesigner d;
        d.prepare (kSr, 1.0);          // a one-second line
        d.setMix (1.0f);
        d.setTempo (120.0);
        oneTap (d, 4.0, 1.0f, 0.0f, 1.0f, 0.0f);      // 4 beats = 2 s at 120

        check (! d.tapReachable (0, 0), "the tap reports itself unreachable");
        check (d.unreachableTaps() == 1, "and is counted", d.unreachableTaps(), 1);
        const auto r = impulse (d, 96000);
        check (peakIn (r.L, 100, 96000).v < 1e-6,
               "NOTHING is emitted - it is not clamped to the longest delay",
               peakIn (r.L, 100, 96000).v, 0.0);

        // The same tap becomes reachable when the tempo makes it short enough,
        // and then it lands where the grid says. Reachability is a function of
        // the TEMPO, which is why it cannot be settled once at edit time.
        d.setTempo (300.0);                            // 4 beats = 0.8 s
        check (d.tapReachable (0, 0), "faster tempo brings it back into reach");
        d.reset();
        const auto r2 = impulse (d, 96000);
        const auto p = peakIn (r2.L, 100, 96000);
        check (std::fabs (p.at - 38400.0) <= 2.0, "and it lands at 800 ms", p.at, 38400.0);
    }

    // ---- mix is equal power ----------------------------------------------
    std::printf ("mix is equal power\n");
    {
        // No taps at all, so the output IS the dry gain.
        auto dryGain = [] (float mix)
        {
            dsp::DelayDesigner d;
            d.prepare (kSr, 1.0);
            d.setTempo (120.0);
            d.setMix (mix);
            const auto r = impulse (d, 64);
            return (double) r.L[0];
        };
        check (std::fabs (dryGain (0.0f) - 1.0) < 1e-5, "mix 0 is the source, untouched", dryGain (0.0f), 1.0);
        check (std::fabs (dryGain (1.0f)) < 1e-5, "mix 1 removes the source", dryGain (1.0f), 0.0);
        check (std::fabs (dryGain (0.5f) - 0.70710678) < 1e-4,
               "mix 0.5 is 1/sqrt(2), not 0.5 - a linear blend would dip 3 dB here",
               dryGain (0.5f), 0.70710678);

        // dry^2 + wet^2 = 1 across the whole travel: a wet tap at 0 dB and the
        // dry signal come out summing to the same power at every setting.
        dsp::DelayDesigner d;
        d.prepare (kSr, 4.0);
        d.setTempo (120.0);
        d.setMix (1.0f);
        oneTap (d, 1.0, 1.0f, 0.0f, 1.0f, 0.0f);
        bool allOk = true;
        double worst = 0.0;
        for (int k = 0; k <= 10; ++k)
        {
            const float m = (float) k / 10.0f;
            d.reset();
            d.setMix (m);
            const auto r = impulse (d, 48000);
            const double dry = r.L[0];
            const double wet = peakIn (r.L, 100, 48000).v / 0.70710678;   // undo the centre pan
            const double p = dry * dry + wet * wet;
            worst = std::fabs (p - 1.0) > std::fabs (worst - 1.0) ? p : worst;
            if (std::fabs (p - 1.0) > 2e-3) allOk = false;
        }
        check (allOk, "dry^2 + wet^2 stays at 1 across the whole travel", worst, 1.0);
    }

    // ---- a lane cannot be driven into oscillation, and says when held back -
    std::printf ("feedback headroom\n");
    {
        dsp::DelayDesigner d;
        d.prepare (kSr, 4.0);
        d.setMix (1.0f);
        d.setTempo (120.0);
        d.setTapCount (0, 4);
        for (int i = 0; i < 4; ++i)
            d.setTap (0, i, 0.25 * (i + 1), 0.5f, 0.5f, 1.0f, 0.0f);   // sum = 2.0
        check (std::fabs (d.feedbackHeadroom (0) - 0.95f / 2.0f) < 1e-6,
               "four taps at 0.5 are scaled to a stable 0.95 total",
               d.feedbackHeadroom (0), 0.95 / 2.0);

        const auto r = impulse (d, 480000);      // ten seconds
        check (peakIn (r.L, 400000, 480000).v < peakIn (r.L, 0, 80000).v,
               "and the lane decays instead of running away",
               peakIn (r.L, 400000, 480000).v, peakIn (r.L, 0, 80000).v);

        d.setTapCount (0, 2);
        d.setTap (0, 0, 0.5, 0.5f, 0.2f, 1.0f, 0.0f);
        d.setTap (0, 1, 1.0, 0.5f, 0.2f, 1.0f, 0.0f);
        check (d.feedbackHeadroom (0) == 1.0f,
               "a lane inside the budget is not scaled at all", d.feedbackHeadroom (0), 1.0);
    }

    std::printf ("\n%s\n", failures == 0 ? "the delay designer is what it claims"
                                         : "THE DELAY DESIGNER IS NOT WHAT IT CLAIMS");
    return failures == 0 ? 0 : 1;
}
