// MILESTONE 2's GATE: is it a hammer, or a trigger wearing a hammer's name?
//
// The claim the felt model makes is NOT "louder notes are louder". It is:
//
//   1. CONTACT TIME FALLS AS VELOCITY RISES, because the felt stiffens under load.
//   2. THE SPECTRUM OPENS UP WITH VELOCITY, AT MATCHED LEVEL. Normalise the two
//      renders to the same loudness and the hard one must STILL be brighter. If it
//      is not, velocity is a volume control and the hammer is decoration.
//   3. A STRIKE IS NOT A PLUCK. Same string, same level: the plucked one should be
//      brighter, because releasing from a corner excites everything at once while a
//      millisecond of felt contact cannot reach the top partials at all.
#include <cstdio>
#include <cmath>
#include <vector>
#include <complex>
#include <algorithm>
#include "dsp/PianoString.h"
#include "dsp/Constants.h"

static const double SR = 48000.0;

struct Shot { double centroid, rms, contactMs; };

static Shot measure (float f0, float B, float vel, float hardness, bool asPluck)
{
    dsp::piano::String s;
    s.prepare (SR);
    s.setString (f0, B, 9.0f, 0.20f);
    s.setHammer (hardness);
    if (asPluck) s.pluck (vel * 0.45f); else s.strike (vel);

    std::vector<float> x ((size_t) (SR * 1.0));
    int contact = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        x[i] = s.process();
        if (s.inContact()) ++contact;
    }
    // CENTROID OVER THE ATTACK, 50 ms, and the window is a finding rather than a
    // convenience. Measured over 300 ms the softest blow reads BRIGHTEST (1090 Hz
    // against 662 at full velocity) because it is 10 times quieter, so most of that
    // window is its tail sitting near the numerical floor and the centroid is
    // measuring noise. Over the attack the order is the one the ear reports:
    // 273, 273, 331, 375 Hz. A strike's character is in its first few periods; after
    // the damping has worked, every velocity converges by design.
    const int n = (int) (SR * 0.05);
    std::vector<double> mag;
    double num = 0, den = 0;
    for (int k = 1; k < 900; ++k)
    {
        const double hz = k * 25.0;                    // 25 Hz resolution to 22.5 kHz
        if (hz > SR * 0.45) break;
        const double w = 2.0 * dsp::kPi * hz / SR;
        std::complex<double> acc {0, 0};
        for (int i = 0; i < n; ++i)
        {
            const double win = 0.5 - 0.5 * std::cos (2.0 * dsp::kPi * i / (n - 1));
            acc += std::complex<double> (x[(size_t) i] * win, 0.0)
                 * std::exp (std::complex<double> (0.0, -w * i));
        }
        const double m = std::abs (acc);
        num += hz * m; den += m;
    }
    double sq = 0; for (int i = 0; i < n; ++i) sq += (double) x[(size_t) i] * x[(size_t) i];
    return { den > 0 ? num / den : 0.0, std::sqrt (sq / n), 1000.0 * contact / SR };
}

int main()
{
    std::printf ("\nMILESTONE 2: A FELT HAMMER, NOT A TRIGGER\n\n");
    int fails = 0;
    const float f0 = 220.0f, B = 2.0e-4f;

    std::printf ("  %-10s %12s %12s %14s\n", "velocity", "contact ms", "rms", "centroid Hz");
    std::vector<Shot> shots;
    for (float v : { 0.15f, 0.4f, 0.7f, 1.0f })
    {
        const Shot s = measure (f0, B, v, 0.5f, false);
        shots.push_back (s);
        std::printf ("  %-10.2f %12.3f %12.5f %14.1f\n", v, s.contactMs, s.rms, s.centroid);
    }

    // 0. THE NOTE HAS TO SOUND AT ALL. The first version of this gate passed a model
    //    whose rms was 0.00000, because nothing asked. It asks now.
    const bool audible = shots.back().rms > 0.01;
    if (! audible) ++fails;
    std::printf ("\n  %-4s the blow actually sounds                     rms %.5f at full velocity\n",
                 audible ? "ok" : "FAIL", shots.back().rms);

    // 1. contact time must FALL with velocity AND LAND WHERE A PIANO'S DOES.
    //    A real hammer touches for 1 to 4 ms, longer for the softest blows. Checking
    //    only that it FELL passed a model running 14 to 31 ms, ten times too long.
    bool inRange = true;
    for (const Shot& sh : shots) if (sh.contactMs < 0.8 || sh.contactMs > 8.0) inRange = false;
    const bool contactFalls = shots.front().contactMs > shots.back().contactMs * 1.15;
    if (! contactFalls) ++fails;
    if (! inRange) ++fails;
    std::printf ("  %-4s contact time shortens as the blow hardens   %.3f ms -> %.3f ms\n",
                 contactFalls ? "ok" : "FAIL", shots.front().contactMs, shots.back().contactMs);
    std::printf ("  %-4s and every blow lands in a piano's range      1 to 4 ms, 8 allowed for the softest\n",
                 inRange ? "ok" : "FAIL");

    // 2. brightness must rise with velocity AT MATCHED LEVEL
    // and the centroid has to be ABOVE THE FUNDAMENTAL to be a centroid of anything.
    // The broken version read 40 to 110 Hz on a 220 Hz string, which is a measurement
    // of silence.
    bool sane = true;
    for (const Shot& sh : shots) if (sh.centroid < f0) sane = false;
    if (! sane) ++fails;
    std::printf ("  %-4s the centroid is above the fundamental        %.0f Hz lowest, f0 is %.0f\n",
                 sane ? "ok" : "FAIL", shots.front().centroid, f0);
    const bool brighter = shots.back().centroid > shots.front().centroid * 1.20;
    if (! brighter) ++fails;
    std::printf ("  %-4s and the spectrum opens up with it            %.0f Hz -> %.0f Hz (%.2fx)\n",
                 brighter ? "ok" : "FAIL", shots.front().centroid, shots.back().centroid,
                 shots.back().centroid / std::max (1.0, shots.front().centroid));
    std::printf ("       (centroid is level-independent, so this is not the level talking)\n");

    // 3. a strike is duller than a pluck
    const Shot pl = measure (f0, B, 1.0f, 0.5f, true);
    const bool duller = shots.back().centroid < pl.centroid;
    if (! duller) ++fails;
    std::printf ("  %-4s and a strike is duller than a pluck          strike %.0f Hz, pluck %.0f Hz\n",
                 duller ? "ok" : "FAIL", shots.back().centroid, pl.centroid);

    // 4. a harder felt is brighter at the same velocity
    const Shot soft = measure (f0, B, 0.7f, 0.0f, false);
    const Shot hard = measure (f0, B, 0.7f, 1.0f, false);
    const bool feltWorks = hard.centroid > soft.centroid * 1.10;
    if (! feltWorks) ++fails;
    std::printf ("  %-4s and a harder felt is brighter than a soft one soft %.0f Hz, hard %.0f Hz\n",
                 feltWorks ? "ok" : "FAIL", soft.centroid, hard.centroid);

    // 5. THE STRIKE POSITION MUST NOTCH ITS OWN HARMONIC. A hammer at 1/n kills the
    //    nth partial, and that missing partial is a large part of why a piano is not a
    //    harpsichord. Injecting the force at ONE point excites everything evenly and
    //    produces no notch: measured, partial 8 came out LOUDER than 6 and 7.
    {
        dsp::piano::String st; st.prepare (SR);
        st.setString (220.0f, 2.0e-4f, 8.0f, 0.20f);
        st.setHammer (0.5f); st.strike (0.8f, 0.125f);
        std::vector<float> y ((size_t)(SR*0.35));
        for (auto& v : y) v = st.process();
        auto bin = [&] (double hz) { const double w = 2*dsp::kPi*hz/SR;
            std::complex<double> a {0,0};
            for (size_t i = 0; i < y.size(); ++i) { const double win = 0.5-0.5*std::cos(2*dsp::kPi*i/(y.size()-1));
                a += std::complex<double>(y[i]*win,0.0)*std::exp(std::complex<double>(0,-w*(double)i)); }
            return 20.0*std::log10 (std::max (1e-9, std::abs (a))); };
        const double p6 = bin (220.0*6), p7 = bin (220.0*7), p8 = bin (220.0*8), p9 = bin (220.0*9);
        const double neigh = std::max (std::max (p6, p7), p9);
        // KNOWN GAP, DELIBERATELY RED. A hammer at 1/n kills the nth partial and this
        // model does not, because one circulating delay line cannot carry the two
        // travelling waves whose recombination does the killing. The author heard it as "it
        // sounds like you are picking the strings, not hammering them", which is
        // exactly right. Faking it with a second opposite tap DOES produce the notch
        // and costs the hammer its velocity response - measured, three other rows here
        // went red - so the fix is two rails, not a patch. This row stays red until
        // then rather than being deleted, because a gap nobody can see is a gap nobody
        // fixes.
        const bool notched = p8 < neigh - 8.0;
        if (! notched) ++fails;
        std::printf ("  %-4s a hammer at 1/8 notches the 8th partial      p8 %.1f dB against %.1f for its neighbours%s\n",
                     notched ? "ok" : "FAIL", p8, neigh,
                     notched ? "" : "   <- KNOWN GAP, needs two rails");
    }

    std::printf ("\n  %d failures.\n", fails);
    return fails == 0 ? 0 : 1;
}
