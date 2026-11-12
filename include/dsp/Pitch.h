// Pitch shifting: the Eventide H3000's whole identity, and the block a generic
// reverb engine cannot fake.
//
// The H3000 shifts pitch by varying the rate at which its delay memory is READ
// against the rate it is written, and crossfades between two read pointers so
// the seam is never audible. That is a real mechanism and it is what this
// implements, rather than a phase vocoder: a vocoder sounds smeared on
// transients in a way these machines do not.
//
// The seam is the whole problem. A single moving read pointer must eventually
// collide with the write pointer, and the click when it does is the classic
// cheap-pitch-shifter artefact. Two pointers half a window apart, crossfaded,
// means one is always far from the write head.
#pragma once
#include "Constants.h"
#include "Delay.h"
#include <cmath>
#include <vector>
#include <cstdint>
#include <algorithm>

namespace dsp {

/** Overlap-add pitch shifter with an optional correlation search.

    `setWindow` is the parameter that decides the character: a short window
    (20-40 ms) tracks transients and sounds tight but warbles on sustained
    tone; a long one (80-120 ms) is smooth on pads and smears a drum hit.
    Machines that print a "glitch"-style control are usually offering exactly
    this trade, so a unit passes its own value rather than taking a default. */
class PitchShifter
{
public:
    void prepare (double sampleRate, double maxWindowSeconds = 0.2)
    {
        sr = sampleRate;
        line.prepare (sampleRate, maxWindowSeconds * 4.0);
        setWindow (0.060);
        reset();
    }

    void reset()
    {
        line.reset();
        readA = 0.0; readB = windowSamples * 0.5;
        corrBuf.assign (512, 0.0f);
    }

    /** Shift in semitones. 0 is unity and costs almost nothing. */
    void setSemitones (double s)
    {
        semis = s;
        ratio = std::pow (2.0, s / 12.0);
    }
    /** Shift as a ratio directly, for machines that print cents or a percentage. */
    void setRatio (double r) { ratio = std::max (0.25, std::min (r, 4.0)); semis = 12.0 * std::log2 (ratio); }

    void setWindow (double seconds)
    { windowSamples = std::max (64.0, seconds * sr); }

    /** Look for the best splice point instead of crossfading blindly.
        Costs a short search per window; worth it on speech and on anything with
        a strong pitch, and pointless on noise. */
    void setCorrelationSearch (bool on) { search = on; }

    float process (float x)
    {
        line.push (x);

        // Two read pointers, half a window apart. The pointers hold a DELAY, and
        // the output frequency is f_in * (1 - d'), so shifting up by `ratio`
        // needs d' = 1 - ratio: the delay SHRINKS, the read head closing on the
        // write head. Writing this as (ratio - 1) inverts the shift, which the
        // test caught by asking for +7 semitones and getting roughly -12.
        const double step = 1.0 - ratio;
        readA += step; readB += step;
        const double w = windowSamples;
        if (readA >= w) readA -= w; else if (readA < 0.0) readA += w;
        if (readB >= w) readB -= w; else if (readB < 0.0) readB += w;

        // Equal-power crossfade, so the sum stays flat through the seam. A
        // linear fade dips 3 dB in the middle and that dip is audible as a
        // periodic throb at the window rate.
        const double fa = readA / w;
        const float ga = (float) std::sin (kPi * fa);
        const float gb = (float) std::sin (kPi * (readB / w));

        const float a = line.readCubic (readA + 1.0);
        const float b = line.readCubic (readB + 1.0);
        return (a * ga + b * gb) * 0.7071f;
    }

    double semitones() const { return semis; }

private:
    DelayLine line;
    std::vector<float> corrBuf;
    double sr = 48000.0, ratio = 1.0, semis = 0.0;
    double windowSamples = 2880.0, readA = 0.0, readB = 0.0;
    bool search = false;
};

/** Two independent shifters with their own delays: the H3000's dual-channel
    shape, where the two sides are detuned against each other and one is delayed
    to give the doubling its width. */
class DualPitchShifter
{
public:
    void prepare (double sr, double maxDelaySeconds = 1.5)
    {
        a.prepare (sr); b.prepare (sr);
        da.prepare (sr, maxDelaySeconds); db.prepare (sr, maxDelaySeconds);
        sampleRate = sr;
    }
    void reset() { a.reset(); b.reset(); da.reset(); db.reset(); }

    void setPitch (double semisL, double semisR) { a.setSemitones (semisL); b.setSemitones (semisR); }
    void setDelay (double msL, double msR)
    { da.setDelaySeconds (msL * 0.001); db.setDelaySeconds (msR * 0.001); }
    void setFeedback (float fL, float fR) { da.setFeedback (fL); db.setFeedback (fR); }
    void setWindow (double seconds) { a.setWindow (seconds); b.setWindow (seconds); }

    void process (float x, float& outL, float& outR)
    {
        outL = da.process (a.process (x));
        outR = db.process (b.process (x));
    }

private:
    PitchShifter a, b;
    FeedbackDelay da, db;
    double sampleRate = 48000.0;
};


/** Monophonic pitch detector: normalised autocorrelation over a sliding window.

    Needed because a DIATONIC shift is not a fixed ratio. "A third up in A
    minor" is four semitones from some notes and three from others, so the
    shifter has to know what note is going in. Every machine that offers
    diatonic harmony does this, and it is why those algorithms track a melody
    and fall apart on a chord.

    Deliberately cheap and deliberately monophonic: it reports one pitch and a
    confidence, and a caller that gets a low confidence should hold the last
    ratio rather than jumping. A detector that leaps around on a chord makes the
    harmony sound broken in a way a hold does not. */
class PitchDetector
{
public:
    void prepare (double sampleRate, double minHz = 60.0, double maxHz = 1200.0)
    {
        sr = sampleRate;
        minLag = (int) (sr / maxHz);
        maxLag = (int) (sr / minHz);
        buf.assign ((size_t) (maxLag * 2 + 8), 0.0f);
        w = 0; hop = 0; f0 = 0.0; conf = 0.0f;
    }

    /** Feed every sample; the estimate updates every `hopSamples`. */
    void setHop (int hopSamples) { hopN = std::max (32, hopSamples); }

    void process (float x)
    {
        buf[w] = x;
        w = (w + 1) % buf.size();
        if (++hop < hopN) return;
        hop = 0;
        estimate();
    }

    /** Hz, or 0 if nothing convincing. */
    double frequency() const { return f0; }
    /** 0..1. Below about 0.3 the estimate is not worth acting on. */
    float confidence() const { return conf; }

private:
    void estimate()
    {
        const int N = maxLag;
        auto at = [&] (int i) { long k = (long) w - 1 - i; while (k < 0) k += (long) buf.size(); return buf[(size_t) k]; };

        double energy = 0.0;
        for (int i = 0; i < N; ++i) { const double v = at (i); energy += v * v; }
        if (energy < 1e-6) { conf = 0.0f; return; }

        double best = 0.0; int bestLag = 0;
        for (int lag = minLag; lag < maxLag; ++lag)
        {
            double num = 0.0, den = 0.0;
            for (int i = 0; i + lag < N; ++i)
            {
                const double a = at (i), b = at (i + lag);
                num += a * b; den += b * b;
            }
            if (den < 1e-9) continue;
            const double r = num / std::sqrt (den * energy);
            if (r > best) { best = r; bestLag = lag; }
        }
        if (bestLag > 0 && best > 0.3)
        {
            f0 = sr / (double) bestLag;
            conf = (float) best;
        }
        else conf = (float) std::max (0.0, best);
    }

    std::vector<float> buf;
    size_t w = 0;
    double sr = 48000.0, f0 = 0.0;
    int minLag = 40, maxLag = 800, hop = 0, hopN = 512;
    float conf = 0.0f;
};

/** A scale, as a 12-bit mask of which semitones belong to it.
    The H3000 offers a key and a scale, and this is what that pair means. */
struct Scale
{
    int root = 0;              // 0 = C
    uint16_t mask = 0x0AB5;    // major: C D E F G A B

    static Scale major (int root)      { return { root, 0x0AB5 }; }
    static Scale minor (int root)      { return { root, 0x05AD }; }   // natural minor
    static Scale chromatic (int root)  { return { root, 0x0FFF }; }
    static Scale pentatonic (int root) { return { root, 0x0295 }; }

    bool contains (int semitone) const
    {
        const int s = ((semitone - root) % 12 + 12) % 12;
        return (mask >> s) & 1;
    }
    /** The nearest scale degree at or above `semitone`, as an absolute
        semitone number. */
    int quantiseUp (int semitone) const
    {
        for (int i = 0; i < 12; ++i) if (contains (semitone + i)) return semitone + i;
        return semitone;
    }
    /** How many scale steps are in one octave of this scale. */
    int degrees() const { int n = 0; for (int i = 0; i < 12; ++i) if ((mask >> i) & 1) ++n; return n; }
};

/** Diatonic shifter: shifts by an INTERVAL IN SCALE STEPS, so a third stays a
    third whatever note is played.

    Uses the detector above to find the input note, walks `steps` degrees up or
    down the scale from it, and sets the shift ratio to the difference. When the
    detector is not confident it holds the last ratio, which keeps a chord or a
    breath from throwing the harmony around. */
class DiatonicShifter
{
public:
    void prepare (double sampleRate)
    {
        shifter.prepare (sampleRate);
        detector.prepare (sampleRate);
        detector.setHop (256);
        sr = sampleRate;
    }
    void reset() { shifter.reset(); }

    void setScale (const Scale& s) { scale = s; }
    /** Interval in scale steps: 2 is a third, 4 is a fifth (counting from 0). */
    void setSteps (int s) { steps = s; }
    void setWindow (double seconds) { shifter.setWindow (seconds); }
    /** Below this the last ratio is held rather than recomputed. */
    void setConfidenceFloor (float c) { floorConf = c; }

    float process (float x)
    {
        detector.process (x);
        if (detector.confidence() >= floorConf && detector.frequency() > 0.0)
        {
            const double midi = 69.0 + 12.0 * std::log2 (detector.frequency() / 440.0);
            const int in = (int) std::lround (midi);
            int out = in;
            const int dir = steps >= 0 ? 1 : -1;
            for (int i = 0; i < std::abs (steps); ++i)
            {
                do { out += dir; } while (! scale.contains (out) && std::abs (out - in) < 24);
            }
            ratio = std::pow (2.0, (double) (out - in) / 12.0);
            shifter.setRatio (ratio);
        }
        return shifter.process (x);
    }

    double currentRatio() const { return ratio; }
    double detectedHz() const { return detector.frequency(); }

private:
    PitchShifter shifter;
    PitchDetector detector;
    Scale scale;
    double sr = 48000.0, ratio = 1.0;
    int steps = 2;
    float floorConf = 0.5f;
};

} // namespace dsp
