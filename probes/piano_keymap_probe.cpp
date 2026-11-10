// MILESTONE 5's GATE: is this 88 instruments, or one transposed?
#include <cstdio>
#include <cmath>
#include <algorithm>
#include "dsp/PianoString.h"
int main()
{
    std::printf ("\nMILESTONE 5: THE 88-KEY MAP\n\n");
    std::printf ("  %-6s %8s %10s %9s %8s %8s %8s %7s\n",
                 "key", "Hz", "B", "T60 s", "strings", "detune", "hard", "strike");
    for (int n : { 21, 28, 36, 40, 48, 60, 72, 84, 96, 108 })
    {
        const auto k = dsp::piano::keySpec (n);
        const char* nm = n==21?"A0":n==28?"E1":n==36?"C2":n==40?"E2":n==48?"C3"
                        :n==60?"C4":n==72?"C5":n==84?"C6":n==96?"C7":"C8";
        std::printf ("  %-6s %8.1f %10.6f %9.2f %8d %8.2f %8.2f %7.3f\n",
                     nm, k.f0, k.B, k.decayT60, k.strings, k.detuneCents, k.hardness, k.strikePos);
    }
    int fails = 0;
    const auto a0 = dsp::piano::keySpec (21), tenor = dsp::piano::keySpec (52),
               c4 = dsp::piano::keySpec (60), c8 = dsp::piano::keySpec (108);

    // 1. B IS FLAT WHERE THE INSTRUMENT IS PLAYED, and this row used to assert the
    //    opposite. It demanded A0 and C8 both be three times the tenor, because that is
    //    how piano scaling is usually DESCRIBED. A real Steinway, measured off the
    //    University of Iowa recordings, says 0.000133 at A1, 0.000127 at A2, 0.000226
    //    at A3 and 0.000123 at A4 - four octaves essentially flat at 1.3e-4. A builder
    //    chooses length, diameter and tension together precisely to hold inharmonicity
    //    steady where it would be heard as out of tune.
    //
    //    So the gate now asserts what was measured. The top-octave rise is kept because
    //    published scaling has it, but it is [MAN] and not [COR]: our own treble
    //    readings swung by a factor of ninety with the analysis window and cannot
    //    support a claim either way.
    const auto a1 = dsp::piano::keySpec (33), a2 = dsp::piano::keySpec (45),
               a4 = dsp::piano::keySpec (69);
    const float lo = std::min (std::min (a1.B, a2.B), std::min (c4.B, a4.B));
    const float hi = std::max (std::max (a1.B, a2.B), std::max (c4.B, a4.B));
    const bool flatMiddle = hi < lo * 1.6f && lo > 8.0e-5f && hi < 2.6e-4f;
    if (! flatMiddle) ++fails;
    std::printf ("\n  %-4s inharmonicity is flat where it is played     A1 %.6f, A2 %.6f, C4 %.6f, A4 %.6f\n",
                 flatMiddle ? "ok" : "FAIL", a1.B, a2.B, c4.B, a4.B);
    const bool topRises = c8.B > c4.B * 3.0f;
    if (! topRises) ++fails;
    std::printf ("  %-4s and climbs in the top octave                 C4 %.6f, C8 %.6f   [MAN, not measured here]\n",
                 topRises ? "ok" : "FAIL", c4.B, c8.B);

    // 2. decay shortens steeply with pitch
    const bool decays = a0.decayT60 > c4.decayT60 * 3.0f && c4.decayT60 > c8.decayT60 * 2.0f;
    if (! decays) ++fails;
    std::printf ("  %-4s and a bass note rings far longer             A0 %.1f s, C4 %.1f s, C8 %.1f s\n",
                 decays ? "ok" : "FAIL", a0.decayT60, c4.decayT60, c8.decayT60);

    // 3. the string count steps 1, 2, 3 and never goes back
    int prev = 0; bool monotonic = true, saw1 = false, saw2 = false, saw3 = false;
    for (int n = 21; n <= 108; ++n)
    { const int s = dsp::piano::keySpec (n).strings;
      if (s < prev) monotonic = false; prev = s;
      if (s == 1) saw1 = true; if (s == 2) saw2 = true; if (s == 3) saw3 = true; }
    const bool counts = monotonic && saw1 && saw2 && saw3;
    if (! counts) ++fails;
    std::printf ("  %-4s and the string count steps 1 to 2 to 3       and never back down\n",
                 counts ? "ok" : "FAIL");

    // 4. every key is playable: nothing clamps to a rail or goes non-physical
    bool sane = true;
    for (int n = 21; n <= 108; ++n)
    { const auto k = dsp::piano::keySpec (n);
      if (k.B <= 0 || k.B > 3e-3f || k.decayT60 < 0.5f || k.hardness < 0 || k.hardness > 1
          || k.strikePos < 0.05f || k.strikePos > 0.2f || k.detuneCents < 0.5f) sane = false; }
    if (! sane) ++fails;
    std::printf ("  %-4s and all 88 keys stay physical                no rails, no zeroes\n",
                 sane ? "ok" : "FAIL");
    // 5. EVERY ONE OF THE 88 MUST BE STABLE AND PLAYABLE, struck through the board.
    //    This is the row that matters most and it is the one that was missing: the
    //    string alone is stable at every pitch, but the COLLISION was not. On a short
    //    loop the wave the hammer just injected returns under the felt during the
    //    contact and deepens the compression, which raises a force going as the 2.5
    //    power, which injects more. Measured peaks of 1703 at midi 85, 1336 at 93 and
    //    6.6e9 at 99 while the middle of the keyboard sat at 0.35.
    double worstPk = 0, quietest = 1e9, loudest = 0;
    int    worstKey = 0;
    for (int n = 21; n <= 108; ++n)
    {
        const auto k = dsp::piano::keySpec (n);
        dsp::piano::Note v; v.prepare (48000.0);
        v.setNote (k.f0, k.B, k.decayT60, k.damping, k.detuneCents, k.strings);
        v.setHammer (k.hardness);
        dsp::piano::Soundboard bd; bd.prepare (48000.0); bd.setAmount (0.80f);
        v.strike (0.7f, k.strikePos);
        double pk = 0;
        for (int i = 0; i < 48000; ++i)
            pk = std::max (pk, (double) std::fabs (bd.process (v.process() * 0.22f * k.level) * 0.5f));
        if (pk > worstPk) { worstPk = pk; worstKey = n; }
        quietest = std::min (quietest, pk); loudest = std::max (loudest, pk);
    }
    const bool stable = worstPk < 1.5;
    if (! stable) ++fails;
    std::printf ("  %-4s and every one of the 88 is stable            loudest %.3f at midi %d\n",
                 stable ? "ok" : "FAIL", worstPk, worstKey);

    //    and balanced enough to play. A real piano is evened out by its scaling; this
    //    model has one hammer mass and one string material, so it needs the level
    //    column to do that job. Without it the keyboard spanned 38 dB.
    const double spread = 20.0 * std::log10 (loudest / std::max (1e-9, quietest));
    // THE BOUND IS MEASURED NOW, not invented. A real Steinway spans 6.9 dB across the
    // whole keyboard at one dynamic, read off the reference recordings; the 20 dB here
    // before was a number I chose with nothing behind it. We are at about 20, so this
    // row is a KNOWN GAP and stays red: the raw keyboard spans 45 dB and what survives
    // a smooth fit is jagged rather than sloped, so the fix is whatever steps from note
    // to note - the dispersion solver's stage count, the delay rounding, the string
    // count changing - and not a bigger curve.
    const bool balanced = spread < 12.0;
    if (! balanced) ++fails;
    std::printf ("  %-4s and the keyboard is as even as a real one    %.1f dB spread, a Steinway is 6.9%s\n",
                 balanced ? "ok" : "FAIL", spread, balanced ? "" : "   <- KNOWN GAP");

    std::printf ("\n  %d failures.\n", fails);
    return fails == 0 ? 0 : 1;
}
