// MILESTONE 1's GATE: does the string's inharmonicity obey the stiff-string law?
//
// The claim is  f_n = n * f0 * sqrt(1 + B*n^2).  So render a note, find where the
// partials actually landed, and FIT B to them. A fit that is good means the block
// makes real dispersion; a fit that is bad means it makes a detuned mess that only
// sounds interesting. Nothing here trusts the ear.
#include <cstdio>
#include <cmath>
#include <vector>
#include <complex>
#include <algorithm>
#include "dsp/PianoString.h"
#include "dsp/Constants.h"

static const double SR = 48000.0;

// One Goertzel-style DFT bin at an arbitrary frequency, so partials can be located
// to a fraction of an FFT bin without a 2^20 transform.
static double mag_at (const std::vector<float>& x, double hz)
{
    const double w = 2.0 * dsp::kPi * hz / SR;
    std::complex<double> acc {0.0, 0.0};
    const int n = (int) x.size();
    for (int i = 0; i < n; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (2.0 * dsp::kPi * i / (n - 1));   // Hann
        acc += std::complex<double> (x[(size_t) i] * win, 0.0)
             * std::exp (std::complex<double> (0.0, -w * i));
    }
    return std::abs (acc);
}

// Find the local peak near `guess` by golden-section on the magnitude.
static double refine (const std::vector<float>& x, double guess, double span)
{
    double lo = guess - span, hi = guess + span;
    for (int it = 0; it < 40; ++it)
    {
        const double m1 = lo + (hi - lo) * 0.382, m2 = lo + (hi - lo) * 0.618;
        if (mag_at (x, m1) > mag_at (x, m2)) hi = m2; else lo = m1;
    }
    return 0.5 * (lo + hi);
}

struct Fit { double B, f0, err; int used; };

// Least squares on  (f_n / n)^2 = f0^2 + f0^2*B*n^2,  which is linear in n^2.
static Fit fitB (const std::vector<std::pair<int,double>>& partials)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
    for (auto& [k, f] : partials)
    {
        const double x = (double) k * k, y = (f / k) * (f / k);
        sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
    }
    const double den = n * sxx - sx * sx;
    const double slope = (n * sxy - sx * sy) / den;
    const double inter = (sy - slope * sx) / n;
    const double f0 = std::sqrt (std::max (1e-9, inter));
    const double B  = slope / std::max (1e-9, inter);
    double err = 0;
    for (auto& [k, f] : partials)
    {
        const double pred = k * f0 * std::sqrt (1.0 + B * k * k);
        err = std::max (err, std::fabs (1200.0 * std::log2 (f / pred)));   // cents
    }
    return { B, f0, err, n };
}

static std::vector<float> render (float f0, float B, double secs)
{
    dsp::piano::String s;
    s.prepare (SR);
    s.setString (f0, B, 8.0f, 0.22f);
    s.pluck (0.5f);
    std::vector<float> out ((size_t) (SR * secs));
    for (auto& v : out) v = s.process();
    return out;
}

int main (int argc, char** argv)
{
    const bool verbose = argc > 1;
    std::printf ("\nMILESTONE 1: ONE DISPERSIVE STRING\n");
    std::printf ("the claim is  f_n = n*f0*sqrt(1 + B*n^2)\n\n");

    struct Case { float f0; float B; const char* what; };
    const Case cases[] = {
        { 110.0f, 0.0f,      "B = 0, a perfectly harmonic string" },
        { 110.0f, 0.00005f,  "B = 5e-5, a treble-like stiffness at A2" },
        { 110.0f, 0.0002f,   "B = 2e-4" },
        { 110.0f, 0.0006f,   "B = 6e-4, bass-like" },
        {  55.0f, 0.0010f,   "B = 1e-3 at A1, where a real bass string lives" },
        { 440.0f, 0.00005f,  "B = 5e-5 at A4, where a real treble string lives" },
    };

    int fails = 0;
    std::printf ("  %-40s %9s %11s %11s %9s %5s\n", "case", "f0 out", "B wanted", "B measured", "worst", "n");
    for (const auto& c : cases)
    {
        auto x = render (c.f0, c.B, 1.2);
        // FIND THE PEAKS WITHOUT BEING TOLD WHERE THEY ARE. The first version
        // searched a narrow window around each HARMONIC, which cannot find a partial
        // that is 11 per cent sharp - so every large-B case read as a failure of the
        // block when it was a failure of the ruler. This scans the band coarsely,
        // keeps the local maxima, refines each, and only THEN assigns partial numbers
        // by nearest prediction. A peak the law did not predict still shows up, as a
        // bad assignment and a large cents error.
        std::vector<std::pair<int,double>> found;
        {
            const double top = std::min (SR * 0.45, c.f0 * 26.0 * 1.3);
            const double step = c.f0 * 0.02;
            std::vector<std::pair<double,double>> peaks;      // hz, magnitude
            double prev2 = 0, prev1 = 0, prevF = 0;
            for (double f = c.f0 * 0.6; f < top; f += step)
            {
                const double m = mag_at (x, f);
                if (prev1 > prev2 && prev1 > m && prev1 > 1e-4)
                    peaks.push_back ({ refine (x, prevF, step * 1.2), prev1 });
                prev2 = prev1; prev1 = m; prevF = f;
            }
            // assign each partial number to its nearest peak, once
            std::vector<bool> taken (peaks.size(), false);
            for (int k = 1; k <= 20; ++k)
            {
                const double pred = k * c.f0 * std::sqrt (1.0 + (double) c.B * k * k);
                if (pred > SR * 0.45) break;
                int best = -1; double bestD = 1e18;
                for (size_t i = 0; i < peaks.size(); ++i)
                {
                    if (taken[i]) continue;
                    const double dd = std::fabs (peaks[i].first - pred);
                    if (dd < bestD) { bestD = dd; best = (int) i; }
                }
                // accept only if it is closer than half the local partial spacing
                if (best >= 0 && bestD < 0.5 * c.f0)
                { taken[(size_t) best] = true; found.push_back ({ k, peaks[(size_t) best].first }); }
            }
        }
        if (found.size() < 6) { std::printf ("  %-34s  too few partials found\n", c.what); ++fails; continue; }
        const Fit fit = fitB (found);
        const bool wantZero = (c.B <= 0.0f);
        // THE CLAIM IS THE NUMBER: the measured B has to be the B that was asked for,
        // within 15 per cent. That is milestone 1 and it is what makes the block a
        // piano string rather than a detuned delay.
        //
        // THE SHAPE IS A SEPARATE, LOOSER CLAIM, and it is stated at the number the
        // design actually reaches rather than the number anyone would like: identical
        // allpasses put the middle of the series up to ~35 cents off the law at large
        // B. Tightening this threshold is the job of a per-stage coefficient design,
        // and moving it without doing that work would be moving the goalposts.
        const bool ok = wantZero ? (std::fabs (fit.B) < 2.0e-5 && fit.err < 6.0)
                                 : (std::fabs (fit.B - c.B) < 0.15 * c.B && fit.err < 35.0);
        if (! ok) ++fails;
        std::printf ("  %-40s %9.2f %11.6f %11.6f %8.2fc %4d  %s\n",
                     c.what, fit.f0, (double) c.B, fit.B, fit.err, fit.used, ok ? "ok" : "FAIL");
        if (verbose)
            for (auto& [k, f] : found)
                std::printf ("        n=%-3d measured %9.2f  harmonic %9.2f  stretch %+7.1f cents\n",
                             k, f, c.f0 * k, 1200.0 * std::log2 (f / (c.f0 * k)));
    }
    std::printf ("\n  %d failures.\n", fails);
    return fails == 0 ? 0 : 1;
}
