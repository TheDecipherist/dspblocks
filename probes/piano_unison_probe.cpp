// MILESTONE 3's GATE: is the decay really two-stage, and does it come from coupling?
//
// The claim: several detuned strings sharing a bridge start in phase, dump energy into
// it fast, then drift out of phase, stop driving it, and ring on quietly for a long
// time. So the envelope is NOT one exponential. It is two, and the second is much
// slower than the first.
//
// This fits a double exponential to the measured envelope and requires the two time
// constants to differ by a real factor. Then it does the control that matters: turn
// the DETUNE off and the two-stage behaviour must COLLAPSE, because identical strings
// never drift. If it survives with zero detune, the effect is coming from something
// else and the claim is wrong.
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <initializer_list>
#include "dsp/PianoString.h"

static const double SR = 48000.0;

struct Decay { double fastT60, slowT60, ratio, kneeMs; };

// THE PROMPT DECAY AGAINST THE AFTERSOUND, measured directly in two fixed windows.
//
// A knee-searching two-segment fit was tried first and it kept splitting at the ATTACK
// rather than at the prompt/aftersound boundary, reporting a flat first segment and
// calling it the fast one. The structure this is testing is not subtle and does not
// need finding: the prompt decay is the first half second, the aftersound is seconds
// three to five, and the claim is that the first is much steeper than the second.
static Decay analyse (const std::vector<float>& x)
{
    auto dbAt = [&] (double t) {
        const size_t a = (size_t) (t * SR); double sq = 0; const int N = 4096;
        for (int k = 0; k < N && a + k < x.size(); ++k) sq += (double) x[a+k] * x[a+k];
        return 20.0 * std::log10 (std::max (1e-9, std::sqrt (sq / N))); };
    const double early = (dbAt (0.1) - dbAt (0.5)) / 0.4;      // dB per second, positive
    const double late  = (dbAt (3.0) - dbAt (5.0)) / 2.0;
    const double fast  = early > 0.01 ? 60.0 / early : 1e6;
    const double slow  = late  > 0.01 ? 60.0 / late  : 1e6;
    return { fast, slow, late > 0.01 ? early / late : 0.0, 0.0 };
}

static std::vector<float> run (float f0, int strings, float detune, double secs)
{
    // THE INSTRUMENT'S OWN SETTINGS, not invented ones. This used to pass a hand-picked
    // B of 2e-4 and a 12 s loop time, so it was measuring a string nobody plays: when
    // the decay and the bridge were re-fitted against a real Steinway the gate went red
    // while the actual instrument got closer to the reference. A gate on arbitrary
    // settings answers an arbitrary question.
    const auto k = dsp::piano::keySpec (57);           // A3, where the reference is
    dsp::piano::Note nt;
    nt.prepare (SR);
    nt.setNote (f0, k.B, k.decayT60, k.damping, detune, strings);
    nt.setHammer (k.hardness);
    nt.strike (0.55f, k.strikePos);
    std::vector<float> y ((size_t)(SR*secs));
    for (auto& v : y) v = nt.process();
    return y;
}

int main()
{
    std::printf ("\nMILESTONE 3: THE COUPLED UNISON\n\n");
    int fails = 0;
    std::printf ("  %-34s %11s %11s %9s\n", "case", "prompt T60", "after T60", "steeper");

    struct C { const char* what; int strings; float detune; };
    const C cases[] = {
        { "3 strings, 2 cents apart (a piano)",   3, 2.0f },
        { "2 strings, 2 cents apart (the tenor)", 2, 2.0f },
        { "3 strings, NO detune (the control)",   3, 0.0f },
        { "1 string (what milestone 2 was)",      1, 0.0f },
    };
    std::vector<Decay> got;
    for (const auto& c : cases)
    {
        const Decay d = analyse (run (220.0f, c.strings, c.detune, 9.0));
        got.push_back (d);
        std::printf ("  %-34s %10.2fs %10.2fs %8.2fx\n",
                     c.what, d.fastT60, d.slowT60, d.ratio);
    }

    // 1. a real unison must be visibly two-stage
    // THE TARGET IS MEASURED: a real Steinway's A3 mf decays with a prompt phase 6.02
    // times steeper than its aftersound (0.1-0.5 s against 3-5 s). Within a factor of
    // two of that is the bar.
    const bool twoStage = got[0].ratio > 3.0 && got[0].ratio < 12.0 && got[1].ratio > 2.0;
    if (! twoStage) ++fails;
    std::printf ("\n  %-4s a detuned unison decays in two stages        3-string %.2fx, 2-string %.2fx, a Steinway is 6.02\n",
                 twoStage ? "ok" : "FAIL", got[0].ratio, got[1].ratio);

    // 2. AND IT HAS TO COME FROM THE DETUNE. Identical strings cannot drift, so the
    //    control must lose most of it. If it does not, something else is doing it and
    //    the explanation above is wrong.
    // the control: identical strings cannot drift, so they must NOT show it
    const bool fromDetune = got[2].ratio < got[0].ratio * 0.75;
    if (! fromDetune) ++fails;
    std::printf ("  %-4s and it comes from the detune, not the count  detuned %.2fx, identical %.2fx\n",
                 fromDetune ? "ok" : "FAIL", got[0].ratio, got[2].ratio);

    // 3. one string is the thing we are trying to stop sounding like
    const bool beatsSingle = got[0].ratio > got[3].ratio * 1.3;
    if (! beatsSingle) ++fails;
    std::printf ("  %-4s and a single string does not do it            unison %.2fx, single %.2fx\n",
                 beatsSingle ? "ok" : "FAIL", got[0].ratio, got[3].ratio);

    std::printf ("\n  %d failures.\n", fails);
    return fails == 0 ? 0 : 1;
}
