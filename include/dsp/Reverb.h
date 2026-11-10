// The reverb tank: allpass diffusers into a feedback loop, with the tap set
// supplied as DATA so a plate and a hall are different topologies rather than
// the same topology with different numbers on the front.
//
// Two things in here are worth stating because both have already cost real time.
//
// 1. An allpass inside a feedback loop does not contribute its own delay D to
//    the loop's slowest mode. It is lossless on the unit circle but has |H| > 1
//    inside it, so it pushes the poles outward. Evaluate
//    H(z) = (z^-D - g)/(1 - g z^-D) at z = 1 - e for small e and large D:
//
//        |H| ~ 1 + e*D*(1+g)/(1-g)
//
//    which is what a pure delay of D*(1+g)/(1-g) would give. So the decay law
//    has to use the EFFECTIVE loop time or short reverb times become
//    unreachable and it looks like a floor at a second or so. That floor is a
//    real bug that was diagnosed twice before the algebra was done once.
//
//    A refinement that this tank does NOT take, on purpose. D*(1+g)/(1-g) is the
//    group delay at DC, and the tail is really governed by the mode-weighted
//    mean D*(1+g^2)/(1-g^2), which is smaller. Where the allpasses ARE the loop
//    the difference is a measured 30 % and the DC form is wrong - see the header
//    of `ReverbFigure8.h`, which derives it and shows the numbers. Here the
//    allpasses are a few hundred samples inside a five-thousand-sample loop, the
//    two forms are close, and the DC form MEASURES right: -6 % to -2 % on
//    hall/plate/room at 2 and 5 seconds. Swapping it would change the tail of
//    four shipping units for no measured gain, so it stays until someone A/Bs it
//    deliberately.
//
// 2. Everything here is Schroeder/Gardner: allpass diffusers and delays, no
//    parallel comb bank. A Freeverb-style comb reverb has a recognisable
//    metallic ring that none of these machines has.
//
// The tap sets below are ours, chosen by ear against the character each machine
// is documented as having, and every one of them is [EAR] in the unit that uses
// it. What a Lexicon or an Eventide actually spends its delay memory on is not
// in any image that has been read.
#pragma once
#include "Delay.h"
#include "Filters.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace dsp {

/** The tap set. Times are in SAMPLES AT 48 kHz and are rescaled to the host
    rate in prepare(), so a unit's numbers mean the same thing everywhere.

    `inDiffuser` are the input allpasses (density before the loop), `tank` the
    allpasses inside the loop (density of the tail), `loop` the two pure delays
    of the figure-of-eight. Counts may be lower than the array size; set them. */
struct ReverbTopology
{
    int   numInDiffusers = 4;
    int   numTankAllpass = 4;
    double inDiffuser[6] = { 229, 613, 173, 449, 0, 0 };
    double tank[6]       = { 149, 197, 271, 173, 0, 0 };
    double loop[2]       = { 1009, 743 };
    /** Multiplies every tap. A plate is a small dense space, a hall a large
        sparse one, and this is the single number that says which. */
    double scale = 1.0;
    /** Extra allpass gain on the input diffusers, added to the Diffusion law.
        A plate diffuses hard at every setting; an ambience barely diffuses. */
    double diffusionBias = 0.0;
    /** How much of the loop is fed back across to the other side. 0 is two
        independent mono tanks, 1 is fully cross-coupled. */
    double crossCoupling = 1.0;
};

/** Tap sets for the room types these machines print on their front panels.
    Primes throughout, so the diffusers do not ring together. */
namespace topologies {

inline ReverbTopology hall()
{
    ReverbTopology t;
    t.numInDiffusers = 4; t.numTankAllpass = 4;
    t.inDiffuser[0] = 229;  t.inDiffuser[1] = 613;  t.inDiffuser[2] = 173;  t.inDiffuser[3] = 449;
    t.tank[0] = 149; t.tank[1] = 197; t.tank[2] = 271; t.tank[3] = 173;
    t.loop[0] = 1009; t.loop[1] = 743;
    t.scale = 1.0; t.diffusionBias = 0.0; t.crossCoupling = 1.0;
    return t;
}

/** A plate is a sheet, not a room: short taps, very high diffusion, dense from
    the first millisecond, and no meaningful early-reflection structure. */
inline ReverbTopology plate()
{
    ReverbTopology t;
    t.numInDiffusers = 4; t.numTankAllpass = 4;
    t.inDiffuser[0] = 113; t.inDiffuser[1] = 157; t.inDiffuser[2] = 89;  t.inDiffuser[3] = 191;
    t.tank[0] = 83; t.tank[1] = 107; t.tank[2] = 127; t.tank[3] = 97;
    t.loop[0] = 487; t.loop[1] = 383;
    t.scale = 0.85; t.diffusionBias = 0.18; t.crossCoupling = 1.0;
    return t;
}

/** A room is small, and its identity is in the early pattern rather than the
    tail, so the loop is short and the diffusion moderate. */
inline ReverbTopology room()
{
    ReverbTopology t;
    t.numInDiffusers = 3; t.numTankAllpass = 4;
    t.inDiffuser[0] = 139; t.inDiffuser[1] = 211; t.inDiffuser[2] = 331; t.inDiffuser[3] = 0;
    t.tank[0] = 109; t.tank[1] = 151; t.tank[2] = 193; t.tank[3] = 131;
    t.loop[0] = 641; t.loop[1] = 499;
    t.scale = 0.7; t.diffusionBias = -0.05; t.crossCoupling = 0.8;
    return t;
}

/** A chamber is even and dimensionless: high diffusion like a plate, but a
    longer loop so it does not colour the way a plate does. */
inline ReverbTopology chamber()
{
    ReverbTopology t;
    t.numInDiffusers = 4; t.numTankAllpass = 4;
    t.inDiffuser[0] = 181; t.inDiffuser[1] = 293; t.inDiffuser[2] = 137; t.inDiffuser[3] = 359;
    t.tank[0] = 127; t.tank[1] = 179; t.tank[2] = 233; t.tank[3] = 149;
    t.loop[0] = 811; t.loop[1] = 619;
    t.scale = 0.9; t.diffusionBias = 0.1; t.crossCoupling = 1.0;
    return t;
}

/** Ambience is almost all early pattern and almost no tail: barely any
    diffusion, a short loop, and the unit is expected to run it at a low decay. */
inline ReverbTopology ambience()
{
    ReverbTopology t;
    t.numInDiffusers = 2; t.numTankAllpass = 2;
    t.inDiffuser[0] = 67; t.inDiffuser[1] = 103;
    t.tank[0] = 71; t.tank[1] = 113;
    t.loop[0] = 293; t.loop[1] = 227;
    t.scale = 0.55; t.diffusionBias = -0.25; t.crossCoupling = 0.5;
    return t;
}

/** A cathedral-scale space: long taps, moderate diffusion, made to be run with
    a long decay and a dark damping filter. */
inline ReverbTopology cathedral()
{
    ReverbTopology t = hall();
    t.inDiffuser[0] = 397; t.inDiffuser[1] = 887; t.inDiffuser[2] = 293; t.inDiffuser[3] = 683;
    t.tank[0] = 251; t.tank[1] = 337; t.tank[2] = 439; t.tank[3] = 283;
    t.loop[0] = 1733; t.loop[1] = 1279;
    t.scale = 1.6;
    return t;
}

} // namespace topologies

/** The envelope that turns a reverb into a gate or an inverse.

    This is the Lexicon "Inverse" algorithm and the Eventide "Reverb Factory"
    gate: the tail is multiplied by a slope over a fixed time and then cut. A
    positive slope rises into the cut, which is the backwards-reverb sound; a
    flat slope is a gate; a negative slope is an ordinary decay.

    It is retriggered by the input rather than free-running, because that is what
    makes it follow playing rather than chop it. */
class ReverbEnvelope
{
public:
    void prepare (double sampleRate) { sr = sampleRate; reset(); }
    void reset() { t = 1e9; env = 0.0f; armed = true; }

    /** -1 falls, 0 holds level, +1 rises. */
    void setSlope (float s) { slope = std::min (std::max (s, -1.0f), 1.0f); }
    /** How long the shaped section lasts before the cut, in seconds. */
    void setDuration (double seconds) { dur = std::max (0.01, seconds); }
    /** Level the input must reach to retrigger. 0 disables retriggering and the
        envelope simply runs. */
    void setThreshold (float th) { thresh = th; }
    /** Off by default: a hall does not want an envelope at all. */
    void setEnabled (bool b) { enabled = b; }
    bool isEnabled() const { return enabled; }

    /** How far through the shaped section the envelope is, 0 at the trigger and
        1 at the cut; -1 when it has already been cut or has never fired.

        Added for the Roland DEP-5, whose non-linear OUTPUT setting pans the
        gated burst from one output to the other ACROSS the gate window. That pan
        needs the envelope's position, and a unit that counts its own samples
        alongside this one has two triggers that can drift apart - which is the
        bug the accessor exists to make impossible. It reads state that already
        exists and changes nothing. */
    double phase() const
    {
        if (! enabled) return -1.0;
        const double n = t / (dur * sr);
        return n >= 1.0 ? -1.0 : n;
    }

    float process (float trigger)
    {
        if (! enabled) return 1.0f;
        if (thresh > 0.0f && std::fabs (trigger) > thresh && armed) { t = 0.0; armed = false; }
        if (thresh > 0.0f && std::fabs (trigger) < thresh * 0.25f) armed = true;
        const double n = t / (dur * sr);
        t += 1.0;
        if (n >= 1.0) { env = 0.0f; return 0.0f; }
        // slope +1: 0 -> 1 across the duration, then cut. slope -1: 1 -> 0.
        const float shaped = slope >= 0.0f
            ? (float) (slope * n + (1.0 - slope) * 1.0)
            : (float) (1.0 + slope * n);
        env = std::max (0.0f, shaped);
        return env;
    }

private:
    double sr = 48000.0, t = 1e9, dur = 0.3;
    float slope = -1.0f, thresh = 0.0f, env = 0.0f;
    bool enabled = false, armed = true;
};

/** A tapped early-reflection pattern.

    Kept separate from the tank because the two are separately audible: the
    early pattern says how big and how hard-walled the space is, and the tail
    says how long it rings. A machine that offers RefDly/RefLvl per side is
    offering exactly this, and a machine that offers none (a plate) should not
    be given one. */
class EarlyReflections
{
public:
    /** 40, not 12 and no longer 24. The machines whose early pattern IS
        recovered data hold more than twelve taps: the Reverb 4000's own table is
        21, in three rings of seven. A block that could not hold the pattern
        would force the unit to thin it, which is a decision about the sound made
        by an array bound - and that is exactly what 24 was doing to the 40-tap
        scatter supersynth's reverb runs (see `EarlyPatterns.h`), where the
        sixteen taps that fell off the end were a quarter of the pattern's
        energy. Raising the bound costs 128 bytes per player and changes no
        behaviour: only `n` is ever walked. */
    static constexpr int kMaxTaps = 40;

    void prepare (double sampleRate, double maxSeconds = 0.5)
    { sr = sampleRate; line.prepare (sampleRate, maxSeconds); n = 0; }
    void reset() { line.reset(); }

    void clear() { n = 0; }
    void addTap (double ms, float levelL, float levelR)
    {
        if (n >= kMaxTaps) return;
        taps[n++] = { ms * 0.001 * sr, levelL, levelR };
    }
    /** Scale every tap's time, so Size moves the whole pattern together. */
    void setSizeScale (double s) { sizeScale = std::max (0.05, s); }

    void process (float x, float& outL, float& outR)
    {
        line.push (x);
        outL = outR = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float v = line.readLinear (taps[i].samples * sizeScale);
            outL += v * taps[i].l;
            outR += v * taps[i].r;
        }
    }

private:
    struct Tap { double samples; float l, r; };
    DelayLine line;
    Tap taps[kMaxTaps] {};
    int n = 0;
    double sr = 48000.0, sizeScale = 1.0;
};

/** The tank.

    Driven by a topology and by the controls these machines actually print:
    decay time in seconds, size, diffusion, a crossover with a separate low
    decay, a damping filter inside the loop and a high cut outside it, plus the
    random modulation ("spin" rate, "wander" depth) that is the reason a Lexicon
    tail does not sound like a static comb. */
class ReverbTank
{
public:
    void prepare (double sampleRate, const ReverbTopology& topo)
    {
        sr = sampleRate > 0 ? sampleRate : 48000.0;
        top = topo;
        const double k = sr / 48000.0;
        // Headroom for the largest Size (4x) and the deepest Wander.
        const int wanderRoom = (int) (sr * 0.06);
        for (int i = 0; i < top.numInDiffusers; ++i)
        {
            inS[i] = top.inDiffuser[i] * k * top.scale;
            inAp[i].prepare (sr, (inS[i] * 4.5 + 64.0) / sr);
        }
        for (int i = 0; i < top.numTankAllpass; ++i)
        {
            tankS[i] = top.tank[i] * k * top.scale;
            tankAp[i].prepare (sr, (tankS[i] * 4.5 + wanderRoom + 64.0) / sr);
        }
        for (int i = 0; i < 2; ++i)
        {
            loopS[i] = top.loop[i] * k * top.scale;
            loopLine[i].prepare (sr, (loopS[i] * 4.5 + 64.0) / sr);
        }
        for (auto& d : damp) d.prepare (sr);
        for (auto& s : lowSplit) s.prepare (sr);
        for (auto& s : highSplit) s.prepare (sr);
        outLp[0].prepare (sr); outLp[1].prepare (sr);
        env.prepare (sr);
        reset();
        dirty = true;
    }

    void reset()
    {
        for (auto& a : inAp) a.reset();
        for (auto& a : tankAp) a.reset();
        for (auto& l : loopLine) l.reset();
        for (auto& d : damp) d.reset();
        for (auto& s : lowSplit) s.reset();
        for (auto& s : highSplit) s.reset();
        outLp[0].reset(); outLp[1].reset();
        env.reset();
        for (int i = 0; i < 6; ++i) rnd[i] = 0.0f, lfoPhase[i] = 0.25f * i;
        // The random walk's seed is STATE, so a reset has to reset it. Leaving
        // it wherever the last run stopped made two identical renders come out
        // different, which silently poisons every before/after measurement: a
        // probe that isolates a voice by rendering twice and subtracting keeps
        // the tank's own noise in the difference. It never announces itself -
        // the reverb sounds fine and the numbers still look like numbers.
        seed = kInitialSeed;
    }

    void setDecaySeconds (double s)      { rt = std::max (0.05, s); dirty = true; }
    void setLowDecayMultiplier (double m){ lowMul = std::max (0.05, m); dirty = true; }
    void setCrossover (double hz)        { crossoverHz = hz; dirty = true; }
    /** The HIGH band's decay, as a multiple of the main decay, mirroring
        setLowDecayMultiplier. Added for the Sony DRE-2000 (1981), whose remote
        prints RT LOW FREQ RANGE and RT HIGH FREQ RANGE as x0.5..x2.0 and
        x0.25..x1.0 MULTIPLIERS on one reverb time rather than as three separate
        times - so a unit that models it as three times is modelling a different
        machine. Nothing else was reachable: setDamping() is a filter INSIDE the
        loop and darkens the tail progressively, which is not a band with its own
        T60.

        DEFAULT 1.0, AND THAT IS LOAD-BEARING. At exactly 1.0 the extra band
        split is skipped and the sample path is the one every shipping unit
        already measures against - not "close to", the same code. Only a caller
        that asks for a high multiplier pays for it. */
    void setHighDecayMultiplier (double m){ highMul = std::max (0.05, m); dirty = true; }
    /** Where the high band starts. Only read when the high multiplier is not 1. */
    void setHighCrossover (double hz)    { highCrossHz = hz; dirty = true; }
    void setDamping (double hz)          { dampHz = hz; dirty = true; }   // Rt HC: inside the loop
    void setHighCut (double hz)          { highCutHz = hz; dirty = true; } // outside it
    void setSize (double s)              { size = std::min (std::max (s, 0.1), 4.0); dirty = true; }
    void setDiffusion (double d)         { diff = std::min (std::max (d, 0.0), 1.0); dirty = true; }
    /** Rate of the random walk, Hz. */
    void setSpin (double hz)             { spinHz = std::max (0.0, hz); }
    /** Depth of the random walk, milliseconds. */
    void setWander (double ms)           { wanderMs = std::max (0.0, ms); dirty = true; }
    void setInfinite (bool b)            { infinite = b; dirty = true; }

    ReverbEnvelope& envelope() { return env; }

    /** The effective loop time in seconds, allpass stretch included. Exposed
        because it is the number a unit needs to sanity-check its decay law. */
    double loopSeconds() const { return loopTime; }

    void process (float inL, float inR, float& outL, float& outR)
    {
        if (dirty) rebuild();
        const float mono = 0.5f * (inL + inR);

        float v = mono;
        for (int i = 0; i < top.numInDiffusers; ++i) v = inAp[i].process (v, inS[i] * size);

        // random walk, one per tank allpass
        for (int i = 0; i < top.numTankAllpass; ++i)
        {
            lfoPhase[i] += spinInc;
            if (lfoPhase[i] >= 1.0f)
            {
                lfoPhase[i] -= 1.0f;
                seed = seed * 1664525u + 1013904223u;
                rnd[i] = (float) ((seed >> 9) & 0xFFFF) / 32768.0f - 1.0f;
            }
        }

        const float xc = (float) top.crossCoupling;
        const int half = std::max (1, top.numTankAllpass / 2);

        // Cross-coupling decides WHERE each side's feedback comes from, not how
        // much of it there is.
        //
        // Scaling only the cross path threw away (1 - crossCoupling) of the
        // loop gain every trip, and the decay law knows nothing about that
        // loss. The `room` tap set at crossCoupling 0.8 decayed in 0.43 of the
        // time it was asked for at 1 s, 0.27 at 2 s and 0.16 at 4 s: the error
        // grows with the requested time, because a fixed extra loss per trip
        // costs proportionally more the longer the tail is meant to be.
        // Reading both lines at the SAME delay and blending keeps the loop gain
        // at 1 whatever the coupling, and the decay law stays exact.
        const double dA = loopS[1] * size, dB = loopS[0] * size;
        const float fbA = loopLine[1].readCubic (dA) * xc + loopLine[0].readCubic (dA) * (1.0f - xc);
        const float fbB = loopLine[0].readCubic (dB) * xc + loopLine[1].readCubic (dB) * (1.0f - xc);

        float a = v + fbA;
        for (int i = 0; i < half; ++i) a = tankAp[i].process (a, tankS[i] * size + rnd[i] * wanderS);
        a = damp[0].lp (a);
        a = splitBands (a, 0);
        a = hardClipf (a, 8.0f);
        loopLine[0].push (std::isfinite (a) ? a : 0.0f);

        float b = v + fbB;
        for (int i = half; i < top.numTankAllpass; ++i) b = tankAp[i].process (b, tankS[i] * size + rnd[i] * wanderS);
        b = damp[1].lp (b);
        b = splitBands (b, 1);
        b = hardClipf (b, 8.0f);
        loopLine[1].push (std::isfinite (b) ? b : 0.0f);

        float wl = loopLine[0].readCubic (loopS[0] * 0.61 * size) + loopLine[1].readCubic (loopS[1] * 0.37 * size);
        float wr = loopLine[1].readCubic (loopS[1] * 0.59 * size) + loopLine[0].readCubic (loopS[0] * 0.41 * size);
        wl = outLp[0].lp (wl); wr = outLp[1].lp (wr);

        const float e = env.process (mono);
        outL = wl * e; outR = wr * e;
    }

private:
    static float hardClipf (float x, float c) { return x < -c ? -c : (x > c ? c : x); }

    /** One trip's loss, per band. Two bands by default and three only when a
        high multiplier has been asked for, so the two-band path stays the exact
        arithmetic every shipping unit was measured with. */
    float splitBands (float v, int side) noexcept
    {
        const float low  = lowSplit[side].lp (v);
        float       rest = v - low;
        if (useHighBand)
        {
            const float mid = highSplit[side].lp (rest);
            rest = mid * loopFb + (rest - mid) * highFb;
        }
        else
            rest *= loopFb;
        return low * lowFb + rest;
    }

    void rebuild()
    {
        dirty = false;
        const float inG   = (float) std::min (0.85, std::max (0.05, 0.25 + 0.5 * diff + top.diffusionBias));
        const float tankG = (float) std::min (0.78, std::max (0.05, 0.35 + 0.35 * diff + top.diffusionBias * 0.5));
        for (int i = 0; i < top.numInDiffusers; ++i) inAp[i].setGain (inG);
        for (int i = 0; i < top.numTankAllpass; ++i) tankAp[i].setGain (tankG);

        // Effective loop time: pure delays plus the allpasses at their stretch.
        const double stretch = (1.0 + tankG) / std::max (0.05, 1.0 - (double) tankG);
        double sum = (loopS[0] + loopS[1]) * size;
        for (int i = 0; i < top.numTankAllpass; ++i) sum += tankS[i] * size * stretch;
        loopTime = sum / sr;

        auto gainFor = [this] (double seconds) -> float
        {
            if (infinite) return 0.9995f;
            const double need = 60.0 * loopTime / std::max (0.05, seconds);
            return (float) std::min (0.9995, std::max (0.0, std::pow (10.0, -need / 40.0)));
        };
        loopFb = gainFor (rt);
        lowFb  = gainFor (rt * lowMul);
        highFb = gainFor (rt * highMul);
        useHighBand = std::abs (highMul - 1.0) > 1.0e-9;

        for (auto& d : damp) d.setCutoff (dampHz);
        for (auto& s : lowSplit) s.setCutoff (crossoverHz);
        for (auto& s : highSplit) s.setCutoff (highCrossHz);
        outLp[0].setCutoff (highCutHz); outLp[1].setCutoff (highCutHz);

        spinInc = (float) (spinHz / sr);

        // Cap the wander so a modulated tap can never index outside the shortest
        // tank line; without this a deep Wander at a small Size wraps the tank
        // onto itself and the tail turns to noise.
        double headroom = 1e9;
        for (int i = 0; i < top.numTankAllpass; ++i)
        {
            const double base = tankS[i] * size;
            headroom = std::min (headroom, base - 2.0);
        }
        wanderS = (float) std::max (0.0, std::min (wanderMs * 0.001 * sr * 0.5, headroom));
    }

    ReverbTopology top;
    AllpassFilter inAp[6], tankAp[6];
    DelayLine loopLine[2];
    OnePole damp[2], lowSplit[2], highSplit[2], outLp[2];
    ReverbEnvelope env;

    double sr = 48000.0;
    double inS[6] {}, tankS[6] {}, loopS[2] {};
    double rt = 2.0, lowMul = 1.0, crossoverHz = 500.0, dampHz = 6000.0, highCutHz = 9000.0;
    double highMul = 1.0, highCrossHz = 6000.0;
    double size = 1.0, diff = 0.7, spinHz = 0.9, wanderMs = 12.0, loopTime = 0.05;
    float loopFb = 0.5f, lowFb = 0.5f, highFb = 0.5f, wanderS = 0.0f, spinInc = 0.0f;
    bool  useHighBand = false;
    float rnd[6] {}, lfoPhase[6] {};
    static constexpr uint32_t kInitialSeed = 0x1234567u;
    uint32_t seed = kInitialSeed;
    bool infinite = false, dirty = true;
};

} // namespace dsp
