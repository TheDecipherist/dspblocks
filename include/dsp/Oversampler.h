// THE OVERSAMPLER. Lifted out of megasynth's `src/engine/GritFx.h` on
// 2026-09-27, where it had been the distortion module's private half. It is here
// because a second consumer arrived - `dsp::filter::SuperFilter` wants the same
// thing for the same reason - and a copied oversampler is the same mistake as a
// copied filter: two places to fix a window function, one of which nobody
// remembers.
//
// IT MOVED VERBATIM. Every number below is the one GritFx measured, every
// comment explaining WHY is the one that was written beside it there, and the
// arithmetic - the order of the multiply-accumulates, the ring sizes, the masks,
// the `* (float) os` on the interpolated phase - is untouched, because GritFx's
// audio has to be bit-identical across the move and 599 stored programs are the
// gate that says so. What is NEW here is the template parameters: GritFx is
// `Oversampler<32, 8, 2>` and reads exactly as it did, and a per-voice consumer
// that cannot afford 32 samples of latency can ask for a shorter kernel.
//
// WHAT IT IS. A windowed-sinc polyphase FIR, run as an interpolator on the way
// up and a decimator on the way down, with a Kaiser window. The prototype is
// designed ONCE in `setFactor` and never again, so nothing here allocates or
// calls a transcendental in the audio path.
//
// THE ONE THING TO KNOW BEFORE USING IT: the latency is `TapsPerPhase` base
// samples, it is the same at EVERY factor including 1x, and it is not optional.
// A linear-phase FIR sharp enough to separate 20 kHz from 28 kHz costs that many
// taps whatever rate you run it at - see THE LATENCY TRADE below, which is a
// measured table and not an opinion.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace dsp {

/** One windowed-sinc polyphase oversampler, `NumChannels` channels sharing one
    designed prototype.

    `TapsPerPhase` is the kernel length in BASE samples and therefore also the
    latency: the prototype is `TapsPerPhase * factor + 1` taps long, so its group
    delay is `TapsPerPhase / 2` base samples each way and `TapsPerPhase` for the
    round trip, EXACTLY, as a whole number, at every factor.

    THE LATENCY TRADE, AND THE MEASUREMENT THAT SETTLES IT. What the antialias
    filter has to do is fixed in HERTZ: pass to 20 kHz, stop by 28 kHz, because
    content above 28 kHz is what folds back under 20 kHz and is audible. A Kaiser
    window's transition width is `(A - 7.95) / (14.36 N)` of the rate it runs at,
    and N is `TapsPerPhase * factor + 1` while the rate is `factor` times the
    base - so the FACTOR CANCELS and the kernel length in base samples is the
    same at 2x as at 4x. That is why GritFx's latency does not move with its
    mode.

    WHAT THE KERNEL LENGTH ACTUALLY BUYS IS THE TOP OF THE PASSBAND, NOT THE
    ALIAS REJECTION, and that is the opposite of what the formula above makes you
    expect. Measured through this block with an identity shaper - a sine in, its
    rms out, so this is the kernel and nothing else - at a 48 kHz base:

        TapsPerPhase  latency    1 kHz    8 kHz   12 kHz   16 kHz   19 kHz   20 kHz
             8        8 smp     -0.001   -0.000   -0.124   -1.099   -2.975   -3.790 dB
            16       16 smp     -0.000   +0.000   +0.001   +0.002   -0.431   -1.096
            32       32 smp     +0.000   +0.000   +0.000   +0.001   -0.001   +0.002

    and the alias figure through SuperFilter at its worst setting reads -62.7 dBc
    at 4x for ALL THREE rows. What folds back audibly is generated well above the
    transition band and even the 8-tap kernel removes it; the other 24 taps buy
    flatness in the last octave. So a consumer picks `TapsPerPhase` by how much
    of the top of the band it is willing to lose and how much delay it can
    cancel, not by how clean it wants to be. GritFx takes 32 because it is an
    insert effect and a host compensates it once; SuperFilter takes 16 because it
    is per voice and two layers beside it have to be delayed to match.

    AT FACTOR 1 THE BLOCK STILL DELAYS BY `TapsPerPhase`, through the decimator's
    own ring used as a plain line. That is deliberate and it is what lets a host
    compensate ONCE: a consumer whose oversampling is a player-facing switch can
    move between 1x, 2x and 4x without the alignment of anything around it
    changing. A consumer that wants a TRUE zero-latency off setting - SuperFilter
    is one, because its off setting has to stay bit-identical to a filter that
    never had an oversampler - simply does not call `process` at all when it is
    off, and reports 0. */
template <int TapsPerPhase, int MaxFactor, int NumChannels = 1>
class Oversampler
{
public:
    static_assert (TapsPerPhase >= 2 && (TapsPerPhase & (TapsPerPhase - 1)) == 0,
                   "TapsPerPhase is a power of two, so the rings' masks are exact");
    static_assert (MaxFactor >= 1 && (MaxFactor & (MaxFactor - 1)) == 0,
                   "the factor is a power of two");
    static_assert (NumChannels >= 1, "at least one channel");

    static constexpr int kTapsPerPhase  = TapsPerPhase;
    static constexpr int kMaxOversample = MaxFactor;
    static constexpr int kMaxTaps       = TapsPerPhase * MaxFactor + 1;
    /** The block's latency in BASE samples, fixed, at every factor. */
    static constexpr int kLatency       = TapsPerPhase;
    /** [REF] the 80 dB Kaiser design. GritFx's own number, unchanged. */
    static constexpr double kKaiserBeta = 7.857;

    /** Design the prototype for a factor and clear the rings. Called from
        `prepare` or from a mode change; NOT from the audio path, and nothing in
        here allocates - the arrays are sized for the largest factor, so a factor
        change never allocates either.

        Cutoff at the BASE Nyquist, Kaiser window, DC gain exactly 1. */
    void setFactor (int factor) noexcept
    {
        if (factor == os && designed) return;
        os = std::clamp (factor, 1, MaxFactor);
        designed = true;
        taps = kTapsPerPhase * os + 1;
        if (os == 1) { h.fill (0.0f); h[0] = 1.0f; clearRings(); return; }

        const int    M    = (taps - 1) / 2;
        const double i0b  = besselI0 (kKaiserBeta);
        // The prototype is built in DOUBLE and normalised in double, then
        // rounded once: normalising after the rounding would leave the DC gain
        // a few ulps off 1 and the block would not be transparent at rest.
        std::array<double, (size_t) kMaxTaps> hd {};
        double sum = 0.0;
        for (int j = 0; j < taps; ++j)
        {
            const double t = (double) (j - M) / (double) os;             // in base samples
            const double s = t == 0.0 ? 1.0 : std::sin (kPi * t) / (kPi * t);
            const double r = (double) (j - M) / (double) M;
            const double w = besselI0 (kKaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
            hd[(size_t) j] = s * w;
            sum += hd[(size_t) j];
        }
        for (int j = 0; j < taps; ++j) h[(size_t) j] = (float) (hd[(size_t) j] / sum);
        clearRings();
    }

    /** Everything with memory back to zero. */
    void reset() noexcept { clearRings(); }

    [[nodiscard]] int factor() const noexcept { return os; }
    /** Fixed, `TapsPerPhase` base samples, at every factor. Report it to the
        host and delay anything that runs BESIDE this block by the same amount. */
    [[nodiscard]] static constexpr int latencySamples() noexcept { return kLatency; }
    /** The designed prototype, for a probe that wants to measure the window
        rather than trust it. Nothing in the audio path reads this. */
    [[nodiscard]] const float* kernel() const noexcept { return h.data(); }
    [[nodiscard]] int kernelLength() const noexcept { return taps; }

    /** ONE BASE SAMPLE IN, ONE OUT, with `shaper` called `factor` times in
        between - once per oversampled sample, in order.

        `shaper` is anything callable as `float (float)`. It is a template rather
        than a `std::function` because the whole point is that the compiler
        inlines it into the loop: a `std::function` here would be an indirect
        call per oversampled sample, which at 4x is four per base sample per
        voice.

        Polyphase both ways, so the cost per base sample is
        `TapsPerPhase * factor` MACs each way plus `factor` shaper calls. */
    template <typename Shaper>
    [[nodiscard]] float process (int ch, float v, Shaper&& shaper) noexcept
    {
        Channel& c = chans[(size_t) ch];
        if (os == 1)
        {
            // No filter at 1x; the output still takes the block's fixed latency,
            // through the decimator ring used as a plain line.
            c.downRing[(size_t) c.downPos] = shaper (v);
            const float y = c.downRing[(size_t) ((c.downPos - kLatency) & kDownMask)];
            c.downPos = (c.downPos + 1) & kDownMask;
            return y;
        }

        c.upRing[(size_t) c.upPos] = v;
        const int base = c.upPos;
        c.upPos = (c.upPos + 1) & kUpMask;

        for (int p = 0; p < os; ++p)
        {
            // Phase p of the interpolator: h[p], h[p + L], h[p + 2L], ...
            float y = 0.0f;
            for (int k = 0, idx = p; idx < taps; ++k, idx += os)
                y += h[(size_t) idx] * c.upRing[(size_t) ((base - k) & kUpMask)];
            y *= (float) os;

            c.downRing[(size_t) c.downPos] = shaper (y);
            c.downPos = (c.downPos + 1) & kDownMask;
        }

        // Every L-th output of the decimation filter, so N MACs per base sample.
        // Taken at PHASE 0 of this base sample: the filter's group delay is
        // TapsPerPhase/2 base samples exactly from there, and TapsPerPhase/2 -
        // (L-1)/L from the last phase, which is the fraction that put the output
        // a sample early.
        float out = 0.0f;
        const int last = c.downPos - os;
        for (int j = 0; j < taps; ++j)
            out += h[(size_t) j] * c.downRing[(size_t) ((last - j) & kDownMask)];
        return out;
    }

private:
    static constexpr double kPi = 3.14159265358979323846;

    /** The rings are powers of two so the wrap is a mask. THE SIZES ARE NOT
        ARBITRARY: the interpolator reads `TapsPerPhase + 1` base samples back
        and the decimator reads `taps` oversampled ones from a write head that
        has already advanced by `factor`. */
    static constexpr int nextPow2 (int n) noexcept
    { int p = 1; while (p < n) p <<= 1; return p; }
    static constexpr int kUpSize   = nextPow2 (TapsPerPhase + 2);
    static constexpr int kDownSize = nextPow2 (kMaxTaps + MaxFactor);
    static constexpr int kUpMask   = kUpSize - 1;
    static constexpr int kDownMask = kDownSize - 1;

    struct Channel
    {
        std::array<float, (size_t) kUpSize>   upRing {};    ///< base-rate history
        std::array<float, (size_t) kDownSize> downRing {};  ///< oversampled history
        int upPos = 0, downPos = 0;
    };

    void clearRings() noexcept
    {
        for (auto& c : chans)
        {
            c.upRing.fill (0.0f); c.downRing.fill (0.0f);
            c.upPos = c.downPos = 0;
        }
    }

    static double besselI0 (double x) noexcept
    {
        // Series, converges fast for the betas used here.
        double sum = 1.0, term = 1.0;
        const double q = 0.25 * x * x;
        for (int k = 1; k < 60; ++k)
        {
            term *= q / ((double) k * (double) k);
            sum += term;
            if (term < 1.0e-14 * sum) break;
        }
        return sum;
    }

    std::array<Channel, (size_t) NumChannels> chans {};
    std::array<float, (size_t) kMaxTaps> h {};
    int  os = MaxFactor > 4 ? 4 : MaxFactor;
    int  taps = kTapsPerPhase * (MaxFactor > 4 ? 4 : MaxFactor) + 1;
    bool designed = false;
};

/** THE OTHER HALF OF AN OVERSAMPLER, and the half that gets forgotten.

    A block with latency does not just need to report it - everything that runs
    BESIDE it has to be delayed by the same amount or it arrives early and combs.
    Solera has two such paths (the M1 PCM engine and, with the OWN switch, the FM
    stack) and megasynth has the filter's own bypass; each of them is one of
    these, prepared with the block's `latencySamples()`.

    Fixed delay, no interpolation, no allocation after `prepare`. `Max` is the
    largest latency it will ever be asked for. */
template <int Max>
class AlignDelay
{
public:
    static_assert (Max >= 1 && (Max & (Max - 1)) == 0, "a power of two, so the wrap is a mask");

    /** `samples` is what the block beside this one reports. 0 is a pass-through
        that is bit-identical to no delay at all. */
    void prepare (int samples) noexcept
    {
        n = std::clamp (samples, 0, Max);
        reset();
    }
    void reset() noexcept { ring.fill (0.0f); pos = 0; }
    [[nodiscard]] int latencySamples() const noexcept { return n; }

    [[nodiscard]] float process (float x) noexcept
    {
        if (n == 0) return x;
        ring[(size_t) pos] = x;
        const float y = ring[(size_t) ((pos - n) & kMask)];
        pos = (pos + 1) & kMask;
        return y;
    }

private:
    static constexpr int kMask = Max * 2 - 1;
    std::array<float, (size_t) (Max * 2)> ring {};
    int pos = 0, n = 0;
};

} // namespace dsp
