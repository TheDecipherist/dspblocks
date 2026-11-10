// Capture and replay of a chunk of audio: the block behind the TC 2290's
// sampler, the H3000's Studio Sampler and Stutter, and every reverse effect.
//
// It is a separate block from the delay line even though both are circular
// buffers, because the CONTROLS are different in kind. A delay is driven by a
// time; a sampler is driven by transport — record, play, stop, loop, a start
// point, a length, a direction and a speed. Modelling a sampler as "a delay
// with a long time" is how a machine's sampler ends up sounding like a delay,
// which is one of the faults this library exists to fix.
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace dsp {

/** Record-and-replay with independent start, length, speed and direction.

    Replay is fractional and cubic-interpolated so speed can be any ratio, which
    is what makes "play it back an octave down and backwards" a setting rather
    than a special case. */
class Sampler
{
public:
    enum class Mode { Idle, Recording, Playing };

    void prepare (double sampleRate, double maxSeconds)
    {
        sr = sampleRate;
        buf.assign ((size_t) std::ceil (sampleRate * std::max (0.05, maxSeconds)) + 4, 0.0f);
        reset();
    }

    void reset()
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        w = 0; recorded = 0; readPos = 0.0; mode = Mode::Idle;
    }

    void record() { w = 0; recorded = 0; mode = Mode::Recording; }
    void stop()   { mode = Mode::Idle; }
    void play()   { if (recorded > 0) { readPos = startPos(); mode = Mode::Playing; } }

    void setLoop (bool b) { looping = b; }
    void setReverse (bool b) { reverse = b; }
    /** 1.0 is as recorded; 0.5 is an octave down and half speed. */
    void setSpeed (double s) { speed = std::max (0.01, std::min (std::fabs (s), 8.0)); }
    /** Window into the recording, both as a fraction 0..1. */
    void setWindow (double startFrac, double lengthFrac)
    {
        startF  = std::min (std::max (startFrac, 0.0), 1.0);
        lengthF = std::min (std::max (lengthFrac, 0.001), 1.0);
    }

    Mode  state() const { return mode; }
    double recordedSeconds() const { return (double) recorded / sr; }
    bool  hasContent() const { return recorded > 0; }

    float process (float x)
    {
        switch (mode)
        {
            case Mode::Recording:
                if (w < buf.size()) { buf[w++] = x; recorded = w; }
                else mode = Mode::Idle;
                return x;

            case Mode::Playing:
            {
                const double lo = startPos();
                const double len = std::max (4.0, lengthF * (double) recorded);
                const double hi = std::min ((double) recorded - 1.0, lo + len);
                if (hi <= lo) { mode = Mode::Idle; return 0.0f; }
                const float y = read (readPos);
                readPos += reverse ? -speed : speed;
                if (readPos >= hi) { if (looping) readPos = lo; else { mode = Mode::Idle; } }
                if (readPos < lo)  { if (looping) readPos = hi; else { mode = Mode::Idle; } }
                return y;
            }

            case Mode::Idle:
            default:
                return 0.0f;
        }
    }

private:
    double startPos() const { return startF * std::max (0.0, (double) recorded - 4.0); }

    float read (double p) const
    {
        if (recorded < 4) return 0.0f;
        const long i1 = (long) p;
        const long i0 = std::max (0L, i1 - 1), i2 = std::min ((long) recorded - 1, i1 + 1), i3 = std::min ((long) recorded - 1, i1 + 2);
        if (i1 < 0 || i1 >= (long) recorded) return 0.0f;
        const float f = (float) (p - (double) i1);
        const float ym1 = buf[(size_t) i0], y0 = buf[(size_t) i1], y1 = buf[(size_t) i2], y2 = buf[(size_t) i3];
        const float c0 = y0, c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + c0;
    }

    std::vector<float> buf;
    size_t w = 0, recorded = 0;
    double sr = 48000.0, readPos = 0.0, speed = 1.0, startF = 0.0, lengthF = 1.0;
    Mode mode = Mode::Idle;
    bool looping = true, reverse = false;
};

/** Stutter: a rolling capture replayed in short slices.

    Different from a sampler because nothing has to be triggered — it runs
    continuously, taking the last `length` of audio and repeating it `count`
    times before moving on. That is the H3000 STUTTER algorithm's shape (its
    parameters are literally length 1 / count 1 / length 2 / count 2) and it is
    also how a "glitch" effect is built.

    Slices are crossfaded at the seam, because a hard splice on a sustained tone
    clicks and the click is louder than the effect. */
class Stutter
{
public:
    void prepare (double sampleRate, double maxSliceSeconds = 2.0)
    {
        sr = sampleRate;
        buf.assign ((size_t) std::ceil (sampleRate * maxSliceSeconds) + 8, 0.0f);
        reset();
    }

    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; readPos = 0.0; rep = 0; capturing = true; }

    void setSliceSeconds (double s) { sliceLen = std::max (0.005, std::min (s, (double) buf.size() / sr - 0.01)); }
    /** How many times each captured slice repeats. 1 is a bypass. */
    void setRepeats (int c) { repeats = std::max (1, c); }
    void setSpeed (double s) { speed = std::max (0.1, std::min (s, 4.0)); }
    void setReverse (bool b) { reverse = b; }

    float process (float x)
    {
        const double n = sliceLen * sr;
        if (capturing)
        {
            buf[w] = x;
            if (++w >= (size_t) n) { w = 0; capturing = false; rep = 0; readPos = reverse ? n - 1.0 : 0.0; }
            return x;
        }
        const float y = read (readPos, (size_t) n);
        readPos += reverse ? -speed : speed;
        if (readPos >= n || readPos < 0.0)
        {
            if (++rep >= repeats) { capturing = true; w = 0; }
            else readPos = reverse ? n - 1.0 : 0.0;
        }
        return y;
    }

private:
    float read (double p, size_t n) const
    {
        if (n < 4) return 0.0f;
        const long i = (long) p;
        if (i < 0 || i >= (long) n) return 0.0f;
        const long j = std::min ((long) n - 1, i + 1);
        const float f = (float) (p - (double) i);
        float y = buf[(size_t) i] * (1.0f - f) + buf[(size_t) j] * f;
        // 64-sample equal-power fade at both ends of the slice, so the splice
        // does not click
        const double fade = std::min (64.0, (double) n * 0.25);
        if (p < fade) y *= (float) std::sin (0.5 * M_PI * p / fade);
        else if (p > (double) n - fade) y *= (float) std::sin (0.5 * M_PI * ((double) n - p) / fade);
        return y;
    }

    std::vector<float> buf;
    size_t w = 0;
    double sr = 48000.0, readPos = 0.0, sliceLen = 0.1, speed = 1.0;
    int repeats = 2, rep = 0;
    bool capturing = true, reverse = false;
};

/** Continuous reverse: a rolling buffer read backwards, so audio comes out
    reversed in blocks of `windowSeconds` with no trigger.

    Two windows half a period apart and crossfaded, for the same reason the
    pitch shifter uses two read pointers: one is always far from the write head,
    so the seam never lands on the moment the buffer is being written. */
class ReverseBuffer
{
public:
    void prepare (double sampleRate, double maxWindowSeconds = 2.0)
    {
        sr = sampleRate;
        buf.assign ((size_t) std::ceil (sampleRate * maxWindowSeconds) + 8, 0.0f);
        setWindow (0.4);
        reset();
    }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; phase = 0.0; }

    void setWindow (double seconds)
    { win = std::max (0.02, std::min (seconds, (double) buf.size() / sr - 0.01)); }

    float process (float x)
    {
        const double n = win * sr;
        buf[w] = x;
        w = (w + 1) % buf.size();
        phase += 1.0;
        if (phase >= n) phase -= n;

        const double pa = phase, pb = std::fmod (phase + n * 0.5, n);
        const float ga = (float) std::sin (M_PI * pa / n);
        const float gb = (float) std::sin (M_PI * pb / n);
        // Read backwards: the newest sample of the window comes out first.
        const float a = tap (n - pa);
        const float b = tap (n - pb + n * 0.5);
        return (a * ga + b * gb) * 0.7071f;
    }

private:
    float tap (double back) const
    {
        double idx = (double) w - back;
        while (idx < 0) idx += (double) buf.size();
        const size_t i = (size_t) idx % buf.size();
        const size_t j = (i + 1) % buf.size();
        const float f = (float) (idx - std::floor (idx));
        return buf[i] * (1.0f - f) + buf[j] * f;
    }

    std::vector<float> buf;
    size_t w = 0;
    double sr = 48000.0, win = 0.4, phase = 0.0;
};

} // namespace dsp
