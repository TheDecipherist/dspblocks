// The SECOND reverb topology: a figure-of-eight tank.
//
// `Reverb.h` holds the first one - a Schroeder/Gardner chain of input diffusers
// into two cross-coupled loops of allpasses. Four units run it. This is not a
// replacement for it and nothing that includes `Reverb.h` changes because this
// file exists; it is a sibling with a different STRUCTURE, so that a unit can be
// A/B'd against the other one rather than silently migrated.
//
// The structure is Dattorro's, from "Effect Design Part 2: Delay-Line
// Modulation and Reverberation" (JAES 45(10), 1997) [PUB]: a bandwidth filter
// and four input diffusers feed a loop that runs figure-of-eight through two
// halves, each half being
//
//     -> [modulated allpass] -> [delay] -> [damping] -> x decay
//        -> [allpass] -> [delay] -> x decay -> (the other half)
//
// and the output is a fixed set of taps read from six points INSIDE the tank
// rather than from its output, which is what gives it a stereo field without a
// second tank.
//
// ---------------------------------------------------------------------------
// Does this violate "no parallel comb bank"?
//
// No, and it is worth spelling out because the next person will worry about it.
// The rule in `Reverb.h` exists to keep a Freeverb-style reverb out of these
// machines: Freeverb sums EIGHT INDEPENDENT feedback combs in PARALLEL, each an
// undiffused resonator whose modes sit at multiples of sr/D, and the sum of
// eight such regular mode combs is the metallic ring the rule is about.
//
// There is no parallel path anywhere in here. Every element is in SERIES inside
// one loop, four of the six are allpasses (flat magnitude, dispersive phase),
// and the two pure delays never see their own output except after a full circuit
// through four allpasses. A single feedback loop is trivially "a comb" in the
// sense that any delay with feedback is, but its modes are scattered by the
// allpasses in front of them, which is exactly what a comb bank does not do.
// The library's own measurement agrees: `dsp::measure::combSpacingMs` returns 0
// for this tank, the same as for the Schroeder one, and the tests assert it.
//
// Dattorro's design is allpass-based. It is on the right side of the rule.
// ---------------------------------------------------------------------------
//
// Two things carry over from `Reverb.h` unchanged, because they were paid for
// once already:
//
// 1. THE DECAY LAW USES THE EFFECTIVE LOOP TIME, NOT THE NOMINAL ONE. An
//    allpass inside a feedback loop does not contribute merely its own delay D.
//    It is lossless on the unit circle but has |H| > 1 inside it - evaluate
//    H(z) = (z^-D - g)/(1 - g z^-D) at z = 1-e and |H| ~ 1 + e*D*(1+g)/(1-g) -
//    so it pushes the poles outward and the loop takes longer to come round than
//    its delays say. Count only the delays and short reverb times become
//    unreachable: the tail floors out at a second or so. That is a real bug that
//    was diagnosed twice before the algebra was done once, and it bites HARDER
//    here than in the Schroeder tank, where the allpasses are a few hundred
//    samples against a 5000-sample loop. Here they ARE the loop: 9735 samples of
//    allpass against 25084 of pure delay, and at Dattorro's own gains they are
//    worth two to five times that.
//
//    WHICH stretch, though. `Reverb.h` uses D*(1+g)/(1-g), which is the group
//    delay AT DC - the peak of a resonance in the group delay that is only
//    about (1-g)/D radians wide. That peak holds fewer than one pole of the
//    loop: pole spacing is 2*pi/tau, so the pole count under it is (1+g)/(2*pi),
//    around a quarter of a pole. A tail cannot decay at the rate of a mode that
//    is not there. What the tail actually decays at is the mean group delay
//    WEIGHTED BY THE MODES, and since the modes are spaced 2*pi/tau apart the
//    mode-weighted mean of tau is <tau^2>/<tau>. Both moments are standard
//    integrals of 1/(1+g^2-2g*cos u):
//
//        <tau>  = D                        (an allpass has mean group delay D)
//        <tau^2>/<tau> = D*(1+g^2)/(1-g^2)
//
//    which is what this block uses. MEASURED, on the four topologies below,
//    impulse in and RT60 fitted between -20 and -50 dB at 48 kHz:
//
//        law                     RT60 error, 1 s to 10 s
//        nominal delays only     roughly +100 %, and under a second unreachable
//        DC peak (1+g)/(1-g)     -27 % to -37 %, every topology, every time
//        modal (1+g^2)/(1-g^2)   +7 % to -9 %, every topology, every time
//
//    `Reverb.h` keeps the DC form and is right to: its allpasses are a minor
//    part of a loop twenty times shorter, where the two forms differ by little
//    and the DC one measures at -6 % to -2 %. Changing it there would be an
//    unmeasured change to four shipping units, which is a separate job.
//
//    WHAT THE LAW STILL DOES NOT KNOW, measured 2026-09-12 on the plate at
//    48 kHz with the same protocol, averaged over asks of 2, 4 and 8 seconds:
//
//        whole-sample delays, no modulation      0.93 - 0.94 of the ask
//        the loop's fractional reads             0.89 - 0.90
//        plus the modulation below               0.82 - 0.84
//
//    The modulation is the largest single term and the solve above is blind to
//    it: the tank is moving while it decays and `gainFor` prices a static
//    circuit. Flat across Size, flat at 96 kHz. The same interpolation darkens
//    the tail by about 900 Hz of spectral centroid. NOT CORRECTED HERE, because
//    it changes the tail of the seven machines running this tank; the argument
//    and the numbers are in the README, and synthshell's FxReverbBlock carries
//    a local correction with a test that fails the day this is put right.
//
//    What no law can do is decay faster than one circuit. The plate's tank is
//    0.52 s of pure delay, so asking it for RT60 0.3 s at Size 1.0 gets 0.79 s
//    and no arithmetic will fix that - `setSize` is the control that does. At
//    Size 0.35 the `room` set reaches 0.3 s within 14 % and 0.6 s within 2 %.
//
// 2. THE MODULATION IS ALWAYS ON. Not gated on a parameter row existing, not
//    zero by default. A static allpass tank rings, and the machine that gates
//    its tank modulation on whether the front panel prints a MOD row ends up
//    with algorithms that never modulate at all. The default here is Dattorro's
//    own: +-0.27 ms of excursion on the two tank input allpasses, at 0.9 Hz,
//    and it is a CONTINUOUS sine rather than the Schroeder tank's random walk
//    (that walk steps once per LFO cycle, so below about 1 Hz a tap moves once
//    every few seconds and measures as not moving at all).
//
// PROVENANCE. The topology, the four input diffuser lengths, the tank delay
// lengths, the two diffusion gains and the output tap set are Dattorro's
// published plate [PUB]. Everything else - the hall/room/chamber tap sets, the
// diffusion law, the crossover and low-decay split, the size scaling - is [EAR]
// and ours, exactly as in `Reverb.h`. No byte of any machine's control store is
// in here, because no machine's control store has been read.
#pragma once
#include "Reverb.h"          // ReverbEnvelope and EarlyReflections are shared, not duplicated
#include "Delay.h"
#include "Filters.h"
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace dsp {

/** The six places inside the tank an output tap may read from. Naming them is
    what lets the tap set be DATA: Dattorro's stereo field is seven signed taps
    per side scattered over these six lines, and a different room is a different
    scatter, not a different mixer. */
enum class Figure8Node : uint8_t
{
    LeftDelay1 = 0, LeftAllpass2, LeftDelay2,
    RightDelay1,    RightAllpass2, RightDelay2
};

struct Figure8Tap
{
    Figure8Node node = Figure8Node::LeftDelay1;
    double samples = 0.0;    // at the topology's own base rate, before size
    float  gain = 0.0f;      // signed: the polarity is half of what makes it wide
};

/** The tank as data.

    Times are in SAMPLES AT `baseRate`, not at 48 kHz as `ReverbTopology` uses,
    and that is deliberate: Dattorro's numbers are published at 29761 Hz and
    keeping them literal is what lets the next person check them against the
    paper instead of against a conversion. `prepare()` rescales to the host rate,
    so a unit's numbers still mean the same thing everywhere. */
struct Figure8Topology
{
    double baseRate = 29761.0;

    // --- in front of the loop ------------------------------------------------
    int    numInDiffusers = 4;
    double inDiffuser[6]  = { 142, 107, 379, 277, 0, 0 };
    /** Dattorro's two input diffusion gains: the first pair diffuse harder than
        the second. `setDiffusion` moves both together around these. */
    float  inGain1 = 0.75f, inGain2 = 0.625f;

    // --- the loop, per half --------------------------------------------------
    double modAllpass[2] = { 672, 908 };     // the modulated one
    double delay1[2]     = { 4453, 4217 };
    double allpass2[2]   = { 1800, 2656 };
    double delay2[2]     = { 3720, 3163 };
    /** "Decay diffusion 1" and "decay diffusion 2". These two gains are what
        stretch the loop; see the note at the top of the file. */
    float  modAllpassGain = 0.70f;
    float  allpass2Gain   = 0.50f;

    // --- the whole thing -----------------------------------------------------
    /** Multiplies every tap. A plate is a small dense space, a hall a large
        sparse one, and this is the single number that says which. */
    double scale = 1.0;
    /** Added to the diffusion law. A plate diffuses hard at every setting. */
    double diffusionBias = 0.0;
    /** Never zero by default. See the note at the top of the file. */
    double modRateHz  = 0.9;
    double modDepthMs = 0.27;      // Dattorro's +-8 samples at 29761 Hz

    // --- where the stereo field is read from ---------------------------------
    int numOutL = 7, numOutR = 7;
    Figure8Tap outL[8] = {
        { Figure8Node::RightDelay1,    266,  0.6f },
        { Figure8Node::RightDelay1,   2974,  0.6f },
        { Figure8Node::RightAllpass2, 1913, -0.6f },
        { Figure8Node::RightDelay2,   1996,  0.6f },
        { Figure8Node::LeftDelay1,    1066, -0.6f },
        { Figure8Node::LeftAllpass2,   266, -0.6f },
        { Figure8Node::LeftDelay2,    2111, -0.6f },
    };
    Figure8Tap outR[8] = {
        { Figure8Node::LeftDelay1,     353,  0.6f },
        { Figure8Node::LeftDelay1,    3627,  0.6f },
        { Figure8Node::LeftAllpass2,  1228, -0.6f },
        { Figure8Node::LeftDelay2,    2673,  0.6f },
        { Figure8Node::RightDelay1,   2111, -0.6f },
        { Figure8Node::RightAllpass2,  335, -0.6f },
        { Figure8Node::RightDelay2,    121, -0.6f },
    };
};

/** Tap sets for the rooms these machines print. Only the plate is published;
    the other five are ours. */
namespace figure8 {

/** Dattorro's plate, verbatim [PUB]. Every length, both diffusion gains and
    both tap sets are the paper's. Note the SEVENTH tap on each side: the
    implementation this block was extracted from had six, dropping the left
    second delay from the left output and the right second delay from the right,
    which narrows the field slightly. */
inline Figure8Topology plate()
{
    Figure8Topology t;              // the defaults above ARE the plate
    // Bias 0 on purpose: at the default Diffusion of 0.7 the four gains come out
    // 0.75 / 0.75 / 0.625 / 0.625 in front and 0.70 / 0.50 in the loop, which is
    // the paper's plate exactly. A bias here would make the docstring a lie.
    t.scale = 1.0; t.diffusionBias = 0.0;
    return t;
}

/** A hall: the same structure at room scale, sparser diffusion in front of the
    loop and a longer circuit. [EAR] */
inline Figure8Topology hall()
{
    Figure8Topology t = plate();
    t.inDiffuser[0] = 191; t.inDiffuser[1] = 149; t.inDiffuser[2] = 461; t.inDiffuser[3] = 331;
    t.modAllpass[0] = 761;  t.modAllpass[1] = 1013;
    t.delay1[0]     = 5279; t.delay1[1]     = 4993;
    t.allpass2[0]   = 2029; t.allpass2[1]   = 2971;
    t.delay2[0]     = 4409; t.delay2[1]     = 3739;
    t.inGain1 = 0.70f; t.inGain2 = 0.58f;
    t.scale = 1.0; t.diffusionBias = 0.0;
    t.modRateHz = 0.7; t.modDepthMs = 0.35;
    return t;
}

/** A room: short, and its identity is in the first 50 ms rather than the tail,
    so the circuit is short and the diffusion moderate. [EAR] */
inline Figure8Topology room()
{
    Figure8Topology t = plate();
    t.inDiffuser[0] = 113; t.inDiffuser[1] = 89; t.inDiffuser[2] = 269; t.inDiffuser[3] = 197;
    t.modAllpass[0] = 439;  t.modAllpass[1] = 601;
    t.delay1[0]     = 2477; t.delay1[1]     = 2311;
    t.allpass2[0]   = 1123; t.allpass2[1]   = 1583;
    t.delay2[0]     = 2069; t.delay2[1]     = 1747;
    t.inGain1 = 0.66f; t.inGain2 = 0.55f;
    t.scale = 1.0; t.diffusionBias = -0.05;
    t.modRateHz = 1.1; t.modDepthMs = 0.20;
    return t;
}

/** A chamber: even and dimensionless. Plate-grade diffusion, hall-grade
    circuit, so it does not colour the way a plate does. [EAR] */
inline Figure8Topology chamber()
{
    Figure8Topology t = plate();
    t.inDiffuser[0] = 167; t.inDiffuser[1] = 127; t.inDiffuser[2] = 409; t.inDiffuser[3] = 293;
    t.modAllpass[0] = 709;  t.modAllpass[1] = 953;
    t.delay1[0]     = 4703; t.delay1[1]     = 4451;
    t.allpass2[0]   = 1889; t.allpass2[1]   = 2777;
    t.delay2[0]     = 3931; t.delay2[1]     = 3343;
    t.inGain1 = 0.74f; t.inGain2 = 0.61f;
    t.scale = 1.0; t.diffusionBias = 0.06;
    t.modRateHz = 0.8; t.modDepthMs = 0.30;
    return t;
}

/** Ambience: almost all early pattern and almost no tail. The circuit is short
    enough that a room-sized decay is physically reachable without shrinking
    Size, and the diffusion is deliberately low, so what comes out still has the
    grain of individual reflections in it. [EAR] */
inline Figure8Topology ambience()
{
    Figure8Topology t = plate();
    t.inDiffuser[0] = 79;  t.inDiffuser[1] = 61;  t.inDiffuser[2] = 181; t.inDiffuser[3] = 131;
    t.modAllpass[0] = 293;  t.modAllpass[1] = 401;
    t.delay1[0]     = 1471; t.delay1[1]     = 1373;
    t.allpass2[0]   = 743;  t.allpass2[1]   = 1051;
    t.delay2[0]     = 1231; t.delay2[1]     = 1039;
    t.inGain1 = 0.55f; t.inGain2 = 0.45f;
    t.scale = 1.0; t.diffusionBias = -0.20;
    t.modRateHz = 1.3; t.modDepthMs = 0.16;
    return t;
}

/** Cathedral scale: the longest circuit here, moderate diffusion, and slow
    shallow modulation because a space this size is not supposed to shimmer.
    Made to be run with a long decay and a dark damping filter. [EAR] */
inline Figure8Topology cathedral()
{
    Figure8Topology t = hall();
    t.inDiffuser[0] = 251;  t.inDiffuser[1] = 197;  t.inDiffuser[2] = 607; t.inDiffuser[3] = 439;
    // The circuit is the longest of the six, and no longer, because a tank
    // cannot decay faster than one trip round itself: at 1.12 s of delay the
    // shortest reachable RT60 was 1.18 s and the library's own 1.0 s test failed
    // on it. What makes this read as a cathedral rather than a big hall is the
    // sparse input diffusion below, which is what a slow build-up sounds like.
    t.modAllpass[0] = 853;  t.modAllpass[1] = 1129;
    t.delay1[0]     = 5923; t.delay1[1]     = 5591;
    t.allpass2[0]   = 2287; t.allpass2[1]   = 3343;
    t.delay2[0]     = 4943; t.delay2[1]     = 4201;
    t.inGain1 = 0.62f; t.inGain2 = 0.50f;
    t.scale = 1.0; t.diffusionBias = -0.04;
    t.modRateHz = 0.5; t.modDepthMs = 0.40;
    return t;
}

} // namespace figure8

/** The figure-of-eight tank.

    The control surface is deliberately the SAME as `ReverbTank`'s - decay,
    size, diffusion, crossover with a separate low decay, damping inside the
    loop and a high cut outside it, plus the gate/inverse envelope - so a unit
    that already drives one can drive the other with the setter names it has.
    The two extra setters, `setBandwidth` and `setModRate`/`setModDepth`, have
    sane defaults and a unit that never calls them still gets a modulated tank. */
class Figure8Tank
{
public:
    void prepare (double sampleRate, const Figure8Topology& topo)
    {
        sr  = sampleRate > 0 ? sampleRate : 48000.0;
        top = topo;
        const double k = sr / (top.baseRate > 0 ? top.baseRate : 29761.0) * top.scale;

        // Headroom for the largest Size and the deepest modulation.
        const double head = sr * 0.02 + 64.0;
        for (int i = 0; i < top.numInDiffusers && i < 6; ++i)
        {
            inS[i] = top.inDiffuser[i] * k;
            inAp[i].prepare (sr, (inS[i] * kMaxSize + head) / sr);
        }
        for (int h = 0; h < 2; ++h)
        {
            modS[h] = top.modAllpass[h] * k;
            d1S[h]  = top.delay1[h]     * k;
            ap2S[h] = top.allpass2[h]   * k;
            d2S[h]  = top.delay2[h]     * k;
            modAp[h].prepare (sr, (modS[h] * kMaxSize + head) / sr);
            ap2[h].prepare   (sr, (ap2S[h] * kMaxSize + head) / sr);
            d1[h].prepare    (sr, (d1S[h]  * kMaxSize + head) / sr);
            d2[h].prepare    (sr, (d2S[h]  * kMaxSize + head) / sr);
            damp[h].prepare (sr);
            split[h][0].prepare (sr); split[h][1].prepare (sr);
        }
        bw.prepare (sr);
        outLp[0].prepare (sr); outLp[1].prepare (sr);
        env.prepare (sr);
        modRateHz  = top.modRateHz;
        modDepthMs = top.modDepthMs;
        reset();
        dirty = true;
    }

    void reset()
    {
        for (auto& a : inAp) a.reset();
        for (int h = 0; h < 2; ++h)
        {
            modAp[h].reset(); ap2[h].reset(); d1[h].reset(); d2[h].reset();
            damp[h].reset(); split[h][0].reset(); split[h][1].reset();
            cross[h] = 0.0f;
        }
        bw.reset(); outLp[0].reset(); outLp[1].reset();
        env.reset();
        // The LFO phase is STATE. Leaving it wherever the last render stopped
        // makes two identical renders come out different, which silently
        // poisons every before/after measurement - the same trap the Schroeder
        // tank's random-walk seed fell into.
        phase = 0.0;
    }

    void setDecaySeconds (double s)       { rt = std::max (0.05, s); dirty = true; }
    void setLowDecayMultiplier (double m) { lowMul = std::max (0.05, m); dirty = true; }
    void setCrossover (double hz)         { crossoverHz = hz; dirty = true; }
    void setDamping (double hz)           { dampHz = hz; dirty = true; }    // inside the loop
    void setHighCut (double hz)           { highCutHz = hz; dirty = true; } // outside it
    /** Dattorro's input bandwidth filter: what the tank is allowed to hear. */
    void setBandwidth (double hz)         { bwHz = hz; dirty = true; }
    void setSize (double s)               { size = std::min (std::max (s, 0.1), kMaxSize); dirty = true; }
    void setDiffusion (double d)          { diff = std::min (std::max (d, 0.0), 1.0); dirty = true; }
    void setModRate (double hz)           { modRateHz = std::max (0.0, hz); dirty = true; }
    void setModDepth (double ms)          { modDepthMs = std::max (0.0, ms); dirty = true; }
    void setInfinite (bool b)             { infinite = b; dirty = true; }

    ReverbEnvelope& envelope() { return env; }

    /** The effective loop time in seconds, allpass stretch included: the number
        a unit needs to sanity-check its own decay law against. */
    double loopSeconds() const { return loopTime; }

    /** The shortest RT60 this tank will produce at the Size and Diffusion it has
        now. Rebuilds if it must, so it is safe to call straight after the
        setters and there is no silent-sample dance to remember.

        Use this where Size is a CONTROL the user drives and must not be taken
        away. `fitSizeForDecay` is the other way round, and it is worth knowing
        why the two are not interchangeable: the Size that reaches a given decay
        does not depend on the Size asked for (the circuit scales with Size, so
        the ratio cancels), which means fitting Size flattens whatever Size was
        carrying. On the H3000's SWEPT REVERB, where "m delay" IS the tank size,
        doing that made all 41 factory presets render identically - the median
        difference between them measured 0.1387 before and 0.0000 after. Fit the
        decay to the size there, and the size to the decay only where the decay
        is the control. */
    double minDecaySeconds()
    {
        if (dirty) rebuild();
        return loopTime * kDecayFloorPerCircuit;
    }

    /** Shrink Size until `seconds` is actually reachable, and return the Size
        that ended up being set.

        A tank cannot decay faster than one trip round itself, and this circuit
        is about ten times the Schroeder tank's, so a unit moving from
        `ReverbTank` to this one finds every short program floored: ask
        `figure8::hall()` at Size 1.0 for 0.2 s and 0.62 s is what comes out. No
        decay law fixes that. Size is the control that does, and a decay that
        short IS a small space, so scaling the circuit to fit is the physical
        answer rather than a fudge.

        The floor was measured on all six topologies at 48 kHz, impulse in, RT60
        fitted between -20 and -50 dB with the damping and high cut opened right
        up, asking each for 0.05 s:

            topology    circuit   floor   floor / circuit
            plate         0.927   0.551   0.594
            hall          1.073   0.620   0.578
            room          0.526   0.361   0.685
            chamber       1.015   0.664   0.655
            ambience      0.305   0.292   0.958
            cathedral     1.178   0.630   0.535

        0.78 is the constant used here: above every entry but ambience, so the fit
        errs towards shrinking slightly more than strictly needed, which lands
        the decay rather than leaving it a few per cent long.

        Call this AFTER `setDiffusion`, because the diffusion gains are what
        stretch the circuit and therefore set the floor. */
    double fitSizeForDecay (double seconds, double sizeWanted)
    {
        setSize (sizeWanted);
        if (dirty) rebuild();
        const double floorRt = loopTime * kDecayFloorPerCircuit;
        if (! (seconds < floorRt) || floorRt <= 0.0) return size;
        const double s = std::max (0.1, size * seconds / floorRt);
        setSize (s);
        return s;
    }
    /** The same circuit without the stretch. Exposed only so a test can show
        that the two are far apart and that using this one would be the bug. */
    double nominalLoopSeconds() const { return nominalTime; }

    void process (float inL, float inR, float& outL, float& outR)
    {
        if (dirty) rebuild();
        const float mono = 0.5f * (inL + inR);

        float v = bw.lp (mono);
        for (int i = 0; i < top.numInDiffusers && i < 6; ++i)
            v = inAp[i].process (v, inS[i] * size);

        // One continuous sine, opposite sign in the two halves so they never
        // wander together.
        phase += modInc;
        if (phase >= 1.0) phase -= 1.0;
        const double lfo = std::sin (6.283185307179586 * phase);

        // Both halves read the OTHER half's previous output, so the structure is
        // symmetric. Feeding one half the value the other produced this same
        // sample would make the two directions round the eight different lengths.
        const float fbIn[2] = { cross[1], cross[0] };

        float half[2];
        for (int h = 0; h < 2; ++h)
        {
            float x = v + decayApply (fbIn[h], split[h][0]);
            x = modAp[h].process (x, modS[h] * size + (h == 0 ? lfo : -lfo) * modSamples);
            d1[h].push (x);
            x = d1[h].readCubic (d1S[h] * size);
            x = damp[h].lp (x);
            x = decayApply (x, split[h][1]);
            x = ap2[h].process (x, ap2S[h] * size);
            d2[h].push (x);
            half[h] = d2[h].readCubic (d2S[h] * size);
            half[h] = hardClipf (half[h], 8.0f);
            if (! std::isfinite (half[h])) half[h] = 0.0f;
        }
        cross[0] = half[0]; cross[1] = half[1];

        float wl = 0.0f, wr = 0.0f;
        for (int i = 0; i < top.numOutL && i < 8; ++i) wl += top.outL[i].gain * readNode (top.outL[i]);
        for (int i = 0; i < top.numOutR && i < 8; ++i) wr += top.outR[i].gain * readNode (top.outR[i]);
        wl = outLp[0].lp (wl); wr = outLp[1].lp (wr);

        const float e = env.process (mono);
        outL = wl * e; outR = wr * e;
    }

private:
    static constexpr double kMaxSize = 2.5;
    static float hardClipf (float x, float c) { return x < -c ? -c : (x > c ? c : x); }

    /** One decay multiply, split at the crossover so the low band can ring for
        longer than the high one. Four of these happen per circuit, which is
        what the decay law below has to know. */
    float decayApply (float x, OnePole& s)
    {
        const float lo = s.lp (x);
        return lo * lowFb + (x - lo) * loopFb;
    }

    float readNode (const Figure8Tap& t) const
    {
        const double d = t.samples * (sr / (top.baseRate > 0 ? top.baseRate : 29761.0)) * top.scale * size;
        switch (t.node)
        {
            case Figure8Node::LeftDelay1:    return d1[0].readLinear (d);
            case Figure8Node::LeftAllpass2:  return ap2[0].tap (d);
            case Figure8Node::LeftDelay2:    return d2[0].readLinear (d);
            case Figure8Node::RightDelay1:   return d1[1].readLinear (d);
            case Figure8Node::RightAllpass2: return ap2[1].tap (d);
            case Figure8Node::RightDelay2:   return d2[1].readLinear (d);
        }
        return 0.0f;
    }

    void rebuild()
    {
        dirty = false;

        const float bias = (float) top.diffusionBias;
        const float g1 = clampG (top.inGain1 + (float) (diff - 0.7) * 0.35f + bias);
        const float g2 = clampG (top.inGain2 + (float) (diff - 0.7) * 0.35f + bias);
        for (int i = 0; i < top.numInDiffusers && i < 6; ++i)
            inAp[i].setGain ((i < 2) ? g1 : g2);

        // The tank allpasses move only a little with Diffusion: they are what
        // sets the loop time, so swinging them wide would swing the decay law
        // with them.
        const float gm = clampG (top.modAllpassGain + (float) (diff - 0.7) * 0.12f + bias * 0.5f);
        const float ga = clampG (top.allpass2Gain   + (float) (diff - 0.7) * 0.12f + bias * 0.5f);
        for (int h = 0; h < 2; ++h)
        {
            modAp[h].setGain (gm); modAp[h].setDelaySamples (modS[h] * size);
            ap2[h].setGain (ga);   ap2[h].setDelaySamples (ap2S[h] * size);
        }

        // The effective loop time: pure delays plus the four allpasses AT THEIR
        // STRETCH, plus the one-sample cross-couple register in each half.
        const double stretchM = modalStretch (gm);
        const double stretchA = modalStretch (ga);
        double eff = 0.0, nom = 0.0;
        for (int h = 0; h < 2; ++h)
        {
            const double m = modS[h] * size, a = ap2S[h] * size;
            eff += m * stretchM + a * stretchA + d1S[h] * size + d2S[h] * size + 1.0;
            nom += m + a + d1S[h] * size + d2S[h] * size + 1.0;
        }
        loopTime    = eff / sr;
        nominalTime = nom / sr;

        // FOUR decay multiplies per circuit - two in each half - so a circuit
        // costs g^4. -60 dB after `seconds` therefore wants
        //     g = 10^(-3 * loopTime / (4 * seconds))
        // and the loopTime in it is the stretched one. Using `nominalTime` here
        // is the bug the header talks about: at Dattorro's own gains that is
        // 0.43 of the real figure, so every tail comes out 2.3x too long and
        // anything under a second is simply unreachable.
        auto gainFor = [this] (double seconds) -> float
        {
            if (infinite) return 0.99995f;
            const double need = 3.0 * loopTime / (kDecayMultipliesPerCircuit * std::max (0.05, seconds));
            return (float) std::min (0.9999, std::max (0.0, std::pow (10.0, -need)));
        };
        loopFb = gainFor (rt);
        lowFb  = gainFor (rt * lowMul);

        for (int h = 0; h < 2; ++h)
        {
            damp[h].setCutoff (dampHz);
            split[h][0].setCutoff (crossoverHz);
            split[h][1].setCutoff (crossoverHz);
        }
        bw.setCutoff (bwHz);
        outLp[0].setCutoff (highCutHz); outLp[1].setCutoff (highCutHz);

        modInc = modRateHz / sr;

        // Cap the excursion so a modulated allpass can never read past its own
        // line at a small Size; without this a deep modulation at Size 0.1 wraps
        // the tank onto itself and the tail turns to noise.
        double headroom = 1e9;
        for (int h = 0; h < 2; ++h) headroom = std::min (headroom, modS[h] * size - 2.0);
        modSamples = (float) std::max (0.0, std::min (modDepthMs * 0.001 * sr, headroom));
    }

    static float clampG (float g) { return std::min (0.92f, std::max (0.05f, g)); }

    /** What one allpass of delay D is worth as loop time, as a multiple of D.
        See the note at the top of the file: this is (1+g^2)/(1-g^2), the mean
        group delay WEIGHTED BY THE MODES, not (1+g)/(1-g), which is the value
        at DC only. */
    static double modalStretch (double g)
    { return (1.0 + g * g) / std::max (0.05, 1.0 - g * g); }

    static constexpr double kDecayMultipliesPerCircuit = 4.0;
    /** The shortest RT60 this tank produces, as a fraction of its effective
        circuit. Measured; see `fitSizeForDecay`. */
    static constexpr double kDecayFloorPerCircuit = 0.78;

    Figure8Topology top;
    AllpassFilter inAp[6], modAp[2], ap2[2];
    DelayLine d1[2], d2[2];
    OnePole damp[2], split[2][2], bw, outLp[2];
    ReverbEnvelope env;

    double sr = 48000.0;
    double inS[6] {}, modS[2] {}, d1S[2] {}, ap2S[2] {}, d2S[2] {};
    double rt = 2.0, lowMul = 1.0, crossoverHz = 500.0, dampHz = 6000.0;
    double highCutHz = 9000.0, bwHz = 12000.0;
    double size = 1.0, diff = 0.7;
    double modRateHz = 0.9, modDepthMs = 0.27;
    double loopTime = 0.5, nominalTime = 0.5, phase = 0.0, modInc = 0.0;
    float loopFb = 0.5f, lowFb = 0.5f, modSamples = 0.0f;
    float cross[2] { 0.0f, 0.0f };
    bool infinite = false, dirty = true;
};

} // namespace dsp
