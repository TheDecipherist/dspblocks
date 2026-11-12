// Does the shared oversampler do what it says, and does the super filter's
// third anchor - ONE pole, 6 dB/oct, the bottom of the SLOPE travel - obey the
// same laws the first two do, and say out loud where it cannot?
//
// Behavioural, like test_blocks.cpp and for the same reason: every check below
// is a MEASUREMENT of rendered audio against a number the header claims, not a
// golden file. A golden file would lock in whatever the code did the day it was
// written, and this block's whole argument is that its constants were measured.
//
// It is a separate binary because it renders a lot of audio - the stability
// sweep alone is 6400 settings of 24000 samples - and being able to run it on
// its own while working on the filter is worth a second target.
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <complex>
#include <algorithm>
#include "dsp/Oversampler.h"
#include "dsp/SuperFilter.h"
#include "dsp/Constants.h"

using dsp::filter::SuperFilter;
static const double kSr = 48000.0;
static int failures = 0;

static void check (bool ok, const char* what, double got = 0, double want = 0)
{
    if (ok) { std::printf ("  ok    %s\n", what); return; }
    std::printf ("  FAIL  %s   (got %g, wanted %g)\n", what, got, want);
    ++failures;
}
static double db (double x) { return 20.0 * std::log10 (std::max (1e-30, x)); }

struct Cfg {
    float cut = 1000.f, res = 0.f, shape = 0.f, width = 0.5f, drive = 0.f, slope = 24.f;
    int   type = 0, os = 1;
    float level = 0.4f;
};

/** The magnitude at one frequency, as the ratio of two HANN-WINDOWED
    correlations at exactly that frequency. NOT an rms: an rms over a window that
    is not a whole number of periods reads about 1 per cent high at 40 Hz, which
    is 0.08 dB, which is enough to move a measured corner by 50 cents and to make
    a trim declared in cents look like it missed. The window cancels in the ratio,
    and this meter agrees with the analytic TPT one-pole to eight digits. */
static double tone (const Cfg& c, double f, int warm = 8000, int meas = 16384)
{
    SuperFilter s; s.prepare (kSr); s.setOversampling (c.os); s.setType (c.type);
    const double th = 6.283185307179586 * f / kSr;
    std::complex<double> num (0.0, 0.0), den (0.0, 0.0);
    for (int i = 0; i < warm + meas; ++i)
    {
        const double a = th * i;
        const float x = (float) std::sin (a) * c.level;
        s.set (c.cut, c.res, c.shape, c.width, c.drive, c.slope, c.type);
        const float y = s.process (x);
        if (i >= warm)
        {
            const int n = i - warm;
            const double w = 0.5 - 0.5 * std::cos (2.0 * dsp::kPi * n / meas);
            const std::complex<double> e (std::cos (-a), std::sin (-a));
            num += (double) y * w * e;
            den += (double) x * w * e;
        }
    }
    return std::abs (num) / std::max (1e-300, std::abs (den));
}

/** The largest response within two octaves of the corner, over the 40 Hz
    passband. A peak reads POSITIVE; a monotonic response reads negative, because
    its own maximum in the band is its low edge. */
static double peakOverBody (const Cfg& c)
{
    const double body = tone (c, 40.0);
    double best = 0.0;
    for (int i = 0; i <= 60; ++i)
    {
        const double f = c.cut * std::pow (2.0, -2.0 + 4.0 * i / 60.0);
        if (f > kSr * 0.45) break;
        best = std::max (best, tone (c, f));
    }
    return db (best / body);
}

/** A bandlimited C3 saw, which is check 6's own meter and the only honest one for
    a shape comparison - white noise asks a different question and answers it
    wrongly, because a highpass keeps almost none of a saw and nearly all of the
    noise. */
static std::vector<float> c3saw (double f0 = 130.81, int n = 96000)
{
    std::vector<float> v; v.reserve ((size_t) n);
    double ph = 0.0;
    for (int i = 0; i < n; ++i)
    {
        ph += f0 / kSr; if (ph >= 1.0) ph -= 1.0;
        double x = 0.0;
        for (int h = 1; (double) h * f0 < kSr * 0.45; ++h) x += std::sin (2.0 * dsp::kPi * h * ph) / h;
        v.push_back ((float) (x * 2.0 / dsp::kPi));
    }
    return v;
}

static double sawRms (const Cfg& c, const std::vector<float>& in, int skip = 24000)
{
    SuperFilter f; f.prepare (kSr); f.setOversampling (c.os); f.setType (c.type);
    double acc = 0.0; int m = 0;
    for (size_t i = 0; i < in.size(); ++i)
    {
        f.set (c.cut, c.res, c.shape, c.width, c.drive, c.slope, c.type);
        const float y = f.process (in[i] * (c.level / 0.4f));
        if ((int) i >= skip) { acc += (double) y * y; ++m; }
    }
    return std::sqrt (acc / m);
}

/** Two settings, rendered side by side, compared as RAW BIT PATTERNS. Where a law
    says "exactly the same" this is what says it - 0.000 dB is a tolerance and
    zero differing words is not. */
static long long sameBits (const Cfg& a, const Cfg& b, const std::vector<float>& in)
{
    SuperFilter x, y; x.prepare (kSr); y.prepare (kSr);
    x.setOversampling (a.os); y.setOversampling (b.os);
    long long diff = 0;
    for (size_t i = 0; i < in.size(); ++i)
    {
        x.set (a.cut, a.res, a.shape, a.width, a.drive, a.slope, a.type);
        y.set (b.cut, b.res, b.shape, b.width, b.drive, b.slope, b.type);
        const float ya = x.process (in[i]), yb = y.process (in[i]);
        if (std::memcmp (&ya, &yb, sizeof ya) != 0) ++diff;
    }
    return diff;
}

/** Silence after one impulse: a loop that oscillates is still going. */
static double selfOsc (const Cfg& c)
{
    SuperFilter s; s.prepare (kSr); s.setOversampling (c.os); s.setType (c.type);
    const int n = (int) (2.0 * kSr);
    double sq = 0.0; int m = 0;
    for (int i = 0; i < n; ++i)
    {
        s.set (c.cut, c.res, c.shape, c.width, c.drive, c.slope, c.type);
        const float y = s.process (i == 0 ? 1.0f : 0.0f);
        if (i > n - 12000) { sq += (double) y * y; ++m; }
    }
    return std::sqrt (sq / std::max (1, m));
}

int main()
{
    // ================================================== the oversampler =====
    std::printf ("dsp::Oversampler\n");
    {
        using Os = dsp::Oversampler<16, 4, 1>;
        check (Os::latencySamples() == 16, "the latency is the kernel length in base samples",
               Os::latencySamples(), 16);

        // An impulse through an identity shaper: the peak must land exactly on
        // the declared latency, at EVERY factor. That is the whole promise the
        // consumer's AlignDelay is built on.
        for (int f : { 1, 2, 4 })
        {
            Os o; o.setFactor (f); o.reset();
            int peak = -1; float best = 0.0f;
            for (int i = 0; i < 300; ++i)
            {
                const float y = o.process (0, i == 0 ? 1.0f : 0.0f, [] (float v) noexcept { return v; });
                if (std::fabs (y) > best) { best = std::fabs (y); peak = i; }
            }
            char msg[96]; std::snprintf (msg, sizeof msg,
                "the impulse lands on the declared latency at %dx", f);
            check (peak == Os::latencySamples(), msg, peak, Os::latencySamples());
        }

        // DC gain exactly 1: an oversampler that is not transparent at rest has
        // put a gain into every consumer's signal path.
        for (int f : { 2, 4 })
        {
            Os o; o.setFactor (f); o.reset();
            float y = 0.0f;
            for (int i = 0; i < 2000; ++i) y = o.process (0, 1.0f, [] (float v) noexcept { return v; });
            char msg[64]; std::snprintf (msg, sizeof msg, "DC passes at unity at %dx", f);
            check (std::fabs (y - 1.0f) < 1e-4f, msg, y, 1.0);
        }

        // And the passband. 16 taps per phase is flat to 16 kHz; the header's
        // own table says -0.43 dB at 19 kHz, so this is the claim not the hope.
        for (double hz : { 1000.0, 8000.0, 16000.0 })
        {
            Os o; o.setFactor (4); o.reset();
            double ph = 0.0, sq = 0.0; int n = 0;
            for (int i = 0; i < 40000; ++i)
            {
                ph += hz / kSr; if (ph >= 1.0) ph -= 1.0;
                const float y = o.process (0, (float) std::sin (6.283185307179586 * ph) * 0.5f,
                                           [] (float v) noexcept { return v; });
                if (i > 8000) { sq += (double) y * y; ++n; }
            }
            const double g = db (std::sqrt (sq / n) / (0.5 * 0.70710678));
            char msg[96]; std::snprintf (msg, sizeof msg, "flat at %.0f Hz through 4x", hz);
            check (std::fabs (g) < 0.05, msg, g, 0.0);
        }
    }

    std::printf ("dsp::AlignDelay\n");
    {
        dsp::AlignDelay<32> d;
        d.prepare (16);
        int peak = -1; float best = 0.0f;
        for (int i = 0; i < 100; ++i)
        { const float y = d.process (i == 0 ? 1.0f : 0.0f); if (std::fabs (y) > best) { best = std::fabs (y); peak = i; } }
        check (peak == 16, "delays by exactly what it was prepared with", peak, 16);
        dsp::AlignDelay<32> z; z.prepare (0);
        check (z.process (1.0f) == 1.0f, "and zero is a true pass-through", z.process (1.0f), 1.0);
    }

    // =========================================== the filter's oversampling ===
    std::printf ("SuperFilter - oversampling\n");
    {
        SuperFilter f; f.prepare (kSr);
        check (f.oversampling() == 1 && f.latencySamples() == 0,
               "OFF is the default and reports ZERO latency", f.latencySamples(), 0);
        f.setOversampling (2);
        check (f.oversampling() == 2 && f.latencySamples() == 16,
               "2x reports the kernel's own latency", f.latencySamples(), 16);
        f.setOversampling (4);
        check (f.latencySamples() == 16, "and 4x reports the SAME latency, so switching "
               "does not move a consumer's alignment", f.latencySamples(), 16);
        f.setOversampling (3);
        check (f.oversampling() == 1, "an unsupported factor falls back to off rather than "
               "asserting - a stored program from a future bank must not crash this",
               f.oversampling(), 1);
    }
    {
        // WHAT IT IS FOR: less aliasing. A 9 kHz sine into a 9 kHz corner with
        // the resonance up and DRIVE at the top, the energy under 20 kHz that is
        // neither the fundamental nor an in-band harmonic.
        auto alias = [] (int os)
        {
            const int N = 1 << 14;
            SuperFilter s; s.prepare (kSr); s.setOversampling (os);
            const int k0 = (int) std::llround (9000.0 * N / kSr);
            const double f = k0 * kSr / N;
            std::vector<std::complex<double>> buf ((size_t) N);
            double ph = 0.0;
            for (int i = 0; i < 12000 + N; ++i)
            {
                ph += f / kSr; if (ph >= 1.0) ph -= 1.0;
                s.set (9000.f, 0.90f, 0.f, 0.5f, 1.0f, 24.f, 0);
                const float y = s.process ((float) std::sin (6.283185307179586 * ph) * 0.5f);
                if (i >= 12000) buf[(size_t) (i - 12000)] = y;
            }
            // naive DFT is too slow here; radix-2 in place
            for (size_t i = 1, j = 0; i < (size_t) N; ++i)
            { size_t bit = (size_t) N >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
              if (i < j) std::swap (buf[i], buf[j]); }
            for (size_t len = 2; len <= (size_t) N; len <<= 1)
            {
                const double ang = -2.0 * dsp::kPi / (double) len;
                const std::complex<double> wl (std::cos (ang), std::sin (ang));
                for (size_t i = 0; i < (size_t) N; i += len)
                { std::complex<double> w (1.0, 0.0);
                  for (size_t k = 0; k < len / 2; ++k)
                  { const auto u = buf[i+k], v = buf[i+k+len/2] * w;
                    buf[i+k] = u + v; buf[i+k+len/2] = u - v; w *= wl; } }
            }
            const int top = (int) (20000.0 * N / kSr);
            double fund = 0.0, al = 0.0;
            for (int k = 1; k <= top; ++k)
            {
                const double pw = std::norm (buf[(size_t) k]);
                bool harm = false;
                for (int h = 1; h * k0 <= top; ++h) if (std::abs (k - h * k0) <= 3) { harm = true; break; }
                if (std::abs (k - k0) <= 3) fund += pw; else if (! harm) al += pw;
            }
            return db (std::sqrt (al / std::max (1e-30, fund)));
        };
        const double a1 = alias (1), a2 = alias (2), a4 = alias (4);
        std::printf ("        aliased energy under 20 kHz: %.1f dBc at 1x, %.1f at 2x, %.1f at 4x\n",
                     a1, a2, a4);
        check (a1 - a2 > 12.0, "2x takes a large bite out of the aliasing", a1 - a2, 12.0);
        check (a2 - a4 > 6.0, "and 4x takes another one at the aggressive setting", a2 - a4, 6.0);
    }

    // ================================================= the third anchor ======
    std::printf ("SuperFilter - the 6 dB anchor, ONE pole\n");
    const auto saw = c3saw();
    {
        // 1. it rolls off at 6 dB/oct, and the three anchors are a factor of two
        //    apart in slope. Measured two octaves above a 1 kHz corner, where the
        //    second pole pair is already in play, so every anchor reads a little
        //    past its nominal - and the one-pole reads a little UNDER, because a
        //    single pole's asymptote is approached from below.
        double slopes[3];
        int n = 0;
        for (float sl : { 6.0f, 12.0f, 24.0f })
        {
            Cfg c; c.slope = sl; c.res = 0.f; c.shape = 0.f; c.width = 0.f; c.cut = 1000.f;
            slopes[n++] = db (tone (c, 8000.0) / tone (c, 4000.0));
        }
        std::printf ("        4k -> 8k over a 1 kHz corner: %.2f, %.2f, %.2f dB/oct\n",
                     slopes[0], slopes[1], slopes[2]);
        check (slopes[0] < -5.0 && slopes[0] > -7.5,
               "the third anchor rolls off at 6 dB/oct", slopes[0], -6.0);
        check (slopes[0] > slopes[1] + 5.0 && slopes[1] > slopes[2] + 10.0,
               "and the three anchors halve the slope at each step down",
               slopes[0] - slopes[1], 6.0);

        // 2. IT CANNOT RESONATE, AND THE PROOF IS BITS RATHER THAN DECIBELS.
        //    Two poles are the minimum for a peak; this cell has no loop, so the
        //    resonance knob reaches no coefficient its output is built from - and
        //    at the detent the other branch of the crossfade is multiplied by
        //    `1.0f - 1.0f`. So the whole travel of RESONANCE has to render the
        //    same words, not merely the same level.
        for (float sh : { 0.0f, 2.0f })
        {
            Cfg a; a.slope = 6.f; a.cut = 1000.f; a.shape = sh; a.width = 0.5f; a.res = 0.f;
            Cfg b = a; b.res = 1.0f;
            const long long d = sameBits (a, b, saw);
            char msg[128]; std::snprintf (msg, sizeof msg,
                "RESONANCE reaches not one bit of the output at slope 6, shape %.0f", sh);
            check (d == 0, msg, (double) d, 0.0);
        }
        //    and it reads as no peak on the meter too: a monotonic response's
        //    largest value in the band is its low edge, so this must be NEGATIVE.
        double worstPeak = -99.0;
        for (float sl : { 6.0f, 7.2f, 7.5f })
        for (float r : { 0.0f, 0.5f, 0.8f, 1.0f })
        {
            Cfg c; c.slope = sl; c.cut = 1000.f; c.shape = 0.f; c.width = 0.5f;
            c.res = r; c.level = 0.001f;
            worstPeak = std::max (worstPeak, peakOverBody (c));
        }
        std::printf ("        the tallest thing anywhere near the corner from slope 7.5 down: %+.2f dB\n",
                     worstPeak);
        check (worstPeak < 0.0, "nothing peaks at the bottom of the travel, at any resonance",
               worstPeak, 0.0);
        //    and the peak falls MONOTONICALLY up the leg rather than switching off
        //    at the detent, which is the crossfade doing the work.
        double lastPeak = -99.0; bool rising = true;
        for (float sl : { 6.0f, 7.5f, 9.0f, 10.5f, 12.0f })
        {
            Cfg c; c.slope = sl; c.cut = 1000.f; c.shape = 0.f; c.width = 0.5f;
            c.res = 1.0f; c.level = 0.001f;
            const double p = peakOverBody (c);
            std::printf ("        slope %5.1f, resonance at the top: %+6.2f dB\n", sl, p);
            if (p < lastPeak - 1e-6) rising = false;
            lastPeak = p;
        }
        check (rising, "and the resonance grows monotonically from nothing as the knob leaves 6",
               rising ? 1.0 : 0.0, 1.0);

        // 3. the travel ARRIVES at 6 and holds there, the way it arrives at 12 and
        //    24 - a knob marked with three numbers has to reach all three.
        check (dsp::filter::slopeMorph6 (12.0f) == 0.0f,
               "slope 12 is exactly the 2-pole and nothing else",
               dsp::filter::slopeMorph6 (12.0f), 0.0);
        check (dsp::filter::slopeMorph6 (10.8f) == 0.0f,
               "and the detent at 12 holds a fifth of the way down the lower leg",
               dsp::filter::slopeMorph6 (10.8f), 0.0);
        check (dsp::filter::slopeMorph6 (7.2f) == 1.0f,
               "the detent at 6 holds a fifth of the way up it",
               dsp::filter::slopeMorph6 (7.2f), 1.0);
        check (dsp::filter::slopeMorph6 (6.0f) == 1.0f, "and 6 is the one-pole outright",
               dsp::filter::slopeMorph6 (6.0f), 1.0);
        check (dsp::filter::slopeMorph (12.0f) == 0.0f && dsp::filter::slopeMorph (6.0f) == 0.0f,
               "and the UPPER leg is untouched below 12, which is what keeps the "
               "12 and 24 positions where they were", dsp::filter::slopeMorph (6.0f), 0.0);

        // 4. THE DETENTS READ 0.000 dB FROM THEIR ANCHOR, and again as bits. Each
        //    flat is a fifth of its leg, so every setting inside it must render the
        //    same audio as the printed number at its end.
        struct Flat { float a, b; const char* what; };
        for (const Flat& fl : { Flat { 6.0f, 7.2f,   "the 6 dB detent is flat from 6 to 7.2" },
                                Flat { 10.8f, 12.0f, "the 12 dB detent is flat from 10.8 to 12" },
                                Flat { 12.0f, 14.4f, "and on up to 14.4" },
                                Flat { 21.6f, 24.0f, "the 24 dB detent is flat from 21.6 to 24" } })
        {
            Cfg p; p.cut = 1000.f; p.res = 0.8f; p.shape = 0.f; p.width = 0.5f; p.drive = 0.4f;
            p.slope = fl.a;
            Cfg q = p; q.slope = fl.b;
            const long long d = sameBits (p, q, saw);
            const double ra = sawRms (p, saw), rb = sawRms (q, saw);
            std::printf ("        %-44s %+.4f dB, %lld differing words\n", fl.what, db (rb / ra), d);
            check (d == 0 && std::fabs (db (rb / ra)) < 1e-9, fl.what, (double) d, 0.0);
        }

        // 5. DRIVE DOES NOTHING AT SLOPE 6 and the header says so as a law: the
        //    Nord's headroom shift is the number, and a shift with a matching shift
        //    back is transparent to a LINEAR filter. This cell is linear.
        for (float sh : { 0.0f, 2.0f })
        {
            Cfg a; a.slope = 6.f; a.cut = 1000.f; a.shape = sh; a.width = 0.5f;
            a.res = 1.0f; a.drive = 0.f;
            Cfg b = a; b.drive = 1.0f;
            const long long d = sameBits (a, b, saw);
            char msg[128]; std::snprintf (msg, sizeof msg,
                "DRIVE reaches not one bit of the output at slope 6, shape %.0f", sh);
            check (d == 0, msg, (double) d, 0.0);
        }

        // 6. AND THE TWO-EDGED SHAPES HOLD THE 12 dB ANCHOR, which is the law in
        //    `kOnePoleOneEdged`: a first-order network has no band and no notch,
        //    and [MAN] the Synthex's printed BP1 at 6 dB/oct is already the 12 dB
        //    anchor's own bandpass - `2(y1 - y2)`, two taps. So the bottom of the
        //    travel at the bandpass IS that mode, bit for bit.
        for (float sh : { 1.0f, 3.0f })
        {
            Cfg a; a.slope = 6.f; a.cut = 1000.f; a.shape = sh; a.width = 0.5f;
            a.res = 0.7f; a.drive = 0.3f;
            Cfg b = a; b.slope = 12.0f;
            const long long d = sameBits (a, b, saw);
            char msg[128]; std::snprintf (msg, sizeof msg,
                "shape %.0f has two edges, so slope 6 IS slope 12 there", sh);
            check (d == 0, msg, (double) d, 0.0);
        }
        //    and the one-edged shapes had better NOT be, or the leg does nothing.
        for (float sh : { 0.0f, 2.0f })
        {
            Cfg a; a.slope = 6.f; a.cut = 1000.f; a.shape = sh; a.width = 0.5f; a.res = 0.7f;
            Cfg b = a; b.slope = 12.0f;
            const double ra = sawRms (a, saw), rb = sawRms (b, saw);
            char msg[128]; std::snprintf (msg, sizeof msg,
                "and shape %.0f has one edge, so the leg moves the audio there", sh);
            check (sameBits (a, b, saw) > 0, msg, db (rb / ra), 1.0);
        }

        // 7. no shape goes thin on it. Check 6's own meter and its own 9 dB gate.
        double v[4];
        for (int sIdx = 0; sIdx < 4; ++sIdx)
        { Cfg c; c.cut = 1000.f; c.slope = 6.f; c.shape = (float) sIdx; c.res = 0.f; c.width = 0.5f;
          v[sIdx] = sawRms (c, saw); }
        const double spread = db (*std::max_element (v, v + 4) / *std::min_element (v, v + 4));
        std::printf ("        the four shapes on a C3 saw: %.4f %.4f %.4f %.4f, spread %.2f dB\n",
                     v[0], v[1], v[2], v[3], spread);
        check (spread < 9.0, "no shape goes thin at 6 dB/oct", spread, 9.0);

        // 8. and its DC gain is exactly 1 whatever the resonance, which is the
        //    reason `bodyExponent` does not exist for it: there is no feedback, so
        //    there is no passband loss to give back.
        double worstDc = 0.0;
        for (float r : { 0.0f, 0.4f, 0.8f, 1.0f })
        { Cfg c; c.cut = 1000.f; c.slope = 6.f; c.res = r; c.width = 0.5f; c.level = 0.05f;
          worstDc = std::max (worstDc, std::fabs (db (tone (c, 40.0)))); }
        check (worstDc < 0.02, "and its passband does not move with the resonance at all",
               worstDc, 0.0);

        // 9. IT MUST NOT SING, which is the other side of check 2 and the one a
        //    player would notice: silence after one impulse at EVERY resonance.
        double quiet = 0.0;
        Cfg q; q.slope = 6.f; q.cut = 1000.f; q.shape = 0.f; q.width = 0.5f;
        for (float r : { 0.0f, 0.4f, 0.8f, 1.0f }) { q.res = r; quiet = std::max (quiet, selfOsc (q)); }
        check (quiet < 1e-9, "and it is silent at the TOP of the resonance knob too, "
               "which no other anchor is", quiet, 0.0);
        //    while the 24 dB anchor still does sing, so the gate is not vacuous.
        Cfg t; t.slope = 24.f; t.cut = 1000.f; t.shape = 0.f; t.width = 0.5f; t.res = 1.0f;
        check (selfOsc (t) > 0.05, "the 4-pole anchor still sings at the top of the knob",
               selfOsc (t), 0.05);

        // 10. AND NOTHING OSCILLATES BELOW RESONANCE 0.8 AT ANY SLOPE, which is the
        //     promise every anchor's loop gain was sized against and which has to be
        //     checked ACROSS the travel and not only at its ends - a crossfade of two
        //     stable loops is stable, but a loop whose gain is normalised per anchor
        //     is not automatically under unity at a corner neither anchor was fitted
        //     at. One impulse, then silence, rms over the last quarter of two seconds.
        double loudestTail = 0.0; float tSl = 0.f, tRe = 0.f, tCu = 0.f;
        for (float sl : { 6.f, 9.f, 12.f, 18.f, 24.f })
        for (float re : { 0.4f, 0.79f })
        for (float cu : { 100.f, 1000.f, 8000.f })
        {
            Cfg c; c.slope = sl; c.res = re; c.cut = cu; c.shape = 0.f; c.width = 0.5f;
            const double q = selfOsc (c);
            if (q > loudestTail) { loudestTail = q; tSl = sl; tRe = re; tCu = cu; }
        }
        std::printf ("        loudest tail below resonance 0.8, over five slopes and three "
                     "corners: %.3e (slope %.0f res %.2f cut %.0f)\n",
                     loudestTail, tSl, tRe, tCu);
        check (loudestTail < 1.0e-6, "nothing self-oscillates below resonance 0.8 at any "
               "slope on the travel", loudestTail, 0.0);
    }

    // ==================================================== DRIVE's own law ====
    // [MAN] JD-800 I-96: "higher values of Drive will increase the overall volume
    // as well." Three of the four topologies used to do the opposite - see
    // `driveMakeup`'s block, which has the fault, the cause and the two exponents.
    std::printf ("SuperFilter - DRIVE raises the level or holds it, on every type\n");
    {
        // the make-up has to be exactly 1 at the bottom of the travel, or every
        // stored program at DRIVE 0 moves.
        for (float q : { dsp::filter::kDriveMakeupCascade, dsp::filter::kDriveMakeupSvf })
            check (dsp::filter::driveMakeup (dsp::filter::kDriveBottomGain, q, 1.0f) == 1.0f,
                   "the make-up is exactly 1.000 at DRIVE 0",
                   dsp::filter::driveMakeup (dsp::filter::kDriveBottomGain, q, 1.0f), 1.0);

        // and DRIVE must not LOSE level anywhere in its travel, at the setting the
        // complaint was measured at. A 110 Hz saw at 0.5 is what a consumer hands
        // this block - see kCascadeNodeMin - and the meter is the whole travel and
        // not just its two ends, because the make-up is linear in dB where the loss
        // is not.
        // 0.5 PEAK AT THE FILTER'S INPUT, which is what a consumer hands this block -
        // see kCascadeNodeMin. `sawRms` scales by `level / 0.4`, so the level stays at
        // its own 0.4 and the ramp carries the 0.5.
        std::vector<float> ramp;
        { double ph = 0.0; for (int i = 0; i < 40000; ++i)
          { ph += 110.0 / kSr; if (ph >= 1.0) ph -= 1.0; ramp.push_back ((float) ((2.0 * ph - 1.0) * 0.5)); } }
        const char* names[4] { "SUPER", "SVF", "CASCADE", "TWIN" };
        for (int t = 0; t < dsp::filter::kTypeCount; ++t)
        {
            double worst = 0.0; float wr = 0.f, wd = 0.f;
            for (float res : { 0.0f, 0.2f, 0.6f })
            {
                Cfg base; base.cut = 1000.f; base.slope = 24.f; base.shape = 0.f;
                base.width = 0.5f; base.res = res; base.type = t; base.level = 0.4f;
                base.drive = 0.f;
                const double r0 = sawRms (base, ramp, 12000);
                for (float dr = 0.0625f; dr <= 1.0001f; dr += 0.0625f)
                {
                    Cfg c = base; c.drive = dr;
                    const double d = db (sawRms (c, ramp, 12000) / r0);
                    if (d < worst) { worst = d; wr = res; wd = dr; }
                }
            }
            char msg[160]; std::snprintf (msg, sizeof msg,
                "%s never loses level anywhere in DRIVE's travel", names[t]);
            std::printf ("        %-8s worst %+.2f dB (resonance %.1f, DRIVE %.3f)\n",
                         names[t], worst, wr, wd);
            check (worst > -0.05, msg, worst, 0.0);
        }

        // AND AT RESONANCE 0 THERE IS NO RESONANCE FOR A LIMITER TO REMOVE, so any
        // loss there is a pure gain loss. That is the part a make-up can cancel and
        // the part that must not come back. The residual is bounded rather than
        // zero: `driveMakeup`'s own table shows the exponent that would zero it puts
        // 40 settings of the stability sweep over their bound.
        for (int t = 0; t < dsp::filter::kTypeCount; ++t)
        {
            double worst = 0.0;
            for (float cut : { 80.f, 400.f, 1000.f, 4000.f, 12000.f })
            for (float sh  : { 0.f, 1.f, 2.f, 3.f })
            for (float dr  : { 0.5f, 1.0f })
            {
                Cfg base; base.cut = cut; base.slope = 24.f; base.shape = sh;
                base.width = 0.f; base.res = 0.f; base.type = t; base.level = 0.4f;
                base.drive = 0.f;
                const double r0 = sawRms (base, ramp, 12000);
                if (r0 < 1e-6) continue;
                Cfg c = base; c.drive = dr;
                worst = std::min (worst, db (sawRms (c, ramp, 12000) / r0));
            }
            char msg[160]; std::snprintf (msg, sizeof msg,
                "%s: at resonance 0, DRIVE's pure gain loss is inside the bound", names[t]);
            std::printf ("        %-8s worst at resonance 0: %+.2f dB\n", names[t], worst);
            check (worst > -4.0, msg, worst, -4.0);
        }
    }

    // ========================================= reconfiguration transients ====
    // AN ARTEFACT A PLAYER COULD HEAR AND NO GATE COULD SEE, which is the fault
    // this session turned out to be about. The author, 2026-09-27, on the six-pole build:
    // "a tiny like bell/piano sound that plays on each key hit", "it changes
    // depending on pitch of key. and is more noticeable with the 6 pole filter",
    // "but shape morph between BP and HP and its so audible", and the detail that
    // identified it - "weird and sometimes its not there. I could trigger it a
    // couple of times by turning filter on and off or clicking track on and off or
    // changing poles."
    //
    // SO IT IS A RECONFIGURATION ARTEFACT: a control jumped in one sample, a
    // resonant loop kicked by the step, and a ring at the loop's own crossing -
    // which is a fixed fraction of the CUTOFF, so with key tracking on it follows
    // the key. Filter on/off, TRACK on/off and the pole switch are all that event.
    //
    // THE METER IS THE TRANSIENT ITSELF AND NOT THE ENVELOPE, and that is the whole
    // trick. The input is periodic with an integer period, so a run that SETTLED at
    // the destination and a run that ARRIVED there are sample-aligned: their
    // difference IS what the reconfiguration added and nothing else. An envelope
    // test finds none of this, because the ring is smaller than the steady peak at
    // plenty of settings.
    //
    // AND THE GRID HAS TO REACH MID-LEG, which took two attempts. The first version
    // of this check swept the printed slopes only and PASSED the six-pole build: at
    // slope 36 the steady peak is the six-pole's own resonant output, which is large,
    // so the transient-to-steady RATIO is small. At slope 30, half way up the leg,
    // the steady peak is a mix of two anchors and the transient is still the
    // six-pole's full ring - which is where the fault lives and where it reads
    // +16.05 dB. A gate that only visits the detents is a gate that cannot see a
    // crossfade's own worst case.
    std::printf ("SuperFilter - a jumped control does not ring\n");
    {
        // 150 Hz saw: 320 samples a period, so every window boundary is a period
        // boundary and the two runs are phase-locked.
        auto tick = [] (int i) { return (float) ((2.0 * ((double) (i % 320) / 320.0) - 1.0) * 0.5); };
        const int settle = 12000, win = 12000;
        auto render = [&] (const Cfg& a, const Cfg& b, std::vector<float>& o)
        {
            SuperFilter f; f.prepare (kSr); f.setType (a.type);
            o.assign ((size_t) win, 0.0f);
            for (int i = 0; i < settle + win; ++i)
            {
                const Cfg& c = i < settle ? a : b;
                f.set (c.cut, c.res, c.shape, c.width, c.drive, c.slope, c.type);
                const float y = f.process (tick (i));
                if (i >= settle) o[(size_t) (i - settle)] = y;
            }
        };
        // THE MEASURED TABLE, both builds, this exact grid. `over` is the transient's
        // peak against the steady peak; `holds` is how long it stays a TENTH of the
        // steady peak, which is the honest way to time a ring - an absolute floor
        // instead reads CASCADE's legitimately slow settle at a 200 Hz corner as a
        // 250 ms event, which it is not.
        //
        //     build                      type 0        type 1        type 2/3
        //     the six-pole, all slopes  +16.05 / 67.1  +13.18 / 30.0  +6.83 / 89.8
        //     the six-pole, up to 24     +8.22 /  6.4  +13.18 / 30.0  +6.83 / 89.8
        //     this build                 +8.22 /  6.4  +13.18 / 30.0  +6.83 / 89.8
        //
        // SO THE BOUND THAT CATCHES IT IS THE AMPLITUDE ON TYPE 0 AND NOT THE
        // DURATION. CASCADE's 89.8 ms at a 200 Hz corner is longer than the
        // six-pole's 67.1 and is a slow settle rather than a ring, so a duration
        // bound tight enough to fail 67 ms would fail a filter doing nothing wrong.
        // Type 0's own worst is 8.22 dB, the six-pole's is 16.05, and 11 sits
        // between them with 2.8 dB of margin below and 5.0 above.
        double worst[4] { -600.0, -600.0, -600.0, -600.0 };
        int    held[4] { 0, 0, 0, 0 };
        char   msg[4][160] {};
        for (int t = 0; t < dsp::filter::kTypeCount; ++t)
        for (float sl : { 6.f, 9.f, 12.f, 18.f, 24.f })
        for (float cu : { 200.f, 4000.f })
        for (float re : { 0.3f, 0.7f, 0.9f })
        {
            Cfg base; base.cut = cu; base.res = re; base.shape = 0.f; base.width = 0.5f;
            base.drive = 0.f; base.slope = sl; base.type = t;
            Cfg bp = base; bp.shape = 1.f;
            Cfg hp = base; hp.shape = 2.f;
            Cfg up = base; up.cut = cu * 4.f;
            Cfg w0 = bp;   w0.width = 0.f;
            Cfg w1 = bp;   w1.width = 1.f;
            struct Pair { const char* what; Cfg a, b; };
            const Pair ps[] { { "shape BP -> HP", bp, hp },
                              { "cutoff x4 up",   base, up },
                              { "cutoff x4 down", up, base },
                              { "width 0 -> 1",   w0, w1 } };
            for (const Pair& p : ps)
            {
                std::vector<float> settled, arrived;
                render (p.b, p.b, settled);
                render (p.a, p.b, arrived);
                double pk = 0.0, sp = 0.0;
                for (int i = 0; i < win; ++i)
                {
                    pk = std::max (pk, std::fabs ((double) arrived[(size_t) i] - settled[(size_t) i]));
                    sp = std::max (sp, (double) std::fabs (settled[(size_t) i]));
                }
                int hold = 0;
                for (int i = 0; i < win; ++i)
                    if (std::fabs ((double) arrived[(size_t) i] - settled[(size_t) i]) > 0.1 * sp) hold = i;
                const double rel = db (pk / std::max (1e-12, sp));
                if (rel > worst[t])
                { worst[t] = rel; held[t] = hold;
                  std::snprintf (msg[t], sizeof msg[t], "%s, slope %.0f, cut %.0f, res %.1f",
                                 p.what, sl, cu, re); }
            }
        }
        const char* names[4] { "SUPER", "SVF", "CASCADE", "TWIN" };
        for (int t = 0; t < dsp::filter::kTypeCount; ++t)
            std::printf ("        %-8s worst %+6.2f dB of the steady peak, a tenth of it for "
                         "%5.1f ms   (%s)\n", names[t], worst[t], held[t] * 1000.0 / kSr, msg[t]);
        check (worst[0] < 11.0, "type 0: a jumped control does not ring - this is the bound "
               "the six-pole build fails, at +16.05 dB", worst[0], 11.0);
        for (int t = 1; t < dsp::filter::kTypeCount; ++t)
        {
            char m[128]; std::snprintf (m, sizeof m,
                "%s: a jumped control stays inside its own measured bound", names[t]);
            check (worst[t] < 16.0, m, worst[t], 16.0);
        }
        // AND ONE OF THEM IS OPEN, WHICH THIS PRINTS RATHER THAN HIDES. The SVF's
        // +13.18 dB for 30 ms on a WIDTH jump at resonance 0.9 is its own Q-32 pair
        // kicked by a corner step, it is in every version of this filter, and nothing
        // in this session moved it. Recorded here so the next reader sees a number
        // and not a silence.

        // AND A SLOPE JUMP CANNOT RING AT ALL, which is structural rather than
        // measured-and-hoped: SLOPE reaches no state and no coefficient. Every
        // anchor runs every sample with coefficients built from the CURRENT cutoff,
        // resonance, shape, width and drive, and `slopeDb` enters only through the
        // output mix - so a cell whose output has been discarded for minutes is at
        // every instant in exactly the state it would be in if it had been heard all
        // along. A weight is not a state.
        //
        // THAT IS THE ANSWER TO "is the always-running cell safe to arrive", and the
        // answer is yes for the one-pole as much as it was for the six-pole: the cell
        // that arrives was never anywhere else.
        long long diff = 0, n = 0;
        for (int t = 0; t < dsp::filter::kTypeCount; ++t)
        for (float cu : { 80.f, 1000.f, 12000.f })
        for (float re : { 0.f, 0.9f, 1.f })
        for (float sh : { 0.f, 1.5f, 3.f })
        for (auto pr : { std::pair<float,float> { 24.f, 6.f }, { 6.f, 24.f },
                         { 12.f, 24.f }, { 24.f, 12.f }, { 9.f, 18.f } })
        {
            SuperFilter a, b; a.prepare (kSr); b.prepare (kSr);
            for (int i = 0; i < settle + 4000; ++i)
            {
                const float sa = i < settle ? pr.first : pr.second;
                a.set (cu, re, sh, 0.5f, 0.5f, sa,        t);
                b.set (cu, re, sh, 0.5f, 0.5f, pr.second, t);
                const float ya = a.process (tick (i)), yb = b.process (tick (i));
                if (i >= settle)
                { if (std::memcmp (&ya, &yb, sizeof ya) != 0) ++diff; ++n; }
            }
        }
        std::printf ("        a SLOPE jump against a run that was always at the destination: "
                     "%lld samples, %lld differing words\n", n, diff);
        check (diff == 0, "a SLOPE jump is EXACTLY transient-free, because slope reaches "
               "no state", (double) diff, 0.0);
    }

    // ================================================= the stability sweep ===
    std::printf ("SuperFilter - finite and bounded over the whole surface\n");
    {
        const float cuts[]   { 20.f, 200.f, 3000.f, 12000.f, 20000.f };
        const float reses[]  { 0.f, 0.6f, 0.85f, 1.f };
        const float shapes[] { 0.f, 1.f, 2.f, 3.f };
        // EXTENDED TO THE NEW BOTTOM OF THE TRAVEL, which is where the one-pole's
        // own shape body is a broadband gain - see kBodyHp1, whose 5.00 row returns
        // 8 settings over the bound here and all of them at `cut 20, shape 2, slope 6`.
        const float slopes[] { 6.f, 9.f, 12.f, 18.f, 24.f };
        const float drives[] { 0.f, 1.f };
        const float widths[] { 0.f, 1.f };
        int settings = 0, nonFinite = 0, loud = 0; double worst = 0.0;
        for (int t = 0; t < dsp::filter::kTypeCount; ++t)
        for (float cu : cuts) for (float res : reses) for (float wd : widths)
        for (float sh : shapes) for (float sl : slopes) for (float dr : drives)
        {
            SuperFilter f; f.prepare (kSr);
            double ph = 0.0, sq = 0.0; int m = 0; bool bad = false;
            for (int i = 0; i < 24000; ++i)
            {
                ph += 110.0 / kSr; if (ph >= 1.0) ph -= 1.0;
                f.set (cu, res, sh, wd, dr, sl, t);
                const float y = f.process ((float) (2.0 * ph - 1.0) * 0.4f);
                if (! std::isfinite (y)) { bad = true; break; }
                if (i > 8000) { sq += (double) y * y; ++m; }
            }
            ++settings;
            if (bad) ++nonFinite;
            else
            {
                const double out = m ? std::sqrt (sq / m) : 0.0;
                // the same two bounds megasynth's check 9e uses
                if (out > (res >= 0.8 ? 3.0 : 1.0)) ++loud;
                worst = std::max (worst, out);
            }
        }
        std::printf ("        %d settings swept, loudest %.3f rms out of a 0.231 rms input\n",
                     settings, worst);
        check (nonFinite == 0, "nothing renders a non-finite sample", nonFinite, 0);
        check (loud == 0, "and nothing leaves its rms bound", loud, 0);
    }

    std::printf ("\n%s\n", failures == 0 ? "the filter and the oversampler behave"
                                         : "SOMETHING IS NOT WHAT IT CLAIMS");
    return failures == 0 ? 0 : 1;
}
