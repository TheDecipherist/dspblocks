// MILESTONE 6's GATE: does the note start like a machine or like an oscillator?
#include <cstdio>
#include <cmath>
#include <vector>
#include <complex>
#include <algorithm>
#include <initializer_list>
#include "dsp/PianoString.h"
#include "dsp/Constants.h"
static const double SR=48000.0;

// Spectral flatness: 1 is noise, near 0 is a tone. The attack of a real piano is
// far flatter than its sustain, because the action makes a broadband sound.
static double flatness (const std::vector<float>& x, size_t from, int n)
{
    double lg = 0, ar = 0; int c = 0;
    for (int k = 2; k < 160; ++k)
    {
        const double hz = k * 60.0; if (hz > SR * 0.45) break;
        const double w = 2*dsp::kPi*hz/SR; std::complex<double> a {0,0};
        for (int i = 0; i < n; ++i) { const double win = 0.5-0.5*std::cos(2*dsp::kPi*i/(n-1));
            a += std::complex<double>(x[from+(size_t)i]*win,0.0)*std::exp(std::complex<double>(0,-w*i)); }
        const double m = std::max (1e-9, std::abs (a));
        lg += std::log (m); ar += m; ++c;
    }
    return std::exp (lg/c) / (ar/c);
}

static std::vector<float> note (int midi, float vel, bool noises, double secs)
{
    const auto k = dsp::piano::keySpec (midi);
    dsp::piano::Note v; v.prepare (SR);
    v.setNote (k.f0, k.B, k.decayT60, k.damping, k.detuneCents, k.strings);
    v.setHammer (k.hardness);
    dsp::piano::Soundboard bd; bd.prepare (SR); bd.setAmount (0.80f);
    dsp::piano::Mechanics mech; mech.prepare (SR);
    v.strike (vel, k.strikePos);
    if (noises) mech.strike (vel, k.f0);
    std::vector<float> y ((size_t)(SR*secs));
    for (auto& s : y) s = bd.process (v.process() * 0.16f * k.level + (noises ? mech.process() : 0.0f)) * 0.5f;
    return y;
}

int main()
{
    std::printf ("\nMILESTONE 6: THE MECHANICS\n\n");
    int fails = 0;
    const auto dry = note (60, 0.8f, false, 0.6), wet = note (60, 0.8f, true, 0.6);
    const int w5 = (int)(SR*0.005), w200 = (int)(SR*0.05);
    const double fa = flatness (dry, 0, w5), fb = flatness (wet, 0, w5);
    const double sa = flatness (wet, (size_t)(SR*0.25), w200);

    const bool broader = fb > fa * 1.5;
    if (! broader) ++fails;
    std::printf ("  %-4s the first 5 ms is broadband now              flatness %.4f -> %.4f\n",
                 broader ? "ok" : "FAIL", fa, fb);

    const bool attackOnly = fb > sa * 2.0;
    if (! attackOnly) ++fails;
    std::printf ("  %-4s and it is the ATTACK, not the whole note     attack %.4f, sustain %.4f\n",
                 attackOnly ? "ok" : "FAIL", fb, sa);

    // it must scale with velocity, and a soft blow must be nearly silent
    auto thump = [] (float vel) {
        dsp::piano::Mechanics m; m.prepare (SR); m.strike (vel, 261.6f);
        double pk = 0; for (int i = 0; i < 4800; ++i) pk = std::max (pk, (double) std::fabs (m.process()));
        return pk; };
    const double soft = thump (0.15f), hard = thump (1.0f);
    const bool scales = hard > soft * 8.0;
    if (! scales) ++fails;
    std::printf ("  %-4s and it scales hard with velocity             soft %.5f, hard %.5f (%.1fx)\n",
                 scales ? "ok" : "FAIL", soft, hard, hard / std::max (1e-9, soft));

    // and it must not swamp the note
    double pn = 0, pt = 0;
    for (size_t i = 0; i < (size_t)(SR*0.02); ++i) pt = std::max (pt, (double) std::fabs (wet[i]));
    for (size_t i = (size_t)(SR*0.1); i < (size_t)(SR*0.2); ++i) pn = std::max (pn, (double) std::fabs (wet[i]));
    // THE BOUND IS MEASURED. A real Steinway's attack is only 1.10x its body at
    // 100-200 ms (A3 mf), 1.13x (A3 ff) and 1.18x (A1) - a piano is far less spiky
    // than it feels. We are at about 6x, which says our PROMPT DECAY is too fast, not
    // that the thump is too loud: the real note is 1.4 dB down at 0.1 s and 5.3 at
    // 0.25 s where ours has dropped ten. The prompt/aftersound RATIO is right (6.02x
    // real against our 6.98x), so it is the rate and not the shape. KNOWN GAP.
    const bool balanced = pt < pn * 2.0;
    if (! balanced) ++fails;
    std::printf ("  %-4s and the attack sits over the body like a piano attack %.4f, body %.4f = %.1fx, a Steinway is 1.1%s\n",
                 balanced ? "ok" : "FAIL", pt, pn, pt / std::max (1e-9, pn),
                 balanced ? "" : "   <- KNOWN GAP");

    std::printf ("\n  %d failures.\n", fails);
    return fails == 0 ? 0 : 1;
}
