// Saturation, distortion and the deliberate destruction of resolution.
//
// Every one of these machines has some of this, and mostly not as an effect:
// it is the converter, the headroom limit and the fixed-point word length
// showing through. A 1985 pitch shifter running 16-bit arithmetic and a 12-bit
// converter sounds the way it does partly because of that, and a plugin with
// float everywhere sounds cleaner than the machine it is copying. That is not
// a compliment.
//
// So these blocks exist to be used at unity as well: `SoftClip` with a high
// threshold does nothing audible until something asks for more headroom than
// exists, which is exactly what the hardware did.
#pragma once
#include "Constants.h"
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace dsp {

/** Symmetric soft clip, `tanh`-shaped, driven by a threshold rather than a
    gain so a unit can say "this stage saturates at +6 dBu" and mean it.

    Below `threshold` the curve is within a fraction of a dB of unity, so a
    signal that never approaches the limit passes through unchanged. */
class SoftClip
{
public:
    void setThreshold (float t) { thr = std::max (0.01f, t); }
    void setDrive (float d) { drive = std::max (0.01f, d); }

    float process (float x) const
    {
        const float v = x * drive / thr;
        return std::tanh (v) * thr;
    }

private:
    float thr = 1.0f, drive = 1.0f;
};

/** Asymmetric drive: different curvature on the two half-cycles, which is what
    produces EVEN harmonics. A symmetric clipper only ever makes odd ones, and
    that is the difference between "warm" and "fuzzy".

    `bias` shifts the operating point; the block removes the DC that shift
    creates, because leaving it in eats headroom in whatever follows. */
class AsymmetricDrive
{
public:
    void prepare (double sampleRate) { r = (float) (1.0 - 2.0 * kPi * 10.0 / sampleRate); x1 = y1 = 0.0f; }
    void setDrive (float d) { drive = std::max (0.01f, d); }
    /** 0 is symmetric; positive squashes the top half sooner. */
    void setBias (float b) { bias = std::min (std::max (b, -0.9f), 0.9f); }

    float process (float x)
    {
        const float v = x * drive + bias;
        const float shaped = v >= 0.0f ? std::tanh (v)
                                       : std::tanh (v * 0.7f) / 0.7f * 0.85f;
        const float y = shaped - x1 + r * y1;   // DC block, folded in
        x1 = shaped; y1 = y;
        return y / drive;
    }

private:
    float drive = 1.0f, bias = 0.1f, r = 0.999f, x1 = 0.0f, y1 = 0.0f;
};

/** Bit crush and sample-rate reduction, separately, because they are different
    artefacts and machines have them for different reasons.

    Word length quantises AMPLITUDE and the error is broadband noise that scales
    with the signal. Sample-rate reduction quantises TIME and the error is
    aliasing: content above the reduced Nyquist folds down to frequencies that
    are not harmonically related to anything, which is the sound of a cheap
    sampler and of a delay line clocked slowly to buy time.

    A 12-bit converter in front of a 16-bit processor is `setBits(12)` with the
    rate left alone. A delay whose memory was stretched by clocking it down is
    `setRateDivider` with the bits left alone. */
class BitCrusher
{
public:
    void prepare (double sampleRate) { sr = sampleRate; hold = 0.0f; phase = 0.0; }
    /** Effective word length. 24 or more is transparent. */
    void setBits (double b) { bits = std::min (std::max (b, 1.0), 24.0); steps = std::pow (2.0, bits - 1.0); }
    /** 1 = full rate, 4 = one sample in four held. */
    void setRateDivider (double d) { div = std::max (1.0, d); }
    /** Or state the rate directly, for a machine that prints one. */
    void setSampleRate (double hz) { div = std::max (1.0, sr / std::max (100.0, hz)); }

    float process (float x)
    {
        phase += 1.0;
        if (phase >= div)
        {
            phase -= div;
            hold = (float) (std::round (std::min (std::max ((double) x, -1.0), 1.0) * steps) / steps);
        }
        return hold;
    }

private:
    double sr = 48000.0, bits = 24.0, steps = 8388608.0, div = 1.0, phase = 0.0;
    float hold = 0.0f;
};

/** Hard clip at a stated ceiling. Not a nicety: a feedback loop that can run
    away needs a bound somewhere, and a bound that is a real clipper is more
    honest than a NaN guard that silently zeroes the tank. */
inline float hardClip (float x, float ceiling = 1.0f)
{ return std::min (std::max (x, -ceiling), ceiling); }

} // namespace dsp
