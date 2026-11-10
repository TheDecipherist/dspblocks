// THE JP-8000 OSCILLATOR, moved out of jp8000 on 2026-10-07 because it now exists
// twice: this unit and solera both carry the seven-saw stack, with the same Szabo
// offsets and the same firmware detune scaling. dspblocks is the declared owner of
// DSP and a second copy anywhere else is the mistake that cost thirty per-unit
// SessionStrip copies (see SuperFilter.h).
//
// It moved VERBATIM. Only the namespace changed, from `jp` to `dsp::jp`, and the
// super-saw DETUNE WORD is now passed in rather than read from the unit's ROM
// table. Nothing was rewritten, re-derived or re-tuned; every law and its
// provenance tag is the one that was measured in jp8000.
//
// WHY THE WORD IS AN ARGUMENT, and it is not a style choice. The real machine
// splits exactly here: the H8 computes a WORD from the knob and writes it to the
// ASIC, and the ASIC renders. So the host arithmetic - the firmware's own detune
// table, the cutoff word, the resonance word - stays in the unit beside the ROM it
// came from, and this header is the ASIC side. It also means no ROM-derived table
// ever lands in a shared library, which is the archive's rule.
//
// WHAT IS [FW] AND WHAT IS NOT. The words are firmware-exact and documented in
// jp8000/docs/laws.md ("Oscillators"). What the ASIC DOES with them is not in the
// ROM at all - the TC170C140 is a Roland/Toshiba custom with no datasheet, no
// microcode in the image, and the published decap does not cover the oscillator
// section. So:
//
//   SUPER SAW      [FW] detune + mix words, [COR] Szabo 2010 measured a real
//                  JP-8000 for the seven offsets and both gain curves, [EAR] the
//                  word/642 scale between them.
//   TRIANGLE MOD   [FW] word A = (127-c1)<<8, word B = c2.  Response [EAR].
//   NOISE          [FW] colour = 100*c1 - 1292 cents, level word from c2.
//                  [EMU] the colour ANCHOR is measured and within 1% end to end.
//                  The SHAPE is still wrong in FORM, not just in tuning: this is
//                  a one-pole low-pass with c2 scaling the output, and the real
//                  source is a RESONANT band - measured flatness runs 0.786 at
//                  low c1 down to 0.0026 at high, and c2 raises level 17.7 dB
//                  while flatness HALVES, which no gain and no one-pole can do.
//   FEEDBACK OSC   [FW] word A = 258*c1>>1, word B = 32767-gain(127-c2). Response [EAR].
//   SQR (PWM)      [FW] word A = c1<<8.  Width curve and depth [EAR].
//   SAW, TRI       plain, band-limited.
//
// Five of those [EAR] lines close with one recording of real hardware sweeping
// CTRL1 and CTRL2 per waveform, because the word at every knob position is already
// known - it is a fit against a known input, not an ear match.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace dsp::jp {

/** A4 = 440 Hz at 6900 cents (note * 100). [MAN] */
inline double centsToHz (double cents) noexcept
{
    return 440.0 * std::exp2 ((cents - 6900.0) / 1200.0);
}

/** polyBLEP correction for a step discontinuity at phase t with increment dt. */
inline double blep (double t, double dt) noexcept
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

/** The seven super-saw ratios, EXACT: the ESP's own integer detune table
    {0, 128, -128, 408, -412, 704, -720} times 10/65536, sorted ascending so index 3
    is the centre saw. [PUB] the TC170C140 reverse engineering (39C3 / gearmulator
    ronaldo esp), read out of the chip's code.

    THEY WERE SZABO'S MEASURED FLOATS UNTIL 2026-10-07 and the swap is worth a
    sentence, because both are right. Szabo measured a real JP-8000 with a spectrum
    analyser; this is the arithmetic the silicon performs. They differ by at most
    0.46 cents at unity detune, which is as strong a cross-check as this machine has
    produced. The integers win only because they are exact rather than sampled. */
inline constexpr double kSsRatio[7] =
    { -0.10986328125, -0.062866210938, -0.01953125, 0.0,
       0.01953125,    0.062255859375,  0.107421875 };

/** The divisor that maps the firmware's detune word onto the 0..1 detune axis.
    DERIVED, 2026-10-07; it was 642.0 and [EAR] before that.

    The ESP reverse engineering gives the MCU's detune value as 1..321 over 128
    steps and the resulting ratio as (value*512 / 2^23) * (coeff/128), which puts
    Szabo's offsets at MCU 320 - so d = MCU/320. That MCU table IS the JP-8000's own
    ROM table at 0x6A3B4: ROM_raw = 4*(MCU - 1), exact in 95 of 128 entries and
    never overshooting. Hence d = (raw + 4)/1280.

    The host word is (raw >> 1) + 2, and every one of the 128 ROM entries is EVEN,
    so that word is exactly (raw + 4)/2 and d = word/640 is exactly (raw + 4)/1280
    on every entry with no rounding anywhere.

    The old 642 was a guess and it was wrong by a constant -0.312% across the whole
    range - the right answer to within a third of a per cent, which is why nothing
    ever sounded wrong. */
// [EMU 2026-10-07] MEASURED, 649.5, replacing a derived 640.0.
//
// Ten isolated settings on JE8086 with the chorus and delay actually off, d read
// from each harmonic cluster against its OWN centre partial so the note's pitch
// cannot bias it, sub-bin peak positions, and 7 to 55 independent harmonic
// estimates per setting with a standard deviation around 0.00005. The three
// lowest settings are excluded: their partials bleed into each other, which the
// amplitude check catches (side/centre reads 1.09 instead of ~1.30) while the
// standard deviation does not, because bleed is consistent across harmonics.
//
// word/d came out 643 to 659, least squares 649.5, and at 649.5 the model is
// within 1.39% worst and 0.62% rms. At 640.0 it was 5.04% worst and 2.11% rms,
// which displaced the outermost partial by 1.17 cents.
//
// THIS DISAGREES WITH THE PUBLISHED DERIVATION BY 1.5% and the disagreement is
// left standing rather than reconciled: the ESP reverse engineering's arithmetic
// gives 640 and the measurement gives 649.5. The measurement wins here because
// it is of the thing itself, but it is of an EMULATOR of the thing, so this is
// [EMU] and not [FW].
inline constexpr double kSsDetuneScale = 649.5;

/** Waveform ids, as the patch byte numbers them. OSC1 takes 0..6, OSC2 takes 0..2
    where 0 is SQR(PWM), 1 SAW, 2 TRI - which is why `sample` branches on `isOsc1`. */
enum Wave { SuperSaw = 0, TriangleMod = 1, Noise = 2, FeedbackOsc = 3, Pwm = 4, Saw = 5, Tri = 6 };

/** One oscillator's state. Two per voice on this machine. */
struct Osc
{
    double phase = 0, inc = 0, last = 0, fbState = 0, lpState = 0, hpState = 0;
    std::uint32_t rng = 0x9e3779b9;
    std::array<double, 7> ssPhase {};   // super saw partials

    void reset() noexcept
    {
        phase = 0; last = fbState = lpState = hpState = 0;
        for (auto& p : ssPhase) p = 0;
    }

    /** The super saw partials start at random phases, the rest at 0 [EAR]. Seeded from
        the note so a given note is reproducible, which is what the snapshot probes need. */
    void seedSuperSawPhases (int note) noexcept
    {
        std::uint32_t r = 0x2545F491u ^ (std::uint32_t) (note * 7919);
        for (auto& p : ssPhase) { r = r * 1664525u + 1013904223u; p = (double) (r >> 8) / 16777216.0; }
    }

    /** One sample. `superDetuneWord` is the firmware's own detune word, 2..642
        ([FW] `(table[ctrl1] >> 1) + 2`), and is ignored by every other waveform. */
    double sample (int wave, int c1, int c2, double hz, double sr, double lfo1v,
                   bool isOsc1, bool sync, bool& wrapped, double superDetuneWord) noexcept
    {
        const double dt = std::min (0.45, hz / sr);
        if (sync && wrapped) phase = 0;
        wrapped = false;
        if (isOsc1 && wave == SuperSaw)
        {   // SUPER SAW: 7 saws, detune from the firmware curve, mix law from the hardware
            const double d = superDetuneWord / kSsDetuneScale;
            const double mix = c2 / 127.0;
            const double centre = -0.55366 * mix + 0.99785;
            const double side = -0.73764 * mix * mix + 1.2841 * mix + 0.044372;
            double out = 0;
            for (int i = 0; i < 7; ++i)
            {
                const double incr = dt * (1.0 + kSsRatio[i] * d);
                double& ph = ssPhase[(std::size_t) i];
                ph += incr; if (ph >= 1.0) { ph -= 1.0; if (i == 3) wrapped = true; }
                // EACH SAW IS BAND-LIMITED, as Solera's long-settled version of this same
                // stack does it. Seven raw ramps alias seven times over, and the machine's
                // own high-pass below cannot take back what the aliases added.
                out += (2.0 * ph - 1.0 - blep (ph, incr)) * (i == 3 ? centre : side);
            }
            // the hardware high-passes the stack near the fundamental [COR]
            const double a = std::exp (-2.0 * M_PI * hz / sr);
            hpState = a * hpState + (1 - a) * out;
            return (out - hpState) * 0.6;
        }
        phase += dt; if (phase >= 1.0) { phase -= 1.0; wrapped = true; }
        const double t = phase;
        switch (wave)
        {
            case TriangleMod: if (isOsc1)
            {   // TRIANGLE MOD: a triangle folded by a sine shaper, c1 = amount, c2 = asymmetry [EAR]
                const double tri = t < 0.5 ? 4 * t - 1 : 3 - 4 * t;
                const double amt = 1.0 + 6.0 * (c1 / 127.0), asym = c2 / 127.0;
                return std::sin (tri * amt * M_PI * 0.5 + asym * tri * tri * 2.0) * 0.8;
            }
            return 2.0 * t - 1.0 - blep (t, dt);                                    // OSC2 SAW
            case Noise: if (isOsc1)
            {   // NOISE: white noise, one-pole low-pass at 100*c1 - 1292 cents on the pitch
                // scale [FW colour], c2 level [FW]
                rng = rng * 1664525u + 1013904223u;
                const double w = (double) (rng >> 8) / 8388608.0 - 1.0;
                // [EMU 2026-10-07] MEASURED, replacing an [EAR] anchor that was a
                // factor of 8.0 high - exactly three octaves - across the whole
                // control range. Ratios engine/measured were 7.93, 8.06, 8.00,
                // 7.94, 7.96, 7.76 at ctrl1 56/70/84/98/112/127, so the error was
                // a constant offset and not a scale. With -6000 instead of -2400
                // the centre lands within 1% of the measured peak everywhere
                // (166/167, 372/369, 834/834, 1873/1887, 4205/4226, 10002/10311).
                // The LAW is [FW 0xA610] 100*c1 - 1292 cents and it was never in
                // doubt; what was wrong was where those cents are anchored.
                const double fc = centsToHz (100.0 * c1 - 1292.0 + 6900.0 - 6000.0);
                const double a = std::exp (-2.0 * M_PI * std::min (fc, sr * 0.45) / sr);
                lpState = a * lpState + (1 - a) * w;
                return lpState * (0.3 + 0.7 * c2 / 127.0) * 1.5;
            }
            return t < 0.5 ? 4 * t - 1 : 3 - 4 * t;                                  // OSC2 TRI
            case FeedbackOsc:
            {   // FEEDBACK OSC: a saw phase-modulated by its own output, c1 amount, c2 tone [EAR]
                const double fb = 0.45 * (c1 / 127.0);
                double ph = t + fb * fbState; ph -= std::floor (ph);
                const double s = 2.0 * ph - 1.0;
                const double a = 0.2 + 0.79 * (1.0 - c2 / 127.0);
                lpState = lpState + a * (s - lpState);
                fbState = lpState;
                return lpState;
            }
            case Pwm:
            {   // SQR (PWM): width 50%..95% from c1, LFO1 modulates it by c2 [FW knobs, EAR curve]
                double w = 0.5 + 0.45 * (c1 / 127.0) + 0.45 * (c2 / 127.0) * lfo1v;
                w = std::clamp (w, 0.05, 0.95);
                double v = t < w ? 1.0 : -1.0;
                v += blep (t, dt); double t2 = t - w; if (t2 < 0) t2 += 1.0; v -= blep (t2, dt);
                return v;
            }
            case Saw: return 2.0 * t - 1.0 - blep (t, dt);
            case Tri: return t < 0.5 ? 4 * t - 1 : 3 - 4 * t;
            case SuperSaw:
            {   // OSC2 SQR (PWM)
                double w = 0.5 + 0.45 * (c1 / 127.0) + 0.45 * (c2 / 127.0) * lfo1v;
                w = std::clamp (w, 0.05, 0.95);
                double v = t < w ? 1.0 : -1.0;
                v += blep (t, dt); double t2 = t - w; if (t2 < 0) t2 += 1.0; v -= blep (t2, dt);
                return v;
            }
            default: return 2.0 * t - 1.0 - blep (t, dt);
        }
    }
};

} // namespace dsp::jp
