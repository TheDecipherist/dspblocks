// Delay lines: the block every echo, flanger, chorus, comb and reverb tap is
// built from.
//
// The read is fractional and interpolated, because a delay whose length can
// only be a whole sample cannot be swept without stepping, and a stepping sweep
// is what makes a flanger sound like a broken chorus.
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace dsp {

/** A power-of-two circular buffer with a fractional read.

    Two interpolations are offered because they are not interchangeable:
    `readLinear` is what a machine with a short integer buffer and a fraction
    actually did (the Supernova's chorus is documented as exactly that, and
    using anything better changes its character), while `readCubic` is for a
    long modulated tap where linear interpolation's high-frequency loss is
    audible as the tap moves. */
class DelayLine
{
public:
    void prepare (double sampleRate, double maxSeconds)
    {
        sr = sampleRate > 0 ? sampleRate : 48000.0;
        size_t want = (size_t) std::ceil (sr * std::max (0.001, maxSeconds)) + 4;
        size_t n = 1; while (n < want) n <<= 1;      // power of two so the wrap is a mask
        buf.assign (n, 0.0f);
        mask = n - 1;
        w = 0;
    }

    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }

    void push (float x) { buf[w] = x; w = (w + 1) & mask; }

    /** Read `d` samples back. d is clamped into the buffer, so a caller asking
        for more than was allocated gets the longest available delay rather than
        reading memory that was never written. */
    float readLinear (double d) const
    {
        d = clampDelay (d);
        const double rp = (double) w - d + (double) buf.size();
        const size_t i0 = (size_t) rp & mask;
        const size_t i1 = (i0 + 1) & mask;
        const float f = (float) (rp - std::floor (rp));
        return buf[i0] + f * (buf[i1] - buf[i0]);
    }

    /** Catmull-Rom. Costs three more multiplies and keeps the top octave
        intact while the tap moves. */
    float readCubic (double d) const
    {
        d = clampDelay (d);
        const double rp = (double) w - d + (double) buf.size();
        const size_t i1 = (size_t) rp & mask;
        const size_t i0 = (i1 - 1) & mask, i2 = (i1 + 1) & mask, i3 = (i1 + 2) & mask;
        const float f  = (float) (rp - std::floor (rp));
        const float ym1 = buf[i0], y0 = buf[i1], y1 = buf[i2], y2 = buf[i3];
        const float c0 = y0;
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + c0;
    }

    double maxDelay() const { return (double) buf.size() - 2.0; }
    double sampleRate() const { return sr; }

private:
    /** The floor is ONE sample, not zero.

        Every consumer in this library reads before it writes, so at read time
        `w` points at the slot the current input is about to go into and the
        most recent sample is at `w - 1`. Asking for a delay of 0 therefore
        reads `buf[w]`, which is the OLDEST sample in the buffer: a delay of
        zero comes out as a delay of the whole buffer. The H3000's Reverb
        Factory found this the hard way - its predelay parameter at 0 ms put
        the entire reverb three seconds late, and the algorithm measured as
        silent. */
    double clampDelay (double d) const
    { return std::min (std::max (d, 1.0), maxDelay()); }

    std::vector<float> buf;
    size_t mask = 0, w = 0;
    double sr = 48000.0;
};

/** One delay tap with feedback and a damping filter in the loop.

    The damping matters more than it looks: a feedback delay with no loss in the
    loop returns every repeat with its top end intact, which no real machine
    does. Tape loses highs, a BBD loses highs and adds noise, and a digital unit
    with a feedback filter loses whatever its filter takes. Without it, repeats
    pile up bright and the effect stops sounding like an echo. */
class FeedbackDelay
{
public:
    void prepare (double sampleRate, double maxSeconds)
    {
        line.prepare (sampleRate, maxSeconds);
        sr = sampleRate;
        lp = hp = 0.0f;
    }

    void reset() { line.reset(); lp = hp = 0.0f; }

    void setDelaySeconds (double s)  { delaySamples = s * sr; }
    void setDelaySamples (double s)  { delaySamples = s; }
    void setFeedback (float f)       { fb = std::min (std::max (f, -0.999f), 0.999f); }

    /** Damping in the feedback path, as corner frequencies in Hz.
        `highCut` 20000 and `lowCut` 0 is a clean digital repeat. */
    void setDamping (double highCutHz, double lowCutHz = 0.0)
    {
        hcA = coef (highCutHz);
        lcA = lowCutHz > 0.0 ? coef (lowCutHz) : 0.0f;
    }

    /** Cubic read: a modulated tap is the common case for this block. */
    float process (float x)
    {
        const float d = line.readCubic (delaySamples);
        float f = d;
        if (hcA > 0.0f) { lp += hcA * (f - lp); f = lp; }
        if (lcA > 0.0f) { hp += lcA * (f - hp); f -= hp; }
        line.push (x + f * fb);
        return d;
    }

    /** Read without advancing, for a caller driving the write itself. */
    float tap (double samples) const { return line.readCubic (samples); }
    void  write (float x) { line.push (x); }
    DelayLine& raw() { return line; }

private:
    float coef (double hz) const
    {
        if (hz <= 0.0 || hz >= sr * 0.5) return 0.0f;
        return (float) (1.0 - std::exp (-2.0 * M_PI * hz / sr));
    }
    DelayLine line;
    double sr = 48000.0, delaySamples = 0.0;
    float fb = 0.0f, lp = 0.0f, hp = 0.0f, hcA = 0.0f, lcA = 0.0f;
};

/** A multi-tap delay: the Ultra-Tap / Digiplex shape.

    Taps are independent in time, level and pan, and the feedback is taken from
    one nominated tap rather than from the sum, which is how these machines
    behave: the repeat pattern stays put while the tail decays. */
class MultiTapDelay
{
public:
    static constexpr int kMaxTaps = 16;

    void prepare (double sampleRate, double maxSeconds)
    { line.prepare (sampleRate, maxSeconds); sr = sampleRate; lp = 0.0f; }

    void reset() { line.reset(); lp = 0.0f; }

    void setTap (int i, double seconds, float level, float pan)
    {
        if (i < 0 || i >= kMaxTaps) return;
        taps[i] = { seconds * sr, level, std::min (std::max (pan, 0.0f), 1.0f) };
        if (i >= n) n = i + 1;
    }
    void setTapCount (int c) { n = std::min (std::max (c, 0), kMaxTaps); }
    void setFeedback (float f, int fromTap = -1)
    { fb = std::min (std::max (f, -0.99f), 0.99f); fbTap = fromTap; }
    void setDamping (double highCutHz)
    { hcA = (highCutHz > 0 && highCutHz < sr * 0.5)
              ? (float) (1.0 - std::exp (-2.0 * M_PI * highCutHz / sr)) : 0.0f; }

    void process (float x, float& outL, float& outR)
    {
        outL = outR = 0.0f;
        float fbSrc = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float v = line.readCubic (taps[i].samples);
            outL += v * taps[i].level * (1.0f - taps[i].pan);
            outR += v * taps[i].level * taps[i].pan;
            if (i == fbTap || (fbTap < 0 && i == n - 1)) fbSrc = v;
        }
        if (hcA > 0.0f) { lp += hcA * (fbSrc - lp); fbSrc = lp; }
        line.push (x + fbSrc * fb);
    }

private:
    struct Tap { double samples = 0; float level = 0, pan = 0.5f; };
    DelayLine line;
    Tap taps[kMaxTaps];
    int n = 0, fbTap = -1;
    double sr = 48000.0;
    float fb = 0.0f, lp = 0.0f, hcA = 0.0f;
};

} // namespace dsp
