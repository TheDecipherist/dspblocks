// Envelope following, compression, expansion and ducking.
//
// These are not an afterthought in the machines this library serves. The TC 2290
// is famous for its dynamic delay, where the repeats duck out of the way of the
// playing and come back in the gaps; the PCM 91's Concert Hall algorithm has
// Compress and Expand rows in its own edit matrix; the Eventide gates its
// Reverb Factory tail. A unit that models the parameter and then does nothing
// with it is exactly the failure this library exists to fix.
//
// One decision worth stating: detection DEFAULTS to a RECTIFIED signal with
// separate attack and release, not RMS. These are 1980s and 1990s units with
// analogue or simple digital detectors, and an RMS detector's smoothing is
// audibly different on transients — slower to open, and it misses a stick hit
// that a peak detector catches.
//
// Some machines say otherwise about THEMSELVES and then the machine wins. The
// TC Triple-C's manual: "The TRIPLE·C is basically an RMS based Compressor,
// meaning that it responds to average level of the incoming source material.
// This key turns the TRIPLE·C into a Peak based Compressor." A unit with that
// evidence calls setDetector(Detector::Rms); PEAK REMAINS THE DEFAULT, so
// nothing that was built before this existed changes behaviour.
#pragma once
#include <cmath>
#include <algorithm>

namespace dsp {

/** Peak or RMS follower with independent attack and release times.
    Times are in milliseconds because that is what every machine prints. */
class EnvelopeFollower
{
public:
    /** What the follower follows. Peak is the default and is what every unit
        written before 2026-10-05 gets without asking. */
    enum class Detector { Peak, Rms };

    void prepare (double sampleRate)
    { sr = sampleRate; env = 0.0f; ms = 0.0f; setTimes (5.0, 100.0); }
    void reset() { env = 0.0f; ms = 0.0f; }

    void setTimes (double attackMs, double releaseMs)
    {
        tA = attackMs; tR = releaseMs;
        recompute();
    }

    void setDetector (Detector d) { mode = d; recompute(); }
    Detector detector() const { return mode; }

    float process (float x)
    {
        if (mode == Detector::Rms)
        {
            // The MEAN SQUARE is what is smoothed and the root is taken after,
            // so the coefficients are computed for HALF the requested time:
            // smoothing x^2 with a time constant of T/2 produces an ENVELOPE
            // with a time constant of T, because the square root halves the
            // exponent. Without the halving a machine's printed 300 ms release
            // measures as 600 ms in the audio - the attack and release a panel
            // prints have to mean the same thing in both detector modes, since
            // one pair of knobs serves both.
            const float sq = x * x;
            ms += (sq > ms ? aA : aR) * (sq - ms);
            env = std::sqrt (std::max (0.0f, ms));
            return env;
        }
        const float r = std::fabs (x);
        env += (r > env ? aA : aR) * (r - env);
        return env;
    }
    float value() const { return env; }

private:
    void recompute()
    {
        const double k = mode == Detector::Rms ? 0.5 : 1.0;
        aA = coef (tA * k);
        aR = coef (tR * k);
    }
    float coef (double ms_) const
    {
        if (ms_ <= 0.0) return 1.0f;
        return (float) (1.0 - std::exp (-1.0 / (ms_ * 0.001 * sr)));
    }
    double sr = 48000.0, tA = 5.0, tR = 100.0;
    float env = 0.0f, ms = 0.0f, aA = 0.1f, aR = 0.01f;
    Detector mode = Detector::Peak;
};

/** Feed-forward compressor with a soft knee, working in dB.

    Ratio is the ratio the panel prints: 4 means 4:1. Ratio 1 is a bypass and
    costs only the detector, so a unit can leave it in the chain unconditionally
    and let the routing data decide whether it does anything. */
class Compressor
{
public:
    void prepare (double sampleRate) { follower.prepare (sampleRate); gainDb = 0.0f; }
    void reset() { follower.reset(); gainDb = 0.0f; }

    void setThresholdDb (float db) { thrDb = db; }
    void setRatio (float r) { ratio = std::max (1.0f, r); }
    void setKneeDb (float db) { knee = std::max (0.0f, db); }
    void setTimes (double attackMs, double releaseMs) { follower.setTimes (attackMs, releaseMs); }
    /** Peak (the default) or RMS. See EnvelopeFollower::Detector. */
    void setDetector (EnvelopeFollower::Detector d) { follower.setDetector (d); }
    void setMakeupDb (float db) { makeup = std::pow (10.0f, db / 20.0f); }

    /** `key` is the detector input, which is normally the signal itself but is
        a separate send when the unit is ducking one thing with another. */
    float process (float x, float key)
    {
        const float e = follower.process (key);
        const float inDb = 20.0f * std::log10 (std::max (e, 1e-7f));
        const float over = inDb - thrDb;
        float reduce = 0.0f;
        if (knee > 0.0f && over > -knee * 0.5f && over < knee * 0.5f)
        {
            const float t = over + knee * 0.5f;
            reduce = (1.0f - 1.0f / ratio) * t * t / (2.0f * knee);
        }
        else if (over > 0.0f)
        {
            reduce = over * (1.0f - 1.0f / ratio);
        }
        gainDb = -reduce;
        return x * std::pow (10.0f, gainDb / 20.0f) * makeup;
    }
    float process (float x) { return process (x, x); }

    /** Gain reduction currently applied, in dB (negative). */
    float reductionDb() const { return gainDb; }

private:
    EnvelopeFollower follower;
    float thrDb = -20.0f, ratio = 4.0f, knee = 6.0f, makeup = 1.0f, gainDb = 0.0f;
};

/** Downward expander and gate. Ratio 1 is a bypass; a high ratio with a fast
    release is a gate, which is why they are one block rather than two. */
class Expander
{
public:
    void prepare (double sampleRate) { follower.prepare (sampleRate); follower.setTimes (1.0, 80.0); }
    void reset() { follower.reset(); }

    void setThresholdDb (float db) { thrDb = db; }
    void setRatio (float r) { ratio = std::max (1.0f, r); }
    void setTimes (double attackMs, double releaseMs) { follower.setTimes (attackMs, releaseMs); }
    /** How far the gate is allowed to close. -80 dB is shut; -12 dB is a duck. */
    void setRangeDb (float db) { rangeDb = std::min (0.0f, db); }

    float process (float x, float key)
    {
        const float e = follower.process (key);
        const float inDb = 20.0f * std::log10 (std::max (e, 1e-7f));
        const float under = thrDb - inDb;
        float cut = 0.0f;
        if (under > 0.0f) cut = std::min (under * (ratio - 1.0f), -rangeDb);
        gainDb = -cut;
        return x * std::pow (10.0f, gainDb / 20.0f);
    }
    float process (float x) { return process (x, x); }
    float reductionDb() const { return gainDb; }

private:
    EnvelopeFollower follower;
    float thrDb = -40.0f, ratio = 2.0f, rangeDb = -60.0f, gainDb = 0.0f;
};

/** The TC 2290's dynamic delay, as its own block because it is not quite a
    compressor: the KEY is the dry input and the THING BEING DUCKED is the wet
    repeats, so the two never share a detector, and the amount is a depth in dB
    rather than a ratio.

    "Dynamic" on that machine means the repeats get out of the way while you
    play and come back when you stop. Set `setDepthDb(-12)` and the repeats sit
    12 dB down under a signal above the threshold. */
class Ducker
{
public:
    void prepare (double sampleRate) { follower.prepare (sampleRate); follower.setTimes (2.0, 200.0); }
    void reset() { follower.reset(); }

    void setThresholdDb (float db) { thrDb = db; }
    /** Negative: how far the ducked signal falls at full key level. */
    void setDepthDb (float db) { depthDb = std::min (0.0f, db); }
    void setTimes (double attackMs, double releaseMs) { follower.setTimes (attackMs, releaseMs); }

    float process (float wet, float key)
    {
        const float e = follower.process (key);
        const float keyDb = 20.0f * std::log10 (std::max (e, 1e-7f));
        // 0 at the threshold, full depth 20 dB above it
        const float t = std::min (1.0f, std::max (0.0f, (keyDb - thrDb) / 20.0f));
        gainDb = depthDb * t;
        return wet * std::pow (10.0f, gainDb / 20.0f);
    }
    float reductionDb() const { return gainDb; }

private:
    EnvelopeFollower follower;
    float thrDb = -30.0f, depthDb = -12.0f, gainDb = 0.0f;
};

} // namespace dsp
