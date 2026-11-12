// Filters: the block that decides whether a delay is a delay or a "band delay",
// whether a comb rings or just repeats, and whether a reverb tail goes dark the
// way a room does.
//
// Four families, because the units ask for four different things:
//
//   OnePole    the damping filter in a feedback loop. Cheap, unconditionally
//              stable, and the only sane choice inside a recirculation.
//   SVF        a resonant filter with simultaneous LP/BP/HP/notch outputs. This
//              is what a "cutoff + Q" pair on a front panel means.
//   Biquad     shelves and peaking bells, for tone controls and spatial EQ.
//   Comb       a delay with feedback taken as a FILTER rather than as an echo:
//              short enough that the repeats fuse into a pitch. A comb at 5 ms
//              rings at 200 Hz; the same block at 500 ms is an echo. The length
//              is the whole difference and it is why "Swept Combs" is not
//              "Swept Delays".
#pragma once
#include "Constants.h"
#include "Delay.h"
#include <cmath>
#include <vector>
#include <algorithm>

namespace dsp {

/** One-pole low-pass. `a = 1 - exp(-2*pi*f/sr)`, which is the law the Lexicon
    coefficient table at PCM 81 ROM 0x72970 fits to 0.00% above 13 kHz, so units
    with that evidence and units without it agree on the shape. */
class OnePole
{
public:
    void prepare (double sampleRate) { sr = sampleRate; z = 0.0f; }
    void reset() { z = 0.0f; }
    void setCutoff (double hz)
    {
        if (hz <= 0.0) { a = 0.0f; return; }
        if (hz >= sr * 0.5) { a = 1.0f; return; }
        a = (float) (1.0 - std::exp (-2.0 * kPi * hz / sr));
    }
    float lp (float x) { z += a * (x - z); return z; }
    float hp (float x) { return x - lp (x); }
    float state() const { return z; }

private:
    double sr = 48000.0;
    float a = 1.0f, z = 0.0f;
};

/** Andrew Simper's topology-preserving state-variable filter.

    Chosen over the classic Chamberlin form because it stays correct as the
    cutoff is swept: the Chamberlin SVF blows up above sr/6 and detunes badly
    below it, and every one of these machines sweeps a filter with an LFO or an
    envelope. All four outputs come out of the same two integrators, so a unit
    that wants a notch pays nothing extra. */
class StateVariableFilter
{
public:
    void prepare (double sampleRate) { sr = sampleRate; reset(); set (1000.0, 0.7071); }
    void reset() { ic1 = ic2 = 0.0f; }

    void set (double cutoffHz, double q)
    {
        const double f = std::min (std::max (cutoffHz, 5.0), sr * 0.49);
        g = (float) std::tan (kPi * f / sr);
        k = (float) (1.0 / std::max (0.05, q));
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    /** One sample in, all four responses out. */
    void process (float x, float& lp, float& bp, float& hp, float& notch)
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2; bp = v1; hp = x - k * v1 - v2; notch = x - k * v1;
    }

    float lowpass  (float x) { float l, b, h, n; process (x, l, b, h, n); return l; }
    float bandpass (float x) { float l, b, h, n; process (x, l, b, h, n); return b; }
    float highpass (float x) { float l, b, h, n; process (x, l, b, h, n); return h; }
    float notch    (float x) { float l, b, h, n; process (x, l, b, h, n); return n; }

private:
    double sr = 48000.0;
    float g = 0.1f, k = 1.4f, a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
};

/** Zero-delay-feedback transistor ladder: four one-pole lowpass stages inside a
    resonance loop, the Moog topology.

    Added for the Alesis Andromeda A6 (2026-09-09), whose FILTER 2 the manual
    describes in as many words as "a 4-pole low pass filter which was designed to
    emulate the Moog modular synthesizers". Every machine here with a 24 dB/octave
    lowpass wants this and none of them should carry its own copy.

    Two things it does that a cascade of four `OnePole`s does not, and both are
    audible:

     - **The feedback is resolved in the same sample, not a sample late.** A
       one-sample-delayed loop detunes the resonant peak downward as the cutoff
       rises and goes unstable near sr/4, which is exactly where an envelope
       sweep spends its attack.
     - **The feedback saturates rather than being clipped in gain.** A real
       ladder's resonance is limited by the transistors compressing, so it stays
       musical past unity loop gain and self-oscillates cleanly. Limiting the
       gain instead makes self-oscillation impossible, and clamping the OUTPUT
       makes it buzz. The Synthex's filter section returned NaN for exactly this
       reason before the saturation went in.

    `setResonance(0..1)`: 1.0 is the loop gain at which the filter self-
    oscillates. A machine whose panel self-oscillates before full travel (they
    mostly do) should map its own control onto this so the anchor lands where the
    manual says it does. */
class LadderFilter
{
public:
    void prepare (double sampleRate) { sr = sampleRate; reset(); set (1000.0, 0.0f); }
    void reset() { for (auto& s : z) s = 0.0f; for (auto& s : y) s = 0.0f; }

    void set (double cutoffHz, float resonance)
    {
        const double f = std::min (std::max (cutoffHz, 5.0), sr * 0.45);
        // Pre-warped one-pole coefficient, shared by all four stages.
        const double wd = 2.0 * kPi * f;
        const double T  = 1.0 / sr;
        const double wa = (2.0 / T) * std::tan (wd * T * 0.5);
        g = (float) (wa * T * 0.5);
        G = g / (1.0f + g);
        k = 4.0f * std::min (std::max (resonance, 0.0f), 1.2f);
    }

    void setResonance (float r) { k = 4.0f * std::min (std::max (r, 0.0f), 1.2f); }

    /** SATURATE EVERY STAGE, not only the loop's input - a transistor ladder has a
        differential pair in each of its four stages, and that is where a driven
        Moog's thickness and the grit in its resonance come from (Tabula's BURN
        ladder does the same, Huovilainen's form; Sub37 B-22, 2026-11-02). Each
        stage's input goes through the same curve as the loop's. OFF by default:
        every machine that does not ask for it is unchanged (dsp_tests pins the
        output with it off). A nonlinear loop aliases; a caller turning this on
        oversamples around the ladder. */
    void setStageSaturation (bool on) { stages = on; }

    float process (float x)
    {
        // Resolve the loop: with four identical one-poles of gain G, the
        // closed-form input is x minus k times the (estimated) output.
        const float G4 = G * G * G * G;
        const float S = (((z[0] * G + z[1]) * G + z[2]) * G + z[3]) * (1.0f - G) ;
        const float u = (x - k * S) / (1.0f + k * G4);
        float v = saturate (u);
        for (int i = 0; i < 4; ++i)
        {
            const float in = stages ? saturate (v) : v;
            v = G * (in - z[i]) + z[i];
            z[i] = v + G * (in - z[i]);
            y[i] = v;
        }
        return v;
    }

    /** The stage taps, for a machine that mixes poles rather than taking the
        last one (a 1/2/3/4-pole selector is a weighted sum of these). */
    float pole (int i) const { return y[std::min (std::max (i, 0), 3)]; }

private:
    static float saturate (float v)
    {
        // tanh-shaped, cheap, and monotonic - the ladder's transistor pair.
        const float a = std::min (std::max (v, -3.0f), 3.0f);
        return a * (27.0f + a * a) / (27.0f + 9.0f * a * a);
    }

    double sr = 48000.0;
    float g = 0.1f, G = 0.1f, k = 0.0f;
    float z[4] {}, y[4] {};
    bool stages = false;
};

/** Biquad with the RBJ cookbook coefficients, for shelves and bells.
    Used where a machine prints a tone control rather than a filter: the
    PCM 91's Spatial EQ, the H3000's tone stages, a "high cut" that is a shelf
    rather than a rolloff. */
class Biquad
{
public:
    enum class Type { LowShelf, HighShelf, Peaking, LowPass, HighPass, BandPass };

    void prepare (double sampleRate) { sr = sampleRate; reset(); }
    void reset() { x1 = x2 = y1 = y2 = 0.0f; }

    void set (Type t, double freqHz, double q, double gainDb = 0.0)
    {
        const double w0 = 2.0 * kPi * std::min (std::max (freqHz, 5.0), sr * 0.49) / sr;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double A  = std::pow (10.0, gainDb / 40.0);
        const double alpha = sw / (2.0 * std::max (0.05, q));
        double b0, b1, b2, a0, a1c, a2c;
        switch (t)
        {
            case Type::LowShelf:
            {
                const double s = 2.0 * std::sqrt (A) * alpha;
                b0 =      A * ((A + 1) - (A - 1) * cw + s);
                b1 =  2 * A * ((A - 1) - (A + 1) * cw);
                b2 =      A * ((A + 1) - (A - 1) * cw - s);
                a0 =           (A + 1) + (A - 1) * cw + s;
                a1c = -2 *    ((A - 1) + (A + 1) * cw);
                a2c =          (A + 1) + (A - 1) * cw - s;
                break;
            }
            case Type::HighShelf:
            {
                const double s = 2.0 * std::sqrt (A) * alpha;
                b0 =      A * ((A + 1) + (A - 1) * cw + s);
                b1 = -2 * A * ((A - 1) + (A + 1) * cw);
                b2 =      A * ((A + 1) + (A - 1) * cw - s);
                a0 =           (A + 1) - (A - 1) * cw + s;
                a1c =  2 *    ((A - 1) - (A + 1) * cw);
                a2c =          (A + 1) - (A - 1) * cw - s;
                break;
            }
            case Type::Peaking:
                b0 = 1 + alpha * A;  b1 = -2 * cw;  b2 = 1 - alpha * A;
                a0 = 1 + alpha / A;  a1c = -2 * cw; a2c = 1 - alpha / A;
                break;
            case Type::LowPass:
                b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
                a0 = 1 + alpha;    a1c = -2 * cw; a2c = 1 - alpha;
                break;
            case Type::HighPass:
                b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
                a0 = 1 + alpha;    a1c = -2 * cw;  a2c = 1 - alpha;
                break;
            case Type::BandPass:
            default:
                b0 = alpha;     b1 = 0;        b2 = -alpha;
                a0 = 1 + alpha; a1c = -2 * cw; a2c = 1 - alpha;
                break;
        }
        cb0 = (float) (b0 / a0); cb1 = (float) (b1 / a0); cb2 = (float) (b2 / a0);
        ca1 = (float) (a1c / a0); ca2 = (float) (a2c / a0);
    }

    float process (float x)
    {
        const float y = cb0 * x + cb1 * x1 + cb2 * x2 - ca1 * y1 - ca2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }

private:
    double sr = 48000.0;
    float cb0 = 1, cb1 = 0, cb2 = 0, ca1 = 0, ca2 = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
};

/** Feedback comb: `y[n] = x[n] + g * lp(y[n - D])`.

    This is the resonator, not the echo. At D short enough for the repeats to
    fuse (under ~50 ms) the ear stops counting them and hears a pitch at 1/D,
    with harmonics of it, and that is a comb. The damping in the loop is what
    keeps a swept comb from screaming at the top of its sweep.

    `setTuning` takes the frequency the comb should ring at, which is how a
    front panel asks for it; `setDelay` takes the time, which is how a delay
    algorithm asks for the same block. */
class CombFilter
{
public:
    void prepare (double sampleRate, double maxSeconds = 0.2)
    { sr = sampleRate; line.prepare (sampleRate, maxSeconds); damp.prepare (sampleRate); damp.setCutoff (sampleRate * 0.45); reset(); }

    void reset() { line.reset(); damp.reset(); }

    void setDelaySamples (double d) { delaySamples = d; }
    void setDelaySeconds (double s) { delaySamples = s * sr; }
    /** Ring at this pitch. A comb's fundamental is sr/D. */
    void setTuning (double hz) { delaySamples = sr / std::max (10.0, hz); }
    void setFeedback (float g) { fb = std::min (std::max (g, -0.999f), 0.999f); }
    void setDamping (double hz) { damp.setCutoff (hz); }
    /** Resonance as a decay time: how long the ring takes to fall 60 dB. */
    void setDecaySeconds (double t)
    {
        const double loop = delaySamples / sr;
        fb = (float) std::min (0.999, std::pow (10.0, -3.0 * loop / std::max (0.01, t)));
    }

    float process (float x)
    {
        const float d = line.readCubic (delaySamples);
        line.push (x + damp.lp (d) * fb);
        return d;
    }

private:
    DelayLine line; OnePole damp;
    double sr = 48000.0, delaySamples = 480.0;
    float fb = 0.0f;
};

/** Feed-forward comb: `y[n] = x[n] + g * x[n - D]`. No feedback, so no ring —
    just the notches. This is the comb a flanger's dry-plus-delayed sum makes,
    and the one to reach for when a machine wants notches without resonance. */
class FeedForwardComb
{
public:
    void prepare (double sampleRate, double maxSeconds = 0.1)
    { sr = sampleRate; line.prepare (sampleRate, maxSeconds); }
    void reset() { line.reset(); }
    void setDelaySamples (double d) { delaySamples = d; }
    void setDelaySeconds (double s) { delaySamples = s * sr; }
    void setGain (float g) { gain = g; }
    float process (float x) { const float d = line.readCubic (delaySamples); line.push (x); return x + gain * d; }

private:
    DelayLine line;
    double sr = 48000.0, delaySamples = 100.0;
    float gain = 1.0f;
};

/** Schroeder allpass: flat magnitude, dispersive phase. The diffuser every
    reverb is built from, and the block whose delay counts for `D*(1+g)/(1-g)`
    when it sits inside a feedback loop (see Reverb.h — getting that wrong put
    a floor under the PCM 91's reverb time for a day). */
class AllpassFilter
{
public:
    void prepare (double sampleRate, double maxSeconds = 0.1)
    { line.prepare (sampleRate, maxSeconds); sr = sampleRate; }
    void reset() { line.reset(); }
    void setDelaySamples (double d) { delaySamples = d; }
    void setDelaySeconds (double s) { delaySamples = s * sr; }
    void setGain (float g) { this->g = std::min (std::max (g, -0.95f), 0.95f); }

    float process (float x) { return process (x, delaySamples); }
    /** Modulated form: the delay is given per sample, for a tank whose taps
        wander. */
    float process (float x, double d)
    {
        const float y = line.readCubic (d);
        const float v = x + g * y;
        line.push (v);
        return y - g * v;
    }
    /** What this allpass is worth as a pure delay when it sits in a loop. */
    double loopEquivalent() const { return delaySamples * (1.0 + g) / std::max (0.05f, 1.0f - g); }
    /** Read the allpass's own line at an arbitrary offset, without disturbing
        it. A figure-of-eight tank reads its stereo field from points INSIDE the
        tank rather than from its output, and three of those points are
        allpasses; see `ReverbFigure8.h`. */
    float tap (double d) const { return line.readLinear (d); }

private:
    DelayLine line;
    double sr = 48000.0, delaySamples = 100.0;
    float g = 0.5f;
};

/** A bank of bandpass filters with independent gains: the shape behind a
    vocoder, a "band delay" and a graphic EQ.

    Bands are spaced logarithmically because that is how the ear divides the
    spectrum and how every machine that prints band centres actually spaces
    them. */
class FilterBank
{
public:
    static constexpr int kMaxBands = 32;

    void prepare (double sampleRate, int bands, double loHz, double hiHz, double q = 4.0)
    {
        sr = sampleRate;
        n = std::min (std::max (bands, 1), kMaxBands);
        for (int i = 0; i < n; ++i)
        {
            const double t = (n == 1) ? 0.5 : (double) i / (n - 1);
            centres[i] = loHz * std::pow (hiHz / loHz, t);
            svf[i].prepare (sampleRate);
            svf[i].set (centres[i], q);
            gains[i] = 1.0f;
        }
        this->q = q;
    }

    void setBandGain (int i, float g) { if (i >= 0 && i < n) gains[i] = g; }
    void setQ (double newQ) { q = newQ; for (int i = 0; i < n; ++i) svf[i].set (centres[i], q); }
    void setBandCentre (int i, double hz) { if (i >= 0 && i < n) { centres[i] = hz; svf[i].set (hz, q); } }
    int  bands() const { return n; }
    double centre (int i) const { return (i >= 0 && i < n) ? centres[i] : 0.0; }

    /** Split into bands, apply the gains, sum. */
    float process (float x)
    {
        float sum = 0.0f;
        for (int i = 0; i < n; ++i) sum += svf[i].bandpass (x) * gains[i];
        return sum;
    }
    /** Split only: the caller gets each band to do its own thing with (delay
        it, shift it, gate it). */
    void split (float x, float* out)
    { for (int i = 0; i < n; ++i) out[i] = svf[i].bandpass (x); }

private:
    StateVariableFilter svf[kMaxBands];
    double centres[kMaxBands] {}, sr = 48000.0, q = 4.0;
    float gains[kMaxBands] {};
    int n = 0;
};

/** DC blocker. Anything with asymmetric drive or a long feedback loop needs
    one, and its absence shows up as a slowly wandering offset that eats
    headroom without being audible until it clips. */
class DCBlocker
{
public:
    void prepare (double sampleRate) { r = (float) (1.0 - 2.0 * kPi * 10.0 / sampleRate); reset(); }
    void reset() { x1 = y1 = 0.0f; }
    float process (float x) { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
private:
    float r = 0.999f, x1 = 0.0f, y1 = 0.0f;
};

} // namespace dsp
