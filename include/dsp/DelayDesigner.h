// The Delay Designer: two lanes of hand-placed taps, each tap with its own
// time, level, feedback, decay and pan.
//
// This is NOT a mimic of any machine. Every other block in this library aims at
// the right KIND of effect for a unit being rebuilt from firmware; this one is
// our own instrument, so it gets whatever controls make it good.
//
// What makes it different from MultiTapDelay next door (which it deliberately
// does not replace): there are TWO independent lanes, each on its own rhythmic
// grid, so a lane of straight quarters can run against a lane of quarter-note
// triplets - and that cross-rhythm is the whole point of the thing. Each tap
// also carries its OWN feedback amount and its OWN damping, where the multi-tap
// block has one feedback taken from one nominated tap.
//
// Three laws are decided here rather than left to the caller, because getting
// any of them wrong is the difference between musical and merely working:
//
//   PAN is constant power. gL = cos(t), gR = sin(t) with t = (pan+1) * pi/4, so
//   gL^2 + gR^2 = 1 at every position. The taps in a delay pattern are
//   decorrelated from one another and from the dry signal, so POWER adds, not
//   amplitude; a linear fade (gL + gR = 1) puts gL^2 + gR^2 = 0.5 at the centre,
//   which is a 3 dB hole exactly where most taps sit.
//
//   MIX is equal power for the same reason and by the same formula:
//   dry = cos(mix * pi/2), wet = sin(mix * pi/2). This matches
//   shell::ShellHost's audition mix on purpose, so the two behave alike.
//
//   FEEDBACK AND DECAY ARE DIFFERENT AXES, and users confuse them because most
//   delays only have one. Here: feedback is how much of a tap is written back
//   into its lane's line, so it decides whether there IS a tail and how strong
//   it starts - broadband, no colour. Decay is a one-pole low-pass on that same
//   tap's feedback contribution, so it decides how the tail DIES: each
//   circulation loses the top end and, with it, energy. Feedback sets the tail's
//   size; decay sets its shape and its colour. They are measurable separately -
//   feedback moves the second repeat's amplitude, decay moves its spectrum while
//   leaving the FIRST repeat untouched - and there is a test for exactly that.
//
// The honest-failure rule (playbook 17b, "a control that lies"): a tap whose
// delay time exceeds the allocated line is NOT clamped to the longest available
// delay. Clamping would put the echo at the wrong beat while the grid keeps
// claiming the right one. It is silenced, counted, and reported through
// tapReachable() / unreachableTaps() so the UI can grey it and say why.
#pragma once
#include "Delay.h"
#include <cmath>
#include <algorithm>

namespace dsp {

/** Two lanes of independently placed delay taps.

    Times are given in BEATS and resolved to samples from the live tempo on
    every setTempo(), never accumulated from a rate. That is what makes the
    thing follow a tempo change - the check that catches a dead clock, where a
    division change never will.

    Real-time safe after prepare(): no allocation, no locks, no statics. */
class DelayDesigner
{
public:
    static constexpr int kLanes = 2;
    /** Taps per lane. 32 is the DSP budget, not an arbitrary round number: each
        tap is a Catmull-Rom read plus a one-pole, so a full pair of lanes is 64
        interpolated reads per sample. It is also the point past which a delay
        pattern stops being a pattern and becomes a reverb. */
    static constexpr int kTapsPerLane = 32;

    /** The most loop gain a lane is allowed to run, summed over its taps. The
        sufficient stability condition for a multi-tap feedback network is
        sum(|fb_i|) < 1; 0.95 leaves a margin. When the sum exceeds it the taps
        are scaled together and feedbackHeadroom() reports the scale, so the UI
        can say the feedback is being held back instead of silently lying. */
    static constexpr float kMaxLoopGain = 0.95f;

    void prepare (double sampleRate, double maxSeconds = 12.0)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxSec = std::max (0.25, maxSeconds);
        for (auto& l : lanes)
            l.line.prepare (sr, maxSec);
        // The allocated line rounds UP to a power of two, so it is usually
        // longer than what was asked for. Reachability is judged against the
        // number the CALLER knows, not the accident of the rounding, because the
        // panel has to grey out exactly the taps the engine will silence and the
        // two must agree on one number.
        maxSec = std::min (maxSec, lanes[0].line.maxDelay() / sr);
        // ~5 ms of gain smoothing: enough to kill the zipper on a pan drag,
        // short enough that nobody hears it as a slew.
        smoothCoef = (float) (1.0 - std::exp (-1.0 / (0.005 * sr)));
        reset();
        retime();
    }

    /** Everything with memory, back to where it started. prepareToPlay calls
        this, and after it two identical renders must be bit-identical: there is
        a test, because a `static` in a DSP function survives every reset and
        cost a day on the D-50. */
    void reset()
    {
        for (auto& l : lanes)
        {
            l.line.reset();
            for (auto& t : l.taps) { t.damp = 0.0f; t.gL = t.gR = 0.0f; t.lp = t.bp = 0.0f; t.env = 0.0f; t.sounding = false; }
            l.lastFired = -1; l.fires = 0;
        }
        mixDry = mixWet = 0.0f;
        primed = false;
    }

    // ---- pattern ---------------------------------------------------------

    void setTapCount (int lane, int n)
    {
        if (! validLane (lane)) return;
        lanes[(size_t) lane].n = std::min (std::max (n, 0), kTapsPerLane);
        retimeLane (lane);
    }

    [[nodiscard]] int tapCount (int lane) const
    { return validLane (lane) ? lanes[(size_t) lane].n : 0; }

    /** One tap. `beats` is its position on the lane's grid in quarter notes,
        `volume` and `feedback` 0..1, `decay` 0..1 (0 darkest, 1 clean), `pan`
        -1 hard left .. +1 hard right. */
    void setTap (int lane, int i, double beats, float volume, float feedback,
                 float decay, float pan, float cutoffHz = 20000.0f, float reso = 0.0f, float driveDb = 0.0f)
    {
        if (! validLane (lane) || i < 0 || i >= kTapsPerLane) return;
        auto& t = lanes[(size_t) lane].taps[(size_t) i];
        t.beats  = std::max (0.0, beats);
        t.volume = clamp01 (volume);
        t.fb     = clamp01 (feedback);
        t.decay  = clamp01 (decay);
        t.pan    = std::min (std::max (pan, -1.0f), 1.0f);
        t.dampA  = dampCoef (t.decay);
        // THE TAP'S OWN FILTER AND DRIVE (2026-09-19): a resonant low-pass on
        // what this tap plays, and a saturator after it, each tap its own. A
        // cutoff at the top with no resonance and no drive is a straight wire,
        // which is what every tap set before today asks for.
        // AN OPEN TAP IS A WIRE: at or above a third of the sample rate (16 kHz
        // at 48k, where the unit's own CUTOFF knob tops out) the filter is off.
        // Below it the coefficient is capped where the Chamberlin form stays
        // stable at the lowest Q: at 18 kHz through the old cap the state ran
        // to infinity and the whole grid read NaN.
        t.svfOn  = cutoffHz < 0.34f * (float) sr || reso > 0.005f;
        t.svfF   = 2.0f * std::sin ((float) M_PI * std::min (std::max (cutoffHz / (float) sr, 0.0005f), 0.18f));
        t.svfQ   = std::max (0.06f, 1.0f - clamp01 (reso) * 0.97f);
        t.drive  = driveDb > 0.01f ? std::pow (10.0f, std::min (driveDb, 24.0f) / 20.0f) : 0.0f;
        t.driveNorm = t.drive > 0.0f ? 0.5f / std::tanh (0.5f * t.drive) : 1.0f;
        // Constant power, computed once here rather than per sample: the pan
        // law is trig and 64 taps of it per sample is not worth paying for.
        const float th = (t.pan + 1.0f) * 0.25f * (float) M_PI;
        t.tgtL = t.volume * std::cos (th);
        t.tgtR = t.volume * std::sin (th);
        retimeTap (lane, i);
        rescale (lane);
    }

    /** The tempo every tap's time is built from. Call it every block: building
        the time from the LIVE tempo is what makes a tempo change land on the
        block it happened, rather than a block late or not at all. */
    void setTempo (double bpm)
    {
        const double b = bpm >= 20.0 ? bpm : 120.0;
        if (b == tempo) return;
        tempo = b;
        retime();
    }

    [[nodiscard]] double tempoBpm() const noexcept { return tempo; }

    /** 0 = dry only, 1 = designed delay only. Equal power, see the header. */
    void setMix (float mix01)
    {
        mix = clamp01 (mix01);
        const float th = mix * 0.5f * (float) M_PI;
        tgtDry = std::cos (th);
        tgtWet = std::sin (th);
    }

    [[nodiscard]] float mixAmount() const noexcept { return mix; }

    // ---- render ----------------------------------------------------------

    /** In place is allowed: every input sample is read before anything is
        written. */
    void process (const float* inL, const float* inR,
                  float* outL, float* outR, int numSamples)
    {
        if (! primed) { snap(); primed = true; }

        for (int s = 0; s < numSamples; ++s)
        {
            const float xL = inL != nullptr ? inL[s] : 0.0f;
            const float xR = inR != nullptr ? inR[s] : 0.0f;
            // The send is MONO, which is deliberate: a delay designer places
            // every tap in the field itself, so feeding it a stereo image would
            // fight the pan controls. This is a rack unit on a mono aux send,
            // which is the mental model the panel presents.
            const float in = 0.5f * (xL + xR);

            float wetL = 0.0f, wetR = 0.0f;
            for (auto& l : lanes)
            {
                float fbSum = 0.0f;
                for (int i = 0; i < l.n; ++i)
                {
                    auto& t = l.taps[(size_t) i];
                    // Unreachable: silent, not clamped. An echo at the wrong
                    // beat is worse than no echo, and the UI says which.
                    if (! t.reachable) continue;

                    const float v = shape (t, l.line.readCubic (t.samples));
                    // THE HIT DETECTOR: a tap's level rising out of silence is
                    // a hit; it re-arms once the level has fallen well below.
                    t.env += 0.01f * (std::fabs (v) - t.env);
                    if (! t.sounding && t.env > 0.02f) { t.sounding = true; l.lastFired = i; ++l.fires; }
                    else if (t.sounding && t.env < 0.005f) t.sounding = false;
                    t.gL += smoothCoef * (t.tgtL - t.gL);
                    t.gR += smoothCoef * (t.tgtR - t.gR);
                    wetL += v * t.gL;
                    wetR += v * t.gR;

                    float f = v;
                    if (t.dampA > 0.0f) { t.damp += t.dampA * (f - t.damp); f = t.damp; }
                    fbSum += f * t.fb * l.fbScale;
                }
                l.line.push (in + fbSum);
            }

            mixDry += smoothCoef * (tgtDry - mixDry);
            mixWet += smoothCoef * (tgtWet - mixWet);
            outL[s] = xL * mixDry + wetL * mixWet;
            outR[s] = xR * mixDry + wetR * mixWet;
        }
    }

    /** THE SAME NETWORK WITH EACH LANE'S WET ON ITS OWN OUTPUT PAIR, and no
        dry: for a unit that processes the lanes differently before it sums
        them (MegaSynth's TAPE puts the PSP 608's per-lane filter, drive and
        reverb send behind this, 2026-09-19). `laneL[l]` / `laneR[l]` are the
        two lanes' stereo wets; MIX is not applied. */
    void processLanes (const float* inL, const float* inR,
                       float* const* laneL, float* const* laneR, int numSamples)
    {
        if (! primed) { snap(); primed = true; }
        for (int s = 0; s < numSamples; ++s)
        {
            const float xL = inL != nullptr ? inL[s] : 0.0f;
            const float xR = inR != nullptr ? inR[s] : 0.0f;
            const float in = 0.5f * (xL + xR);
            for (int li = 0; li < kLanes; ++li)
            {
                auto& l = lanes[(size_t) li];
                float wetL = 0.0f, wetR = 0.0f, fbSum = 0.0f;
                for (int i = 0; i < l.n; ++i)
                {
                    auto& t = l.taps[(size_t) i];
                    if (! t.reachable) continue;
                    const float v = shape (t, l.line.readCubic (t.samples));
                    // THE HIT DETECTOR: a tap's level rising out of silence is
                    // a hit; it re-arms once the level has fallen well below.
                    t.env += 0.01f * (std::fabs (v) - t.env);
                    if (! t.sounding && t.env > 0.02f) { t.sounding = true; l.lastFired = i; ++l.fires; }
                    else if (t.sounding && t.env < 0.005f) t.sounding = false;
                    t.gL += smoothCoef * (t.tgtL - t.gL);
                    t.gR += smoothCoef * (t.tgtR - t.gR);
                    wetL += v * t.gL;
                    wetR += v * t.gR;
                    float f = v;
                    if (t.dampA > 0.0f) { t.damp += t.dampA * (f - t.damp); f = t.damp; }
                    fbSum += f * t.fb * l.fbScale;
                }
                // the whole pattern's recirculation, beside the taps' own
                l.line.push (in + fbSum + 0.5f * (wetL + wetR) * l.patternFb * l.fbScale);
                laneL[li][s] = wetL;
                laneR[li][s] = wetR;
            }
        }
    }
    // ---- what the UI has to be able to say -------------------------------

    /** False when the tap's time is longer than the line that was allocated.
        Such a tap is silent, and the panel must show it as silent. */
    [[nodiscard]] bool tapReachable (int lane, int i) const
    {
        return validLane (lane) && i >= 0 && i < lanes[(size_t) lane].n
            && lanes[(size_t) lane].taps[(size_t) i].reachable;
    }

    /** How many taps are out of reach right now, across both lanes. Changes
        with the TEMPO, because a slow tempo makes every tap longer - which is
        exactly why this cannot be decided once at edit time. */
    [[nodiscard]] int unreachableTaps() const
    {
        int n = 0;
        for (const auto& l : lanes)
            for (int i = 0; i < l.n; ++i)
                if (! l.taps[(size_t) i].reachable) ++n;
        return n;
    }

    /** 1.0 when the lane's feedback is running as asked; less when the taps had
        to be scaled together to keep the lane stable. */
    [[nodiscard]] float feedbackHeadroom (int lane) const
    { return validLane (lane) ? lanes[(size_t) lane].fbScale : 1.0f; }

    /** The longest delay this designer will produce, in seconds. A tap past it
        is silenced and counted, never clamped. */
    [[nodiscard]] double maxDelaySeconds() const { return maxSec; }

    /** WHICH TAP OF A LANE SOUNDED LAST (-1: none yet), and how many times
        a tap has sounded on that lane: a hit is the tap's output rising out
        of silence, followed per tap by a one-pole envelope. A unit's panel
        reads these at display rate to put the sounding tap's own values on
        the unit's knobs (MegaSynth's TAPE: the author, "a knob position should match
        that for each hit"). */
    /** THE WHOLE LANE'S FEEDBACK, 0..0.9: its summed output pushed back into
        its own line, so the drawn pattern repeats and decays as a pattern.
        This is a DIFFERENT control from a tap's own feedback, which repeats
        that ONE tap at its own time; a lane whose taps all have zero feedback
        is silent after one pass however hard you turn a per-tap knob, which is
        what "the feedback knob doesnt work when lanes are active" was. Scaled
        by the lane's own stability factor, as the per-tap feedback is. */
    void setLaneFeedback (int lane, float amount)
    {
        if (! validLane (lane)) return;
        lanes[(size_t) lane].patternFb = std::min (std::max (amount, 0.0f), 0.9f);
    }

    [[nodiscard]] int lastFiredTap (int lane) const { return validLane (lane) ? lanes[(size_t) lane].lastFired : -1; }
    [[nodiscard]] unsigned fireCount (int lane) const { return validLane (lane) ? lanes[(size_t) lane].fires : 0u; }

    /** A tap's resolved time in samples, for a test or a readout. */
    [[nodiscard]] double tapSamples (int lane, int i) const
    {
        return validLane (lane) && i >= 0 && i < kTapsPerLane
             ? lanes[(size_t) lane].taps[(size_t) i].samples : 0.0;
    }

private:
    struct Tap
    {
        double beats = 0.0, samples = 0.0;
        float  volume = 0.0f, fb = 0.0f, decay = 1.0f, pan = 0.0f;
        float  tgtL = 0.0f, tgtR = 0.0f;   // constant-power gains, precomputed
        float  gL = 0.0f, gR = 0.0f;       // smoothed, so a pan drag does not zip
        float  dampA = 0.0f, damp = 0.0f;  // the decay one-pole
        bool   svfOn = false; float svfF = 0.0f, svfQ = 1.0f, lp = 0.0f, bp = 0.0f;   // the tap's own filter
        float  drive = 0.0f, driveNorm = 1.0f;                                       // and its drive
        float  env = 0.0f; bool sounding = false;                                    // the hit detector
        bool   reachable = true;
    };

    struct Lane
    {
        DelayLine line;
        Tap  taps[kTapsPerLane];
        int  n = 0;
        float fbScale = 1.0f;
        int  lastFired = -1; unsigned fires = 0;
        float patternFb = 0.0f;   // the WHOLE pattern's recirculation, below
    };

    static bool validLane (int l) { return l >= 0 && l < kLanes; }

    /** THE TAP'S OWN FILTER AND DRIVE on what it just read (2026-09-19): a
        state-variable low-pass, Q floored so a full resonance rings and does
        not run away, then the TAPE unit's own drive law (a signal at -6 dBFS
        out at -6 dBFS whatever the drive). Both loops - the summed process()
        and processLanes() - go through here, so a tap sounds the same
        whichever the unit calls. */
    static float shape (Tap& t, float v)
    {
        if (t.svfOn)
        {
            const float hp = v - t.lp - t.svfQ * t.bp;
            t.bp += t.svfF * hp;
            t.lp += t.svfF * t.bp;
            if (! std::isfinite (t.lp) || ! std::isfinite (t.bp)) { t.lp = t.bp = 0.0f; }
            v = t.lp;
        }
        if (t.drive > 0.0f) v = std::tanh (v * t.drive) * t.driveNorm;
        return v;
    }
    static float clamp01 (float v) { return std::min (std::max (v, 0.0f), 1.0f); }

    /** Decay to a low-pass corner: 0 -> 200 Hz (the tail dies dark and fast),
        1 -> open, no filter at all. Exponential, because pitch and brightness
        are heard that way. */
    float dampCoef (float decay) const
    {
        const double fc = 200.0 * std::pow (20000.0 / 200.0, (double) decay);
        if (fc >= sr * 0.45) return 0.0f;                    // clean repeat
        return (float) (1.0 - std::exp (-2.0 * M_PI * fc / sr));
    }

    void retime()
    { for (int l = 0; l < kLanes; ++l) retimeLane (l); }

    void retimeLane (int lane)
    {
        auto& l = lanes[(size_t) lane];
        for (int i = 0; i < kTapsPerLane; ++i) retimeTap (lane, i);
        (void) l;
        rescale (lane);
    }

    void retimeTap (int lane, int i)
    {
        auto& t = lanes[(size_t) lane].taps[(size_t) i];
        t.samples = t.beats * (60.0 / tempo) * sr;
        t.reachable = t.samples <= maxSec * sr;
    }

    /** Hold the lane's summed loop gain under kMaxLoopGain. Only reachable taps
        count, because a silenced tap feeds back nothing. */
    void rescale (int lane)
    {
        auto& l = lanes[(size_t) lane];
        float sum = 0.0f;
        for (int i = 0; i < l.n; ++i)
            if (l.taps[(size_t) i].reachable) sum += l.taps[(size_t) i].fb;
        l.fbScale = sum > kMaxLoopGain ? kMaxLoopGain / sum : 1.0f;
    }

    void snap()
    {
        for (auto& l : lanes)
            for (auto& t : l.taps) { t.gL = t.tgtL; t.gR = t.tgtR; }
        mixDry = tgtDry;
        mixWet = tgtWet;
    }

    Lane   lanes[kLanes];
    double sr = 48000.0, tempo = 120.0, maxSec = 12.0;
    float  smoothCoef = 0.01f;
    float  mix = 0.5f, tgtDry = 0.70710678f, tgtWet = 0.70710678f;
    float  mixDry = 0.0f, mixWet = 0.0f;
    bool   primed = false;
};

} // namespace dsp
