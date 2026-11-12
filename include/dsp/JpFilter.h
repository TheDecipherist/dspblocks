// THE JP-8000 FILTER, moved out of jp8000 on 2026-10-07 alongside JpOsc.h.
// dspblocks is the declared owner of DSP and a second copy anywhere else is the
// mistake that cost thirty per-unit SessionStrip copies (see SuperFilter.h).
//
// It moved VERBATIM. Only the namespace changed, from `jp` to `dsp::jp`. Nothing
// was rewritten, re-derived or re-tuned.
//
// THE SPLIT IS THE HARDWARE'S. The H8 turns a cents total into a frequency WORD
// and a damping WORD and writes both to the ASIC; the ASIC is the filter. So
// `cutoffWord()` and `resonanceQ()` - which read the unit's own ROM tables - stay
// in jp8000 beside the ROM they came from, and what is here is the ASIC side: how
// a word becomes a corner frequency, the two-pole section itself, and the input
// trim that keeps a Q of 496 inside sixteen bits.
//
// PROVENANCE. The word reading is [FW+SVF]: the frequency word is Q14, the ASIC's
// fixed point everywhere else, and it is the Chamberlin F = 2 sin(pi fc / Fs),
// whose 2.0 is Nyquist - which is exactly what the firmware's 0x7FFF clamp means.
// The TOPOLOGY itself is open debt 2 in jp8000/docs/laws.md: the TC170C140 has no
// datasheet, no microcode in the image and the published decap does not cover it,
// so both documentary routes are confirmed exhausted and only a hardware sweep is
// left. The resonance trim is [EAR], measured over all 769 programs.
#pragma once

#include <algorithm>
#include <cmath>

#include "Constants.h"

namespace dsp::jp {

/** The DSP tick, 88.2 kHz, halved: the filter runs at the audio rate.
    [FW+SCH] 67.7376 MHz / 768 = 88,200 feeding all four DSPs. */
inline constexpr double kAsicAudioRate = 44100.0;

/** The ASIC's frequency word read as Chamberlin F = 2 sin(pi fc / Fs), Q14, so the
    0x7FFF clamp is Nyquist. [FW+SVF] 14.1 Hz at cutoff 0, 553 Hz at 64, 5.1 kHz at
    100, Nyquist from 120. */
inline double asicFreqWordHz (int word, double rate = kAsicAudioRate) noexcept
{
    const double halfF = std::clamp (word / 32768.0, 0.0, 1.0);
    return rate / kPi * std::asin (halfF);
}

/** RESONANCE LEVEL STAGING [EAR], and why it is not a taste knob. The ROM's damping
    word gives Q up to 496, and a state-variable low-pass peaks at Q, so one note
    reached a peak of 1074 through the plugin path before the master limiter. A
    16-bit ASIC cannot hold that state either, so it must scale its own input. We
    scale by Q^-0.75, which leaves the resonant peak at Q^0.25, +13 dB at the top.
    Measured over all 769 programs, one note each: median peak 0.29, 90th 0.92, so
    the limiter only meets the extremes. Open debt 3: a hardware recording at one
    cutoff with resonance 0 and 127 reads the passband loss and replaces this. */
inline double resonanceTrim (double q) noexcept
{
    return std::pow (std::max (1.0, q), -0.75);
}

/** Filter types, as the patch byte numbers them. [FW 0x27] */
enum FilterType { Hpf = 0, Bpf = 1, Lpf = 2 };

/** Two-pole state-variable filter (trapezoidal), outputs LP/BP/HP.
    Two of these in series give the machine's -24 dB/oct slope. [FW 0x28] */
struct Svf
{
    double ic1 = 0, ic2 = 0, g = 0.1, k = 1, a1 = 0, a2 = 0, a3 = 0;

    void set (double hz, double q, double sr) noexcept
    {
        g = std::tan (kPi * std::min (hz, sr * 0.49) / sr); k = 1.0 / q;
        a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }

    inline double process (double x, int type) noexcept
    {
        const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        if (type == Lpf) return v2;
        if (type == Bpf) return v1;
        return x - k * v1 - v2;                   // HPF
    }

    void reset() noexcept { ic1 = ic2 = 0; }
};

} // namespace dsp::jp
