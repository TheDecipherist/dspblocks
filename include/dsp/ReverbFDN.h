// A FEEDBACK DELAY NETWORK with per-band decay, built entirely from published
// work. See ~/projects/eproms/docs/reverb-research.md for the survey this came
// out of and for every citation.
//
// WHY THIS EXISTS. The author judged every finished effects unit by ear in one session
// and his verdicts sorted the archive perfectly by which tank it ran: five on
// dsp::Figure8Tank were "the best reverbs and programs you have made so far",
// twelve on ReverbTank's stock topologies were "metal playing in a bathroom",
// "a can through a comb". Measured on the PCM 60, those twelve carry an ECHO
// DENSITY of 0.20 to 0.33 where a diffused tail approaches 1.0, with
// autocorrelation peaks of 0.23 to 0.34 at lags of 1.6 to 5 ms - a 5 ms lag is
// a 200 Hz ring, and that is the metal, measured.
//
// WHAT IS FROM WHERE, because this must never be mistaken for a machine:
//
//   [PUB] the four input allpass diffusers, at Dattorro's own lengths.
//         Dattorro, "Effect Design Part 1", JAES 45(9), 1997. The front end is
//         most of why his structure sounds good and it is orthogonal to
//         whatever tank sits behind it, so it is kept exactly.
//   [PUB] the feedback delay network and its lossless mixing matrix.
//         Stautner & Puckette, CMJ Spring 1982; Jot. A figure-of-eight is a
//         TWO delay network with a fixed crossing; density grows with the
//         number of lines for the same memory, and thin density IS too few
//         lines recirculating.
//   [PUB] per-band decay, g = 10^(-3 D / (T60 fs)) per line per band. Jot's
//         method. Independently, Yamaha's own patent US 4,731,835 divides the
//         signal with an HPF, a BPF and an LPF "for forming different
//         reverberation tones for respective frequency bands" - the same idea
//         from the other direction, a decade earlier.
//   [EAR] THE DELAY LENGTHS ARE OURS. Eight primes, mutually coprime by
//         construction. No machine's tap set is copied and none could be: of
//         every unit in this archive only the M-ONE could ever yield one, and
//         only if its DSP PROGRAM memory turns up.
//
// So this is a better reverb than the twelve currently have. It is not any of
// them, and no unit may claim it is.
#pragma once
#include "Delay.h"
#include "Filters.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace dsp {

/** THE DRUNKEN TAPE, kept on purpose.

    The author, on a build that had the modulation on the OUTPUT taps: "some kind of
    weird comb modulating effect. sounding like the sound is drunk or driven
    through a tape thats out of batteries." Then: "make a note of the settings
    for the drunken tape lol. could be fun to use as a preset effect actually."

    He is right, and it is cheap to get back. It was a fault as a reverb and it
    is a real effect in its own right - a tape machine with a tired motor, wow
    and flutter on a diffuse wash.

    To rebuild it, from the current code:

      1. In process(), mix the MODULATED reads v[] into the output alongside
         the fixed interior taps t[] - the version that caused it was
           l = 0.6f * (t[0]+t[2]-t[4]+t[6]) + 0.4f * (v[1]-v[5]);
           r = 0.6f * (t[1]-t[3]+t[5]+t[7]) + 0.4f * (v[2]+v[6]);
         Any v[] in the output puts the sweep on the signal, which IS the wow.
      2. Modulate ALL eight lines rather than the alternating half: set every
         entry of kModulated to true.
      3. Raise the depth cap in setModDepth from 1.2 ms to 8 ms and ask for
         2 to 6 ms. 12 ms - which is what law::modWanderMs returns at nominal -
         is the full seasick version.
      4. Slow the rate: modHz around 0.15 to 0.3, so the drift is a wander
         rather than a vibrato.

    An effect built from this must say what it is. It is NOT a machine in this
    archive and no unit may present it as one - it is a fault of ours, kept
    because it sounded good, which is a respectable thing for an effect to be
    as long as it is labelled honestly. */
class FDNTank
{
public:
    static constexpr int kLines = 16;

    /** Decorrelates this instance's modulation from every other. A machine
        running two of these - the M-ONE blends two engines with FX Balance -
        must give them different seeds or the two walks move together and the
        sum is a chorus rather than a wash. */
    void setSeed (uint32_t s) { seed = s; }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        // [EAR] Eight primes from about 4.8 ms to 40 ms at 48 kHz. The SPREAD
        // matters more than the values. The first set ran 23 to 58 ms, so
        // nothing at all arrived between the diffusers and 23 ms and then the
        // tail was immediately dense - the author: "doesnt sound like a reverb now.
        // more like the ocean". A tank needs a BUILD-UP: short lines first, so
        // echoes arrive, then thicken. Prime, so mutually coprime, so no two
        // lines share a period.
        //
        // AND THEY SET THE DEFAULT ROOM, which is a judgement and not a
        // measurement. At 7..62 ms the author had "its massive. but really really
        // big" - a cathedral at every setting, with the machine's own Size
        // control (three positions, 0.70 / 1.00 / 1.45) far too narrow to
        // bring it back. These lengths put the DEFAULT at a large room, so a
        // 2:1 size control spans something useful either side of it.
        // SIXTEEN, not eight. The author on P02, with one engine and FX Balance at
        // zero: "like a delay effect, triplet like, of the original tap". That
        // is what too few lines sounds like on a sparse source - each line
        // returns as its own audible echo instead of disappearing into a wash.
        // Density grows with the number of lines for the same memory, which is
        // the FDN's whole argument (Stautner & Puckette; Jot), and eight was
        // reaching only 0.40 of the 1.0 a diffused tail gives.
        static constexpr int kBase[kLines] =
            {  229,  293,  367,  439,  541,  631,  757,  881,
              1013, 1151, 1327, 1489, 1621, 1787, 1933, 2113 };
        for (int i = 0; i < kLines; ++i)
        {
            base[i] = kBase[i] * sr / 48000.0;
            // room for 4x size and the modulation swing
            line[i].prepare (sr, (base[i] * 4.0 + 0.05 * sr) / sr);
            lowSplit[i].prepare (sr);
            highSplit[i].prepare (sr);
        }
        // [PUB] Dattorro's four input diffusers, at his lengths, which he gives
        // in samples at his own 29761 Hz.
        static constexpr int kDif[4] = { 142, 107, 379, 277 };
        for (int i = 0; i < 4; ++i)
            dif[i].prepare (sr, (kDif[i] * sr / 29761.0 + 8.0) / sr);
        for (auto& f : outLp) f.prepare (sr);
        reset();
        dirty = true;
    }

    void reset()
    {
        for (auto& d : line) d.reset();
        for (auto& d : dif)  d.reset();
        for (auto& f : lowSplit)  f.reset();
        for (auto& f : highSplit) f.reset();
        for (auto& f : outLp) f.reset();
        for (auto& s : state) s = 0.0f;
        for (int i = 0; i < kLines; ++i)
        {
            phase[i] = (float) i / kLines;
            walkT[i] = (float) i / kLines;
            pos[i] = 0.0f; target[i] = 0.0f;
            // SEEDED PER INSTANCE. The author: "the FX Balance is what makes the
            // drunken tape sound. 100% crazy. at 0% no drunken sound." FX
            // Balance blends the machine's TWO engines, which are two of these
            // tanks - and every instance seeded its walk identically, so both
            // wandered in perfect lockstep and summing them DOUBLED the pitch
            // movement instead of averaging it away. One engine was clean; two
            // in step were a chorus.
            rng[i] = 22695477u * (uint32_t) (i + 1) + 1u + seed * 2654435761u;
        }
        for (int i = 0; i < 4; ++i)
        {
            difWalkT[i] = (float) i / 4.0f;
            difPos[i] = difTarget[i] = 0.0f;
            difRng[i] = 69069u * (uint32_t) (i + 7) + 1u + seed * 2654435761u;
        }
    }

    /** The MID band's reverberation time, in seconds. The other two bands are
        multiples of it, which is how a machine with RT LOW and RT HIGH RANGE
        controls actually presents this and how Jot parameterises it. */
    void setDecaySeconds (double s)       { rt = std::max (0.05, s); dirty = true; }
    /** Low band decay, as a multiple of the mid. A real room is LONGER low, so
        values above 1 are the normal case - and this is the thing Dattorro's
        single decay coefficient structurally cannot express. */
    void setLowMultiplier (double m)      { lowMul = clampd (m, 0.05, 8.0); dirty = true; }
    /** High band decay, as a multiple of the mid. Normally below 1: air and
        surfaces absorb the top first. */
    void setHighMultiplier (double m)     { highMul = clampd (m, 0.05, 8.0); dirty = true; }
    void setLowCrossover (double hz)      { lowHz = clampd (hz, 40.0, 1000.0); dirty = true; }
    void setHighCrossover (double hz)     { highHz = clampd (hz, 800.0, 12000.0); dirty = true; }
    /** Scales every line together, so the room gets bigger without the ratios
        between the lines changing - which is what keeps them coprime. */
    /** Scales every line together, so the room gets bigger without the ratios
        between the lines changing - which is what keeps them coprime. The range
        is deliberately wide, 0.25 to 4, because a machine whose own size
        control is a three-position enum needs the tank to do the work. */
    void setSize (double s)               { size = clampd (s, 0.25, 4.0); dirty = true; }
    /** 0 is the bare network, 1 is all four input diffusers at full. */
    void setDiffusion (double d)          { diff = (float) clampd (d, 0.0, 1.0); }
    /** Depth of the per-line modulation, in milliseconds.

        SMALL. The author, on the first build: "there is an odd whooshing sound. like a
        wave hitting and coming back at you." That is what a swept delay line
        is - a swept comb - and eight of them sweeping together is a chorus
        across the whole tail. Dattorro modulates TWO taps and the research note
        that modulating every line raises density was taken as if it were free.
        It is not.

        So: half the lines are modulated, not all (see kModulated), and the
        depth is capped at 2.5 ms rather than 8. Above about 1 ms the sweep is
        audible as pitch movement on a sustained sound, which is a flanger and
        not a reverb. */
    void setModDepth (double ms)          { modMs = clampd (ms, 0.0, 1.2); }

    /** OFF, SMOOTH or VINTAGE, which is the M-ONE's own Mod Type and a TYPE
        rather than an amount. [MAN] the M-ONE manual, Reverb - Hall:

          Smooth  "uses a complicated modulation pattern, that allows the Reverb
                   Tail to be modulated WITHOUT DETUNING the original source"
          Vintage "a very simple modulation pattern that tended to detune the
                   original source slightly ... the traditional detuning effect"

        So detuning is a CHOICE the machine offers on one setting out of three,
        and the difference is STRUCTURAL rather than a gain. SMOOTH moves the
        DIFFUSER COEFFICIENTS and leaves every delay length alone, so the
        pattern changes over time while nothing is ever resampled - no Doppler,
        no pitch. VINTAGE walks the delay reads, which shifts pitch, because
        that is the thing it exists to reproduce. */
    enum class ModType { off, smooth, vintage };
    void setModType (ModType t)           { modType = t; }
    void setModRate (double hz)           { modHz = clampd (hz, 0.01, 4.0); }
    /** Output lowpass, outside the loop. */
    void setHighCut (double hz)           { cutHz = clampd (hz, 500.0, 20000.0); dirty = true; }

    void process (float inL, float inR, float& outL, float& outR)
    {
        if (dirty) rebuild();

        // [PUB] the input diffusers, in series, both channels summed in - the
        // network behind them is where the stereo is made.
        float x = 0.5f * (inL + inR);
        // SMOOTH's modulation lives HERE, in the diffusers' COEFFICIENTS. Each
        // wanders a little around its nominal g, independently, so the
        // diffusion pattern is never the same twice and the tail decorrelates -
        // and because no delay LENGTH moves, nothing is resampled and nothing
        // detunes. That is the whole difference between Smooth and Vintage, and
        // it is why Smooth can be the default on a reverb meant to leave the
        // source alone.
        float gMod[4] { 0.0f, 0.0f, 0.0f, 0.0f };
        if (modType == ModType::smooth && modMs > 0.0)
            for (int i = 0; i < 4; ++i)
            {
                difWalkT[i] -= (float) (modHz * (1.0 + 0.31 * i) / sr);
                if (difWalkT[i] <= 0.0f)
                {
                    difWalkT[i] += 1.0f;
                    difRng[i] = difRng[i] * 1664525u + 1013904223u;
                    difTarget[i] = (float) ((int32_t) (difRng[i] >> 8) % 2000 - 1000) * 0.001f;
                }
                difPos[i] += (float) (modHz / sr) * 3.0f * (difTarget[i] - difPos[i]);
                gMod[i] = difPos[i] * 0.22f * (float) (modMs / 1.2);
            }

        for (int i = 0; i < 4; ++i)
        {
            const float d  = dif[i].readLinear (difLen[i]);
            const float gg = kDifG * diff + gMod[i];
            const float v  = x - gg * d;
            dif[i].push (v);
            x = gg * v + d;
        }

        // read every line, modulated
        float v[kLines];
        for (int i = 0; i < kLines; ++i)
        {
            // ONLY HALF THE LINES MOVE. The unmodulated half holds the pitch
            // steady, so what the moving half adds is decorrelation rather
            // than vibrato. Modulating all eight was the whoosh.
            if (kModulated[i] && modMs > 0.0 && modType == ModType::vintage)
            {
                // A RANDOM WALK, NOT A SINE. The author found it by program: P00 and
                // P01 differ only in Mod Speed, 7 against 20, which this
                // machine's law turns into 1.33 Hz and 2.73 Hz - and P00 was
                // "drunk or driven through a tape thats out of batteries"
                // while P01 was fine. A sine near 1 Hz is the worst case there
                // is: slow enough that the ear tracks the pitch rising and
                // falling, which IS wow. Faster hides it, and that is why the
                // two programs disagreed.
                //
                // So the shape is wrong, not the depth. A walk has no period
                // to latch onto - it wanders and never repeats - which is what
                // the old tank's setSpin did and why nobody ever heard it as
                // pitch. Target moves at modHz, the position eases toward it,
                // so the motion is smooth and aperiodic.
                walkT[i] -= (float) (modHz * rateMul[i] / sr);
                if (walkT[i] <= 0.0f)
                {
                    walkT[i] += 1.0f;
                    rng[i] = rng[i] * 1664525u + 1013904223u;
                    target[i] = (float) ((int32_t) (rng[i] >> 8) % 2000 - 1000) * 0.001f;
                }
                // EASE OVER THE WHOLE INTERVAL, not in 14 ms and then wait.
                // A fixed coefficient of 0.0015 is a 14 ms time constant, so
                // at P00's 1.33 Hz the position SNAPPED to each new target in
                // 14 ms and then sat still for 736 - a series of little slides
                // rather than a wander, and the author heard it as "windy ringy tapy"
                // with one engine and FX Balance at zero. The rate of travel
                // has to follow the rate of the walk.
                pos[i] += (float) (modHz * rateMul[i] / sr) * 3.0f * (target[i] - pos[i]);
                const double swing = modMs * 0.001 * sr * pos[i];
                v[i] = line[i].readLinear (std::max (1.0, len[i] + swing));
            }
            else
                v[i] = line[i].readLinear (len[i]);
        }

        // [PUB] per-band absorption, per line. Three bands, each with the gain
        // Jot's formula gives for THIS line's length and THAT band's T60, so
        // every line decays correctly in every band regardless of its length.
        for (int i = 0; i < kLines; ++i)
        {
            const float lo = lowSplit[i].lp (v[i]);
            const float hi = v[i] - highSplit[i].lp (v[i]);
            const float mid = v[i] - lo - hi;
            v[i] = gLow[i] * lo + gMid[i] * mid + gHigh[i] * hi;
        }

        // [PUB] the lossless mixing matrix. Householder: y = x - (2/N) sum(x),
        // which is orthogonal, so the network loses energy only where we chose
        // to lose it - in the absorption above - and nowhere else.
        float sum = 0.0f;
        for (int i = 0; i < kLines; ++i) sum += v[i];
        const float corr = 2.0f / kLines * sum;
        for (int i = 0; i < kLines; ++i)
            line[i].push (x * kInGain + (v[i] - corr));

        // [PUB] TAPS FROM INSIDE THE LINES, not from their ends - which is
        // Dattorro's idea and the thing an earlier version of this file claimed
        // to do and did not. Reading the line ends put line 0's whole output,
        // 229 samples or 4.77 ms, straight into the mix as a single arrival at
        // 70 per cent of peak: a discrete slapback, and against a dry signal a
        // 4.77 ms comb. The author heard it immediately at 50 per cent mix - "its
        // almost like the sound is doubled".
        //
        // Each line is now read a SECOND time at an interior offset, at a
        // different irrational fraction of its own length so no two interior
        // taps coincide with each other or with any line end. Sixteen
        // non-coincident arrivals instead of eight, none of them the whole of
        // anything, which is what makes an onset diffuse rather than discrete.
        static constexpr double kTapFrac[kLines] =
            { 0.413, 0.629, 0.271, 0.557, 0.337, 0.691, 0.479, 0.233,
              0.571, 0.317, 0.653, 0.389, 0.719, 0.251, 0.607, 0.443 };
        // AND THESE TAPS DO NOT MOVE. The modulation belongs in the
        // RECIRCULATION, where it decorrelates what goes round again; on an
        // OUTPUT tap it is vibrato, and the listener hears it as pitch. The author:
        // "some kind of weird comb modulating effect. sounding like the sound
        // is drunk or driven through a tape thats out of batteries" - which is
        // wow, exactly, and it was the output taps riding the same swept reads
        // that feed the matrix. Fixed positions here; the swept reads above
        // still feed the network.
        float t[kLines];
        for (int i = 0; i < kLines; ++i)
            t[i] = line[i].readLinear (std::max (1.0, len[i] * kTapFrac[i]));

        // Alternating signs decorrelate the two sides.
        // v[] is the MODULATED read and feeds the matrix only. Mixing any of
        // it into the output put the sweep back on the signal.
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < kLines; i += 2) l += (i % 4 == 0) ? t[i] : -t[i];
        for (int i = 1; i < kLines; i += 2) r += (i % 4 == 1) ? t[i] : -t[i];
        l = outLp[0].lp (l);
        r = outLp[1].lp (r);
        outL = l * 0.19f;
        outR = r * 0.19f;
    }

private:
    static double clampd (double v, double lo, double hi)
    { return v < lo ? lo : (v > hi ? hi : v); }

    /** Jot: a line of D samples must have gain g per pass so that the tail is
        60 dB down after T60 seconds. g = 10^(-3 D / (T60 fs)). */
    static float jotGain (double samples, double t60, double sr)
    {
        if (t60 <= 0.0) return 0.0f;
        return (float) std::pow (10.0, -3.0 * samples / (t60 * sr));
    }

    void rebuild()
    {
        dirty = false;
        for (int i = 0; i < kLines; ++i)
        {
            len[i] = base[i] * size;
            gMid[i]  = jotGain (len[i], rt, sr);
            gLow[i]  = jotGain (len[i], rt * lowMul, sr);
            gHigh[i] = jotGain (len[i], rt * highMul, sr);
            lowSplit[i].setCutoff (lowHz);
            highSplit[i].setCutoff (highHz);
        }
        static constexpr int kDifBase[4] = { 142, 107, 379, 277 };
        for (int i = 0; i < 4; ++i) difLen[i] = kDifBase[i] * sr / 29761.0;
        for (auto& f : outLp) f.setCutoff (cutHz);
    }

    static constexpr float kDifG   = 0.625f;   // [PUB] Dattorro's input diffusion
    static constexpr float kInGain = 0.5f;

    double sr = 48000.0;
    std::array<DelayLine, kLines> line;
    std::array<DelayLine, 4> dif;
    std::array<OnePole, kLines> lowSplit, highSplit;
    std::array<OnePole, 2> outLp;
    double base[kLines] {}, len[kLines] {}, difLen[4] {};
    float  gLow[kLines] {}, gMid[kLines] {}, gHigh[kLines] {};
    float  state[kLines] {}, phase[kLines] {};
    float  walkT[kLines] {}, pos[kLines] {}, target[kLines] {};
    float  difWalkT[4] {}, difPos[4] {}, difTarget[4] {};
    uint32_t difRng[4] {};
    ModType modType = ModType::smooth;
    uint32_t rng[kLines] {};
    // [EAR] incommensurate modulation rates, so no two lines ever line up
    static constexpr float rateMul[kLines] =
        { 1.00f, 1.13f, 1.31f, 1.47f, 1.63f, 1.79f, 1.97f, 2.11f,
          1.07f, 1.23f, 1.39f, 1.53f, 1.71f, 1.87f, 2.03f, 2.23f };
    // Alternating, so a moving line always sits next to a steady one in the
    // matrix and the network never sweeps as a whole.
    static constexpr bool kModulated[kLines] =
        { true, false, true, false, true, false, true, false,
          true, false, true, false, true, false, true, false };

    double rt = 2.0, lowMul = 1.5, highMul = 0.6;
    double lowHz = 250.0, highHz = 4000.0, size = 1.0, modMs = 0.25, modHz = 0.28;
    double cutHz = 12000.0;
    float  diff = 1.0f;
    uint32_t seed = 0;
    bool   dirty = true;
};

} // namespace dsp
