// Does the new tank actually beat the old one? The claims in
// ~/projects/eproms/docs/reverb-research.md section 8.6, measured.
#include "dsp/ReverbFDN.h"
#include "dsp/Reverb.h"
#include "dsp/MeasureDecay.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

namespace {
constexpr double SR = 48000.0;

template <typename T>
std::vector<float> ir (T& tank, double secs)
{
    const int n = (int) (SR * secs);
    std::vector<float> out ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        float l = 0.0f, r = 0.0f;
        tank.process (i == 0 ? 1.0f : 0.0f, i == 0 ? 1.0f : 0.0f, l, r);
        out[(size_t) i] = 0.5f * (l + r);
    }
    return out;
}

double echoDensity (const std::vector<float>& x, int from, int len)
{
    int cross = 0;
    for (int i = from + 1; i < from + len && i < (int) x.size(); ++i)
        if ((x[(size_t) i] >= 0.0f) != (x[(size_t) i - 1] >= 0.0f)) ++cross;
    return ((double) cross / ((double) len / SR)) / (SR * 0.5);
}

std::pair<double,double> combPeak (const std::vector<float>& x, int from, int len)
{
    double e = 0.0;
    for (int i = from; i < from + len && i < (int) x.size(); ++i)
        e += (double) x[(size_t) i] * x[(size_t) i];
    if (e <= 1e-18) return { 0.0, 0.0 };
    double best = 0.0, lag = 0.0;
    for (int L = (int) (SR * 0.001); L < (int) (SR * 0.120); ++L)
    {
        double s = 0.0;
        for (int i = from; i + L < from + len && i + L < (int) x.size(); ++i)
            s += (double) x[(size_t) i] * x[(size_t) (i + L)];
        if (s / e > best) { best = s / e; lag = L / SR * 1000.0; }
    }
    return { best, lag };
}
} // namespace

int main()
{
    const int from = (int) (SR * 0.060), len = (int) (SR * 0.400);

    std::printf ("TAIL QUALITY: the stock tank against the new one\n");
    std::printf ("echo density 1.0 = as dense as noise; comb peak over ~0.3 is audible flutter\n\n");
    std::printf ("%-26s %14s %12s %10s\n", "tank", "echo density", "comb peak", "at lag");

    for (const char* which : { "room", "plate", "hall" })
    {
        dsp::ReverbTank old;
        dsp::ReverbTopology t = std::string (which) == "room"  ? dsp::topologies::room()
                              : std::string (which) == "plate" ? dsp::topologies::plate()
                                                               : dsp::topologies::hall();
        old.prepare (SR, t);
        old.setDecaySeconds (2.5);
        const auto a = ir (old, 2.0);
        const double d = echoDensity (a, from, len);
        const auto [p, l] = combPeak (a, from, len);
        std::printf ("  ReverbTank %-14s %13.3f %12.3f %7.2f ms%s\n",
                     which, d, p, l, p > 0.30 ? "   COMB" : "");
    }

    dsp::FDNTank fdn;
    fdn.prepare (SR);
    fdn.setDecaySeconds (2.5);
    fdn.setLowMultiplier (1.0);
    fdn.setHighMultiplier (1.0);
    const auto b = ir (fdn, 2.0);
    const double d2 = echoDensity (b, from, len);
    const auto [p2, l2] = combPeak (b, from, len);
    std::printf ("  FDNTank %-17s %13.3f %12.3f %7.2f ms%s\n",
                 "(flat decay)", d2, p2, l2, p2 > 0.30 ? "   COMB" : "");

    // THE TEST DATTORRO AND THE STOCK TANK CANNOT PASS: ask for a LONGER low
    // decay than high, and measure that the tail actually does it.
    std::printf ("\nPER-BAND DECAY, which is the capability the others do not have\n");
    std::printf ("%-34s %12s %12s\n", "asked for", "measured", "error");
    struct Want { double mid, lowMul, highMul; };
    for (const Want& w : { Want{2.0, 2.0, 0.5}, Want{2.0, 1.0, 1.0}, Want{3.0, 1.5, 0.4} })
    {
        dsp::FDNTank f;
        f.prepare (SR);
        f.setDecaySeconds (w.mid);
        f.setLowMultiplier (w.lowMul);
        f.setHighMultiplier (w.highMul);
        f.setHighCut (18000.0);
        const auto x = ir (f, 12.0);
        const auto lo = dsp::measureBandT60 (x, SR, 80.0, 200.0);
        const auto md = dsp::measureBandT60 (x, SR, 800.0, 1600.0);
        const auto hi = dsp::measureBandT60 (x, SR, 5000.0, 9000.0);
        auto row = [&] (const char* nm, double want, const dsp::DecayResult& got)
        {
            const double e = got.usable ? std::fabs (got.t60 - want) / want * 100.0 : -1.0;
            std::printf ("  %-32s %9.2f s %9.2f s %7.0f%%  r2 %.3f\n",
                         nm, want, got.usable ? got.t60 : 0.0, e, got.r2);
        };
        std::printf ("  mid %.1f s, low x%.2f, high x%.2f\n", w.mid, w.lowMul, w.highMul);
        row ("    low  band 80-200 Hz", w.mid * w.lowMul, lo);
        row ("    mid  band 0.8-1.6 kHz", w.mid, md);
        row ("    high band 5-9 kHz", w.mid * w.highMul, hi);
    }
    return 0;
}
