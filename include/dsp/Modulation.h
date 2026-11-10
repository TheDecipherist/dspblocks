// Phaser, flanger, chorus, tremolo and rotary: the modulation family.
//
// These four are constantly confused with each other in plugin code, and the
// confusion is audible, so the distinction is worth stating once:
//
//   phaser   a chain of ALLPASS sections, swept. Notches are NOT harmonically
//            spaced, and there are (stages/2) of them. Nothing is delayed.
//   flanger  a SHORT delay (0.1-10 ms) swept, mixed with the dry signal, with
//            feedback. Notches ARE harmonically spaced, which is the sound of a
//            comb and the reason a flanger sounds metallic and a phaser does not.
//   chorus   a LONGER delay (10-40 ms) swept gently, usually several voices,
//            little or no feedback. Too short and it flanges; too much feedback
//            and it flanges.
//   tremolo  amplitude only. No delay anywhere.
//
// Getting a flanger's delay range wrong turns it into a chorus, which is exactly
// the failure this library exists to stop.
#pragma once
#include "Delay.h"
#include <cmath>
#include <array>

namespace dsp {

/** A one-pole low-pass, duplicated here so this header does not have to depend
    on Filters.h. Same law: a = 1 - exp(-2*pi*f/sr). */
class OnePoleLite
{
public:
    void prepare (double sampleRate) { sr = sampleRate; z = 0.0f; }
    void reset() { z = 0.0f; }
    void setCutoff (double hz)
    { a = (hz <= 0.0) ? 0.0f : (hz >= sr * 0.5 ? 1.0f : (float) (1.0 - std::exp (-2.0 * M_PI * hz / sr))); }
    float lp (float x) { z += a * (x - z); return z; }
private:
    double sr = 48000.0; float a = 1.0f, z = 0.0f;
};

/** A low-frequency oscillator with the shapes these machines actually offer.
    Phase is kept in turns (0..1) so a caller can set an offset per voice. */
class LFO
{
public:
    enum class Shape { Sine, Triangle, Square, Saw, Random };

    void prepare (double sampleRate) { sr = sampleRate; phase = 0.0; hold = 0.0f; }
    void setRate (double hz) { inc = hz / sr; }
    void setShape (Shape s) { shape = s; }
    void setPhase (double turns) { phase = turns - std::floor (turns); }
    /** Where the cycle is now, in turns [0, 1): read-only, for a rate lamp on a panel. */
    [[nodiscard]] double getPhase() const noexcept { return phase; }

    /** Returns -1..+1. */
    float tick()
    {
        const double p = phase;
        phase += inc;
        if (phase >= 1.0)
        {
            phase -= std::floor (phase);
            if (shape == Shape::Random) hold = 2.0f * (float) rnd() - 1.0f;
        }
        switch (shape)
        {
            case Shape::Sine:     return (float) std::sin (2.0 * M_PI * p);
            case Shape::Triangle: return (float) (4.0 * std::fabs (p - 0.5) - 1.0);
            case Shape::Square:   return p < 0.5 ? 1.0f : -1.0f;
            case Shape::Saw:      return (float) (2.0 * p - 1.0);
            case Shape::Random:   return hold;
        }
        return 0.0f;
    }

private:
    double rnd() { state = state * 1103515245u + 12345u; return (double) ((state >> 16) & 0x7FFF) / 32767.0; }
    double sr = 48000.0, phase = 0.0, inc = 0.0;
    Shape shape = Shape::Sine;
    float hold = 0.0f;
    uint32_t state = 22222u;
};

/** Phaser: a cascade of first-order allpass sections with a swept corner.

    Stage count is a real parameter, not decoration: each PAIR of stages puts one
    notch in the response, so a 4-stage phaser has two notches and a 12-stage has
    six. Machines print the stage count, so a unit passes its own. */
class Phaser
{
public:
    static constexpr int kMaxStages = 12;

    void prepare (double sampleRate)
    { sr = sampleRate; lfo.prepare (sampleRate); reset(); }

    void reset() { for (auto& z : zs) z = 0.0f; fbState = 0.0f; }

    void setStages (int n) { stages = std::min (std::max (n & ~1, 2), kMaxStages); }
    void setRate (double hz) { lfo.setRate (hz); }
    void setDepth (float d) { depth = std::min (std::max (d, 0.0f), 1.0f); }
    void setFeedback (float f) { fb = std::min (std::max (f, -0.95f), 0.95f); }
    void setMix (float m) { mix = std::min (std::max (m, 0.0f), 1.0f); }
    /** The sweep runs between these two corner frequencies. */
    void setRange (double loHz, double hiHz) { lo = loHz; hi = hiHz; }

    float process (float x)
    {
        const float m = lfo.tick() * depth;
        // sweep exponentially: a phaser swept linearly in Hz spends most of its
        // time at the top of the range and sounds lopsided
        const double f = lo * std::pow (hi / lo, (double) (0.5f * (m + 1.0f)));
        const double w = std::tan (M_PI * std::min (f, sr * 0.45) / sr);
        const float a = (float) ((w - 1.0) / (w + 1.0));   // allpass coefficient

        float v = x + fbState * fb;
        for (int i = 0; i < stages; ++i)
        {
            const float y = a * v + zs[i];
            zs[i] = v - a * y;
            v = y;
        }
        fbState = v;
        return x * (1.0f - mix) + v * mix;
    }

private:
    LFO lfo;
    std::array<float, kMaxStages> zs {};
    double sr = 48000.0, lo = 200.0, hi = 2000.0;
    int stages = 4;
    float depth = 1.0f, fb = 0.0f, mix = 0.5f, fbState = 0.0f;
};

/** Flanger: a short swept delay summed with the dry signal, with feedback.

    The delay range is what makes this a flanger and not a chorus, so it is
    clamped to the flanging window rather than left open. Negative feedback is
    allowed and is a real setting: it puts the notches where the peaks were. */
class Flanger
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        line.prepare (sampleRate, 0.02);      // 20 ms is beyond any flanger
        lfo.prepare (sampleRate);
        reset();
    }
    void reset() { line.reset(); lp = 0.0f; }

    void setRate (double hz) { lfo.setRate (hz); }
    void setShape (LFO::Shape s) { lfo.setShape (s); }
    /** Sweep bounds in milliseconds, clamped to 0.1..10 ms. */
    void setDelayRange (double loMs, double hiMs)
    {
        loS = std::max (0.1, loMs) * 0.001 * sr;
        hiS = std::min (10.0, std::max (loMs, hiMs)) * 0.001 * sr;
    }
    void setDepth (float d) { depth = std::min (std::max (d, 0.0f), 1.0f); }
    void setFeedback (float f) { fb = std::min (std::max (f, -0.95f), 0.95f); }
    void setMix (float m) { mix = std::min (std::max (m, 0.0f), 1.0f); }
    void setDamping (double hz)
    { hcA = (hz > 0 && hz < sr * 0.5) ? (float) (1.0 - std::exp (-2.0 * M_PI * hz / sr)) : 0.0f; }

    float process (float x)
    {
        const float m = 0.5f * (lfo.tick() * depth + 1.0f);
        const double d = loS + (hiS - loS) * m;
        const float wet = line.readCubic (d);
        float f = wet;
        if (hcA > 0.0f) { lp += hcA * (f - lp); f = lp; }
        line.push (x + f * fb);
        return x * (1.0f - mix) + wet * mix;
    }

private:
    DelayLine line; LFO lfo;
    double sr = 48000.0, loS = 48.0, hiS = 240.0;
    float depth = 1.0f, fb = 0.0f, mix = 0.5f, lp = 0.0f, hcA = 0.0f;
};

/** Chorus: several gently modulated taps, spread in LFO phase.

    Voices are spread evenly around the LFO cycle rather than given independent
    oscillators, which is what keeps the ensemble coherent instead of turning
    into a wash. Feedback is deliberately absent: a chorus with feedback is a
    flanger. */
class Chorus
{
public:
    static constexpr int kMaxVoices = 4;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        line.prepare (sampleRate, 0.08);
        for (int i = 0; i < kMaxVoices; ++i) { lfos[i].prepare (sampleRate); }
        setVoices (voices);
        reset();
    }
    void reset() { line.reset(); }

    void setVoices (int n)
    {
        voices = std::min (std::max (n, 1), kMaxVoices);
        for (int i = 0; i < voices; ++i) lfos[i].setPhase ((double) i / voices);
    }
    void setRate (double hz) { for (auto& l : lfos) l.setRate (hz); }
    /** Centre delay and sweep width, both in milliseconds. Chorus territory is
        10-40 ms centre; shorter than that and it flanges. */
    void setDelay (double centreMs, double widthMs)
    { centre = centreMs * 0.001 * sr; width = widthMs * 0.001 * sr; }
    void setMix (float m) { mix = std::min (std::max (m, 0.0f), 1.0f); }
    /** Linear interpolation, for machines documented as using it. The Supernova's
        chorus is a 64-sample buffer with an integer index and a fraction, and
        interpolating better changes its character. */
    void setLinearInterpolation (bool b) { linear = b; }

    void process (float x, float& outL, float& outR)
    {
        float sum = 0.0f, diff = 0.0f;
        for (int i = 0; i < voices; ++i)
        {
            const double d = centre + width * lfos[i].tick();
            const float v = linear ? line.readLinear (d) : line.readCubic (d);
            sum += v;
            diff += (i & 1) ? -v : v;         // alternate voices to opposite sides
        }
        const float n = 1.0f / (float) voices;
        line.push (x);
        const float wetL = (sum + diff) * 0.5f * n;
        const float wetR = (sum - diff) * 0.5f * n;
        outL = x * (1.0f - mix) + wetL * mix;
        outR = x * (1.0f - mix) + wetR * mix;
    }

private:
    DelayLine line;
    std::array<LFO, kMaxVoices> lfos;
    double sr = 48000.0, centre = 960.0, width = 240.0;
    int voices = 2;
    float mix = 0.5f;
    bool linear = false;
};

/** Tremolo: amplitude only, with an optional stereo phase offset that turns it
    into auto-pan. Depth 1 reaches silence at the bottom of the cycle. */
class Tremolo
{
public:
    void prepare (double sampleRate) { lfo.prepare (sampleRate); }
    void setRate (double hz) { lfo.setRate (hz); }
    void setShape (LFO::Shape s) { lfo.setShape (s); }
    void setDepth (float d) { depth = std::min (std::max (d, 0.0f), 1.0f); }
    /** 0 = both channels together, 0.5 = auto-pan. */
    void setStereoOffset (double turns) { offset = turns; }

    void process (float x, float& outL, float& outR)
    {
        const float a = lfo.tick();
        const float b = (float) std::sin (2.0 * M_PI * (0.25 + offset) + std::asin (std::min (std::max (a, -1.0f), 1.0f)));
        outL = x * (1.0f - depth * 0.5f * (1.0f - a));
        outR = x * (1.0f - depth * 0.5f * (1.0f - (offset > 0.0 ? b : a)));
    }

private:
    LFO lfo;
    float depth = 0.5f;
    double offset = 0.0;
};


/** Rotary speaker: two rotors at different rates, each with its own Doppler
    (a swept delay) and amplitude modulation, plus the crossover that sends bass
    to the drum and treble to the horn.

    The Doppler is the part usually left out, and it is the part that makes a
    Leslie a Leslie rather than a tremolo: the horn is physically moving toward
    and away from the microphone, so the pitch shifts as well as the level, and
    the two are locked in quadrature because the fastest approach happens where
    the horn is side-on.

    Ramping is real too. A Leslie takes a second or two to spin up and longer to
    spin down, and a machine offering a Slow/Fast switch is offering that ramp. */
class Rotary
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        hornLine.prepare (sampleRate, 0.01);
        drumLine.prepare (sampleRate, 0.01);
        split.prepare (sampleRate);
        split.setCutoff (800.0);
        hornPhase = drumPhase = 0.0;
        hornRate = slowHorn; drumRate = slowDrum;
    }
    void reset() { hornLine.reset(); drumLine.reset(); split.reset(); }

    void setFast (bool f) { fast = f; }
    void setSpeeds (double slowHornHz, double fastHornHz, double slowDrumHz, double fastDrumHz)
    { slowHorn = slowHornHz; fastHorn = fastHornHz; slowDrum = slowDrumHz; fastDrum = fastDrumHz; }
    /** Seconds to change speed. The horn is light and the drum is heavy, so
        the drum always takes longer. */
    void setRampSeconds (double hornSec, double drumSec)
    { hornRamp = std::max (0.01, hornSec); drumRamp = std::max (0.01, drumSec); }
    void setCrossover (double hz) { split.setCutoff (hz); }
    void setDepth (float d) { depth = std::min (std::max (d, 0.0f), 1.0f); }

    void process (float x, float& outL, float& outR)
    {
        const double tgtH = fast ? fastHorn : slowHorn;
        const double tgtD = fast ? fastDrum : slowDrum;
        hornRate += (tgtH - hornRate) / (hornRamp * sr);
        drumRate += (tgtD - drumRate) / (drumRamp * sr);

        hornPhase += hornRate / sr; if (hornPhase >= 1.0) hornPhase -= 1.0;
        drumPhase += drumRate / sr; if (drumPhase >= 1.0) drumPhase -= 1.0;

        const float low = split.lp (x);
        const float high = x - low;

        // Doppler: delay swept in quadrature with the amplitude, depth scaled
        // by the rotor radius (the horn swings wider than the drum).
        const double hs = std::sin (2.0 * M_PI * hornPhase);
        const double hc = std::cos (2.0 * M_PI * hornPhase);
        const double ds = std::sin (2.0 * M_PI * drumPhase);
        const double dc = std::cos (2.0 * M_PI * drumPhase);

        hornLine.push (high);
        drumLine.push (low);
        const float h = hornLine.readCubic (0.0016 * sr * (1.0 + 0.9 * depth * hc));
        const float d = drumLine.readCubic (0.0010 * sr * (1.0 + 0.5 * depth * dc));

        const float hAmL = 1.0f + depth * 0.7f * (float) hs;
        const float hAmR = 1.0f - depth * 0.7f * (float) hs;
        const float dAmL = 1.0f + depth * 0.3f * (float) ds;
        const float dAmR = 1.0f - depth * 0.3f * (float) ds;

        outL = h * hAmL * 0.5f + d * dAmL * 0.5f;
        outR = h * hAmR * 0.5f + d * dAmR * 0.5f;
    }

private:
    DelayLine hornLine, drumLine;
    OnePoleLite split;
    double sr = 48000.0, hornPhase = 0.0, drumPhase = 0.0;
    double slowHorn = 0.8, fastHorn = 6.8, slowDrum = 0.7, fastDrum = 5.7;
    double hornRate = 0.8, drumRate = 0.7, hornRamp = 1.0, drumRamp = 3.0;
    float depth = 1.0f;
    bool fast = false;
};

/** Ring modulator: multiply by an oscillator. Sum and difference frequencies
    only, no original, which is why it sounds inharmonic on anything but a pure
    tone. `setMix` below 1 leaves some carrier through, which is amplitude
    modulation rather than ring modulation and is the gentler of the two. */
class RingModulator
{
public:
    void prepare (double sampleRate) { osc.prepare (sampleRate); osc.setShape (LFO::Shape::Sine); }
    void setFrequency (double hz) { osc.setRate (hz); }
    void setShape (LFO::Shape s) { osc.setShape (s); }
    void setMix (float m) { mix = std::min (std::max (m, 0.0f), 1.0f); }
    float process (float x)
    {
        const float c = osc.tick();
        return x * (1.0f - mix) + x * c * mix;
    }
private:
    LFO osc;
    float mix = 1.0f;
};

} // namespace dsp
