// MILESTONE 4's GATE: is it a soundboard, or a small reverb with a nice name?
//
// The author asked for a grand, and reached for reverb to get there. A hall is the room
// AROUND the instrument; the soundboard is part OF it. The two are separable and this
// is where the separation is made testable:
//
//   1. IT RINGS BRIEFLY. A plate decays in a couple of hundred milliseconds. A room
//      takes seconds. Anything longer here is a reverb, and it smears the attack.
//   2. IT COLOURS. Its response must be full of peaks and troughs - that is what
//      makes a piano woody rather than electric. A flat response is a wire.
//   3. IT RADIATES BADLY AT THE BOTTOM, which is why short grands have no bass and
//      why a piano's lowest notes are carried by their upper partials.
//   4. AND IT MUST NOT SWALLOW THE STRIKE. The attack that milestone 2 produced has
//      to survive: the note's first 50 ms must still get brighter with velocity.
#include <cstdio>
#include <cmath>
#include <vector>
#include <complex>
#include <algorithm>
#include <initializer_list>
#include "dsp/PianoString.h"
#include "dsp/Constants.h"

static const double SR = 48000.0;

static std::vector<float> impulse (float amount, int n)
{
    dsp::piano::Soundboard b; b.prepare (SR); b.setAmount (amount);
    std::vector<float> y ((size_t) n);
    for (int i = 0; i < n; ++i) y[(size_t) i] = b.process (i == 0 ? 1.0f : 0.0f);
    return y;
}

static double magAt (const std::vector<float>& x, double hz)
{
    const double w = 2.0 * dsp::kPi * hz / SR;
    std::complex<double> a {0, 0};
    for (size_t i = 0; i < x.size(); ++i)
        a += std::complex<double> (x[i], 0.0) * std::exp (std::complex<double> (0.0, -w * (double) i));
    return std::abs (a);
}

int main()
{
    std::printf ("\nMILESTONE 4: A SOUNDBOARD, NOT A SMALL REVERB\n\n");
    int fails = 0;
    const auto h = impulse (1.0f, (int) (SR * 2.0));

    // 1. how long the plate rings
    double peak = 0; for (float v : h) peak = std::max (peak, (double) std::fabs (v));
    double t60 = 2.0;
    {
        const int hop = 512;
        for (size_t i = 0; i + hop < h.size(); i += hop)
        {
            double sq = 0; for (int k = 0; k < hop; ++k) sq += (double) h[i+k]*h[i+k];
            if (20.0 * std::log10 (std::max (1e-9, std::sqrt (sq/hop) / peak)) < -60.0)
            { t60 = (double) i / SR; break; }
        }
    }
    const bool brief = t60 < 0.6 && t60 > 0.02;
    if (! brief) ++fails;
    std::printf ("  %-4s the plate rings briefly, like a plate        T60 %.3f s (a room is seconds)\n",
                 brief ? "ok" : "FAIL", t60);

    // 2. it colours: peaks and troughs across the band
    std::vector<double> db;
    for (double f = 60.0; f < 6000.0; f *= 1.012) db.push_back (20.0 * std::log10 (std::max (1e-9, magAt (h, f))));
    double lo = 1e9, hi = -1e9; int peaks = 0;
    for (size_t i = 1; i + 1 < db.size(); ++i)
    {
        lo = std::min (lo, db[i]); hi = std::max (hi, db[i]);
        if (db[i] > db[i-1] && db[i] > db[i+1]) ++peaks;
    }
    // A BOARD COLOURS; IT MUST NOT MUTE. This row used to demand 15 peaks and called
    // a bigger range better, which rewarded exactly the fault the author heard: at 85 per cent
    // wet with high-Q modes the board put 21 dB of comb across A3's own partials -
    // partial 2 came out 22 dB under the Steinway's while partial 5 came out 11 dB
    // over. That is not colouration, it is a hand on the strings, and he named it
    // before any measurement did: "muting, like if you palmed a guitar."
    //
    // So the range is now BOUNDED at both ends. Some ripple is a soundboard; 20 dB of
    // it is a comb filter.
    const bool coloured = (hi - lo) > 6.0 && (hi - lo) < 22.0 && peaks >= 8;
    if (! coloured) ++fails;
    std::printf ("  %-4s and it colours without muting                %.1f dB range (6 to 22), %d peaks\n",
                 coloured ? "ok" : "FAIL", hi - lo, peaks);

    // 3. bass radiates badly
    double low = 0, mid = 0; int nl = 0, nm = 0;
    // BELOW THE FIRST MODES, which is where the claim lives. The first window tried
    // was 25-55 Hz and it straddled the lowest plate mode at 55 Hz, so it was asking
    // whether the plate is quiet at a frequency it resonates at. A plate radiates
    // badly BELOW its modes, not at them.
    for (double f = 18.0; f < 42.0;  f *= 1.02) { low += magAt (h, f); ++nl; }
    for (double f = 200.0; f < 3000.0; f *= 1.02) { mid += magAt (h, f); ++nm; }
    const double rolloff = 20.0 * std::log10 ((mid/nm) / std::max (1e-9, low/nl));
    const bool poorBass = rolloff > 6.0;
    if (! poorBass) ++fails;
    std::printf ("  %-4s and radiates badly below its lowest modes    mid is %.1f dB over 18-42 Hz\n",
                 poorBass ? "ok" : "FAIL", rolloff);

    // 4. the strike still survives it
    auto centroid = [] (const std::vector<float>& x, int n) {
        double num = 0, den = 0;
        for (int k = 1; k < 500; ++k) { const double hz = k * 40.0; if (hz > SR*0.45) break;
            const double w = 2.0*dsp::kPi*hz/SR; std::complex<double> a {0,0};
            for (int i = 0; i < n; ++i) { const double win = 0.5-0.5*std::cos(2*dsp::kPi*i/(n-1));
                a += std::complex<double>(x[(size_t)i]*win,0.0)*std::exp(std::complex<double>(0,-w*i)); }
            const double m = std::abs(a); num += hz*m; den += m; }
        return den > 0 ? num/den : 0.0; };
    auto note = [&] (float vel) {
        dsp::piano::Note nt; nt.prepare (SR); nt.setNote (220.0f, 2e-4f, 10.0f, 0.20f, 2.5f, 3);
        dsp::piano::Soundboard b; b.prepare (SR); b.setAmount (0.85f);
        nt.setHammer (0.5f); nt.strike (vel);
        std::vector<float> y ((size_t)(SR*0.3));
        for (auto& v : y) v = b.process (nt.process() * 0.5f);
        return y; };
    const double cSoft = centroid (note (0.15f), (int)(SR*0.05));
    const double cHard = centroid (note (1.00f), (int)(SR*0.05));
    const bool strikeSurvives = cHard > cSoft * 1.15;
    if (! strikeSurvives) ++fails;
    std::printf ("  %-4s and the strike survives the plate            soft %.0f Hz, hard %.0f Hz (%.2fx)\n",
                 strikeSurvives ? "ok" : "FAIL", cSoft, cHard, cHard / std::max (1.0, cSoft));

    std::printf ("\n  %d failures.\n", fails);
    return fails == 0 ? 0 : 1;
}
