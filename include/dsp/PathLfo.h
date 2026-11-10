/*  MegaSynth - the engine side of the drawable LFO: everything that READS a path.

    `LfoPath.h` is the shape and nothing else. This is the module that PLAYS it:
    a rate in Hz or locked to the clock, a direction, what a new note does to it,
    the delay and rise that fade the shape in, a slew on the output and a start
    phase. Serum's own control table - Serum 2 user guide pp. 191-193, in
    `docs/serum-guide/15-exploring-sound-modulation.md` - is the specification,
    and every law below says whether the guide states it (`[MAN]`) or we fitted
    it (`[EAR]`).

    WHY THE PATH IS AN ARGUMENT AND NOT A MEMBER. A path belongs to the PROGRAM
    and this block belongs to a VOICE, so sixteen voices reading one path must
    not each carry a copy of a 32-point array; and a block holding a pointer to
    one would have a lifetime to get wrong on every program change. `tick()`
    takes the path it is to read, which also leaves this struct trivially
    copyable, so resetting a voice is an assignment.

    WHAT THIS FILE IS NOT. No JUCE, no parameters, no Program, no allocation and
    nothing that can throw. The coefficient builder turns the panel's frozen ids
    into the arguments below - `lfo_trigger` is MODE, `lfo_fade` is RISE,
    `lfo_sync` carries the division, `lfo_rate` is RATE, `lfo_phase` is PHASE -
    and this block never learns what a parameter is. That is the same split the
    machine's architecture rule states: the processor computes every
    coefficient, the engine holds blocks only.
*/
#pragma once

#include "LfoPath.h"

#include <cmath>

namespace megasynth
{

/** WHAT A NEW NOTE DOES TO THE LFO. `[MAN]` p. 191, the MODE (Retrig) row:
    FREE "follows the host clock and ignores note timing"; RETRIG "retriggers
    the LFO, causing the LFO to start with a new note"; ENVELOPE is "similar to
    RETRIG however the LFO plays through a single cycle before stopping".

    Our own parameter calls these free / key / one-shot and its id is frozen, so
    the renaming happens in the coefficient builder and the manual's own names
    live here, next to the sentences they came from. */
enum PathLfoMode { kPathLfoFree = 0, kPathLfoRetrig = 1, kPathLfoEnvelope = 2 };

/** THE UNIT A SYNCED RATE ARRIVES IN: whole MIDI Timing Clocks per cycle, so a
    quarter is 24, an eighth is 12 and an eighth triplet is 8.

    MIDI HAS NO TEMPO MESSAGE. A master fires 0xF8 twenty-four times per quarter
    note and the tempo IS their rate, which is why a division is only ever a
    COUNT OF CLOCKS and why anything off that grid cannot be followed exactly.

    THE TABLE OF DIVISIONS IS NOT HERE AND MUST NOT BE COPIED HERE. It lives in
    `shell::TempoSync` in ~/projects/synthshell, which every machine of ours
    links, and `TempoSync::clocks (division)` produces exactly this argument
    (its table is in ticks at 192 per quarter, which is 8 per clock). A second
    copy of a division table is how two parts of one instrument end up
    disagreeing about what a dotted eighth is. What crosses into this file is
    the UNIT, not the vocabulary - and the unit is a double rather than an int
    because 1/64 is one and a half clocks and a machine with an internal divider
    follows it perfectly well. */
inline constexpr double kPathLfoClocksPerQuarter = 24.0;

/** ONE DRAWN LFO, PER VOICE.

    Every control is a transform ON the path, applied in the order the signal
    meets them: the accumulator decides WHERE on the path to read, delay and
    rise decide HOW MUCH of the path is heard yet, and smooth decides how fast
    the output is allowed to move. Nothing in here edits the path. */
struct PathLfo
{
    // ----------------------------------------------------------- preparation

    void prepare (double sampleRate) noexcept
    {
        sr = (sampleRate > 0.0 && std::isfinite (sampleRate)) ? sampleRate : 48000.0;
        // EVERY TIME IS RE-DERIVED HERE, and that is not tidiness. Delay, rise
        // and smooth are stated in seconds and used in samples, so a block whose
        // setters converted on the way in would hold numbers scaled to whatever
        // rate was current when the coefficient builder last ran - and a host
        // that changes sample rate under a loaded plugin does exactly that.
        updateTimes();
        reset();
    }

    /** Back to the top of everything: the start phase, no elapsed time, no
        smoothing history. A voice that is handed out fresh gets this. */
    void reset() noexcept
    {
        acc = startAccumulator();
        sinceTrigger = 0;
        hint = 0;
        done = false;
        smoothPrimed = false;
        smoothedX = smoothedY = 0.0f;
        updateIncrement();
    }

    // ---------------------------------------------------------- coefficients

    /** RATE in Hertz - `[MAN]` p. 192, the HZ half of BPM/HZ: the rate "can also
        be set to a frequency in Hertz (when HZ is enabled)". One Hz is one
        traverse of the graph per second, in every direction including ping pong
        (see setDirection). */
    void setFreeRateHz (double hz) noexcept
    {
        freeHz = (std::isfinite (hz) && hz > 0.0) ? hz : 0.0;
        updateIncrement();
    }

    /** RATE locked to the clock - `[MAN]` p. 192: "The LFO rate is in
        beat-synced units by default (when BPM is enabled)."

        THE DIVISION AND THE TEMPO ARRIVE TOGETHER, IN ONE CALL, AND THAT IS THE
        WHOLE POINT OF THE SIGNATURE. Nine effects units in this workspace
        shipped reading 500 ms at 120 BPM and still 500 ms at 90, with every
        test green, because the division reached the engine and the tempo never
        did. A setter for the division alone is the shape of code that produces
        that fault, so there is not one: you cannot set a division here without
        stating the tempo it is to be measured at. CALL THIS EVERY BLOCK with
        the LIVE tempo, not only when the division changes.

        `clocksPerCycle` of 0 means sync is off and the free rate is used again,
        which mirrors `shell::TempoSync::off` reporting zero ticks - `beats(d) >
        0` is the is-it-synced test everywhere in the shell and `clocks > 0` is
        the same test here. */
    void setSyncedRate (double clocksPerCycle, double beatsPerMinute) noexcept
    {
        syncClocks = (std::isfinite (clocksPerCycle) && clocksPerCycle > 0.0)
                     ? clocksPerCycle : 0.0;
        // The same floor the shell puts under a tempo, so a synced LFO and a
        // synced delay cannot disagree about what an absurd transport means.
        bpm = (std::isfinite (beatsPerMinute) && beatsPerMinute > 20.0)
              ? beatsPerMinute : 20.0;
        updateIncrement();
    }

    /** DIRECTION - `[MAN]` p. 191: Forward, Reverse, Ping Pong.

        PING PONG RUNS THE ACCUMULATOR AT HALF SPEED, and this is the line that
        the whole direction question turns on. A ping-pong cycle is the graph
        read forwards and then backwards, which is TWICE the work, so at a given
        RATE it must take twice the time - the graph is traversed at the same
        speed you set, there are simply two traverses in a cycle. Fold the
        return journey into one accumulator turn instead and the traverse runs
        twice as fast the moment anybody chooses ping pong, which measures as a
        rate that is wrong rather than as a direction that is wrong. */
    void setDirection (int dir) noexcept
    {
        direction = (dir >= kPathForward && dir <= kPathPingPong) ? dir : kPathForward;
        updateIncrement();
    }

    void setMode (int m) noexcept
    {
        mode = (m >= kPathLfoFree && m <= kPathLfoEnvelope) ? m : kPathLfoFree;
    }

    /** READ THE PATH AS A FIGURE - `[MAN]` p. 191, the Path type - rather than
        as a graph. The clock is the same either way; what changes is that
        `tickXY` walks the point list in order and the loopback point is an
        INDEX into it rather than a time. Set once a block from the type. */
    void setFigure (bool on) noexcept { figure = on; }

    /** WHERE THE RISE STARTS FROM. `[MAN]` p. 192 says the fixed output is
        "the value of the left-most point" of the graph, and that is what the
        Serum types do. This machine's own LFO ([FW] JP-8000, `Coeffs::lfoFade`)
        fades in from the CENTRE - a gain on the wobble, not a morph out of a
        level - and the factory bank and tools/lfo_probe.cpp are voiced against
        exactly that. So type Normal keeps the JP-8000 law and says so here; the
        other types take the manual's. Two laws under one knob, both written
        down, rather than one law that re-voices a bank in silence. */
    void setRiseFromCentre (bool on) noexcept { riseFromCentre = on; }

    /** PHASE - `[MAN]` p. 193: "Sets the start position of the LFO phase."

        A START POSITION AND NOT A RUNNING OFFSET. It is applied when a cycle
        begins - a note in RETRIG and ENVELOPE, a transport anchor in FREE - and
        never again on the way out, because an offset added at both ends is
        applied twice and the shape then moves whenever anything re-anchors.

        It is measured in TRAVERSES, like the rate, so a quarter turn means a
        quarter of the way into the graph in all three directions. */
    void setPhaseOffset (double turns) noexcept
    {
        phaseOffset = std::isfinite (turns) ? turns - std::floor (turns) : 0.0;
    }

    /** DELAY - `[MAN]` p. 193: "The amount of time before the rise begins. The
        LFO has a fixed output, as described above. After the delay time period,
        the rise begins." */
    void setDelaySeconds (double s) noexcept
    {
        delaySec = (std::isfinite (s) && s > 0.0) ? s : 0.0;
        updateTimes();
    }

    /** RISE - `[MAN]` p. 192: "The amount of time for the LFO graph shape to
        have influence over the LFO output. The LFO begins with a fixed output
        (imagine the LFO graph as a fat horizontal line, with the value of the
        left-most point of the [...] graph) and slowly (based on the rise time)
        becoming the shape of the visible graph." */
    void setRiseSeconds (double s) noexcept
    {
        riseSec = (std::isfinite (s) && s > 0.0) ? s : 0.0;
        updateTimes();
    }

    /** SMOOTH - `[MAN]` p. 193: "Smooth the LFO output. This is useful for
        avoiding abrupt jumps in the LFO output, without having to draw ramps on
        every segment of the LFO graph."

        IN MILLISECONDS, because a slew is a time. The panel's knob is a
        percentage and the mapping from one to the other is the coefficient
        builder's business, the same way every other range in this machine is. */
    void setSmoothMs (double ms) noexcept
    {
        smoothMs = (std::isfinite (ms) && ms > 0.0) ? ms : 0.0;
        updateTimes();
    }

    // ------------------------------------------------------------------ play

    /** A NEW NOTE.

        `[MAN]` p. 191: FREE "ignores note timing", so a note must not move the
        phase in that mode - only in RETRIG and ENVELOPE does a cycle start
        here.

        THE DELAY AND THE RISE RESTART IN ALL THREE MODES, INCLUDING FREE, and
        that is deliberate. It is the rule `Lfo.h` already states for this
        machine from the JP-8000: one phase for the whole part but one fade per
        voice, so a chord's movement is one gesture while a note added to it
        still swims INTO the wobble already running. A free-running LFO whose
        rise restarted for every voice at once would be a fade with a phase, not
        a phase with a fade. */
    void retrigger() noexcept
    {
        if (mode != kPathLfoFree) acc = startAccumulator();
        sinceTrigger = 0;
        done = false;
        hint = 0;
        // The smoother re-primes rather than gliding out of the last note's
        // value: a fresh voice has no history, and one that inherited a
        // neighbour's would open with a sweep nobody asked for.
        smoothPrimed = false;
    }

    /** RE-ANCHOR TO THE TRANSPORT - `[MAN]` p. 192, HOST: "When HOST is
        enabled, the phase is 'anchored' to the host transport position."

        `turns` is the position in CYCLES OF THE RATE, which is what
        `shell::MidiClock::phase (division)` hands back, and the PHASE knob is
        added here so an anchored LFO still honours it. Call it at the START of
        a block: the clock's own `phase()` reports the block's start for exactly
        this reason, and anchoring to where the block ENDS puts every synced
        modulator one block past the beat forever. */
    void anchorTo (double turns) noexcept
    {
        const double t = std::isfinite (turns) ? turns : 0.0;
        acc = wrap01 ((t + phaseOffset) * traverseToAccumulator());
    }

    /** ONE SAMPLE, 0..1 out, from the path handed in, read as a GRAPH. */
    [[nodiscard]] float tick (const LfoPath& path) noexcept
    {
        // A PATH ARRIVES FROM THE EDITOR, FROM A PROGRAM AND FROM A BANK FILE A
        // HUMAN MAY HAVE TYPED INTO, and only the first of those is under our
        // control. `LfoPath::tidy()` is what puts a path back inside its own
        // rules, but nothing in this block can force it to have been called, so
        // a count outside the array's bounds is refused here rather than read.
        const bool usable = path.count >= 2 && path.count <= kMaxPathPoints;

        // THE FIXED OUTPUT IS THE LEFT-MOST POINT, LITERALLY, and in every
        // direction. `[MAN]` p. 192 says "the value of the left-most point",
        // and it says it about the graph rather than about the playback - so a
        // REVERSE LFO, which starts its traverse at the right, still delays and
        // rises out of the value on the left. That reads like a bug and is what
        // the manual states; changing it to "wherever playback starts" is the
        // easy correction to make and would put us a long way from Serum.
        const float held = safe01 (usable && ! riseFromCentre ? path.p[0].y : 0.5f);
        const float live = usable ? safe01 (path.valueAt (pathPhase(), &hint)) : held;

        // THE RISE IS A MORPH BETWEEN THE FLAT LINE AND THE SHAPE, which is the
        // manual's own picture - the graph "as a fat horizontal line [...]
        // slowly becoming the shape of the visible graph" - rather than a gain
        // on the output. The two are not the same: a gain would fade the LFO in
        // from 0, which is a different value and usually a different DESTINATION
        // setting, while a morph starts at the left-most point and stays there.
        //
        // LINEAR, and that is `[EAR]`: no document states the curve. Linear is
        // chosen because its endpoints are exact - the delay ends and the rise
        // arrives on a sample you can name - and every curve we could put here
        // instead would blur the one property the probe can measure.
        float v = safe01 (held + (live - held) * riseAmount());
        v = slew (smoothedY, v);
        advance (path);
        return v;
    }

    /** ONE SAMPLE, read as a FIGURE: where the LFO is, x and y both 0..1.

        The same clock, the same delay, rise and slew as `tick`, applied to two
        coordinates. The fixed output the rise climbs out of is the FIRST POINT
        of the figure - the manual's "left-most point" read in the only way a
        figure has a first point - and the slew runs on each axis separately,
        so a square figure with SMOOTH on rounds its corners rather than
        cutting them. */
    [[nodiscard]] PathXY tickXY (const LfoPath& path) noexcept
    {
        const bool usable = path.count >= 2 && path.count <= kMaxPathPoints;
        PathXY held { 0.5f, 0.5f };
        if (usable && ! riseFromCentre) held = { safe01 (path.p[0].x), safe01 (path.p[0].y) };
        PathXY live = held;
        if (usable)
        {
            const PathXY q = path.positionAt (pathPhase(), &hint);
            live = { safe01 (q.x), safe01 (q.y) };
        }
        const float r = riseAmount();
        PathXY v { safe01 (held.x + (live.x - held.x) * r),
                   safe01 (held.y + (live.y - held.y) * r) };
        // PRIMED ONCE FOR BOTH AXES: the prime is a property of the note, not
        // of a coordinate, and priming them separately would let one axis glide
        // out of a neighbour's value while the other started clean.
        const bool wasPrimed = smoothPrimed;
        v.x = slew (smoothedX, v.x);
        smoothPrimed = wasPrimed;
        v.y = slew (smoothedY, v.y);
        advance (path);
        return v;
    }

    // ---------------------------------------------------------------- report

    /** The rate the block is actually running at, in traverses per second. The
        panel's readout and the sync probe both want this, and it is the one
        number that proves a tempo reached the engine. */
    [[nodiscard]] double rateHz() const noexcept { return liveHz; }

    /** The accumulator, 0..1 - one turn is one CYCLE, which in ping pong is two
        traverses. */
    [[nodiscard]] double cyclePhase() const noexcept { return acc; }

    /** WHERE THE PLAYHEAD SITS ON THE GRAPH, 0..1, which is what the editor
        draws and what `valueAt` is read with. */
    [[nodiscard]] float pathPhase() const noexcept
    {
        // THE END OF A CYCLE IS 1, NOT THE 0 IT WOULD WRAP TO. `directedPhase`
        // takes the fractional part, so an accumulator sitting exactly at 1
        // reads as the START of the graph; a stopped envelope would then hold
        // the value it began with instead of the one it ended on. `Lfo.h` has
        // the same note about its one-shot, and it is the same fault: the
        // difference between a gesture that stays up and one that stays down.
        if (acc >= 1.0) return direction == kPathForward ? 1.0f : 0.0f;
        return LfoPath::directedPhase ((float) acc, direction);
    }

    /** HOW MUCH OF THE SHAPE IS HEARD, 0..1: 0 through the delay, climbing
        across the rise, 1 after it. The editor draws the fat line fading into
        the curve with this. */
    [[nodiscard]] float riseAmount() const noexcept
    {
        const double t = (double) sinceTrigger;
        if (t < delaySamples) return 0.0f;
        if (riseSamples <= 0.0) return 1.0f;
        const double u = (t - delaySamples) / riseSamples;
        return u >= 1.0 ? 1.0f : (float) u;
    }

    /** ENVELOPE MODE HAS PLAYED THROUGH AND STOPPED. Always false in the other
        two modes and false while a loopback point is cycling. */
    [[nodiscard]] bool finished() const noexcept { return done; }

    /** A DIVISION AND A TEMPO INTO A RATE. A quarter (24 clocks) at 120 BPM is
        2 Hz, because a quarter at 120 BPM is 500 ms - the number every clock
        test in this workspace is measured against. */
    [[nodiscard]] static double hzForClocks (double clocksPerCycle, double beatsPerMinute) noexcept
    {
        if (! (clocksPerCycle > 0.0) || ! std::isfinite (clocksPerCycle)) return 0.0;
        return (beatsPerMinute / 60.0) * (kPathLfoClocksPerQuarter / clocksPerCycle);
    }

    /** THE ONE PLACE A VALUE IS MADE SAFE. It catches NaN by asking whether the
        value is above zero rather than whether it is below - `! (v > 0)` is
        true for a NaN and a comparison the other way round is not. One sample
        of NaN travels the whole chain to the DAC and does not come back. */
    [[nodiscard]] static float safe01 (float v) noexcept
    {
        if (! (v > 0.0f)) return 0.0f;
        return v > 1.0f ? 1.0f : v;
    }

private:
    [[nodiscard]] static double wrap01 (double v) noexcept
    {
        if (! std::isfinite (v)) return 0.0;
        return v - std::floor (v);
    }

    /** Traverses to accumulator turns: a half in ping pong, because a cycle
        there is two traverses. Every phase that crosses this block's interface
        is in traverses, and this is the only place the two differ. */
    [[nodiscard]] double traverseToAccumulator() const noexcept
    {
        return direction == kPathPingPong ? 0.5 : 1.0;
    }

    [[nodiscard]] double startAccumulator() const noexcept
    {
        return wrap01 (phaseOffset * traverseToAccumulator());
    }

    void updateTimes() noexcept
    {
        delaySamples = delaySec * sr;
        riseSamples  = riseSec * sr;
        // THE SLEW IS A ONE-POLE, and the alternative is not available at any
        // price: a smoother that did not lag would have to see the future, and
        // an LFO reading a path a player is dragging has no future to see. What
        // a one-pole does give is the property that matters - it starts moving
        // on the SAME SAMPLE the step does, so SMOOTH softens a step without
        // moving where the step is, which is what the probe asserts.
        //
        // A coefficient of 1 is OFF, and off is exact rather than merely fast:
        // the output is the input, with no state and nothing to converge.
        const double t = smoothMs * 0.001 * sr;
        smoothG = t > 0.0 ? (float) (1.0 - std::exp (-1.0 / t)) : 1.0f;
        if (! (smoothG > 0.0f)) smoothG = 1.0f;
        if (smoothG > 1.0f) smoothG = 1.0f;
    }

    void updateIncrement() noexcept
    {
        const double hz = syncClocks > 0.0 ? hzForClocks (syncClocks, bpm) : freeHz;
        liveHz = (std::isfinite (hz) && hz > 0.0) ? hz : 0.0;
        inc = (liveHz / sr) * traverseToAccumulator();
        if (! std::isfinite (inc) || inc < 0.0) inc = 0.0;
    }

    /** WHERE THE ACCUMULATOR JUMPS BACK TO when an envelope reaches the end, or
        a negative number for "hold the last value".

        `[MAN]` p. 191: the loopback point makes the LFO "play through, then
        cycle back to the selected loopback point", and p. 191's shortcut row
        says the very LAST point means no loopback position at all. That is by
        INDEX and not by position, which is why the test below is on the index.

        `[EAR]` for reverse and ping pong, which the guide does not discuss. The
        loop target is kept in the ACCUMULATOR's domain so one line covers all
        three: reverse reaches the loopback point when the accumulator is at
        1 - x, and ping pong reaches it halfway through its outward traverse.
        Both then replay from the loopback point to the end of the cycle, which
        is what the forward case does too. */
    [[nodiscard]] double loopAccumulator (const LfoPath& path) const noexcept
    {
        const int lb = path.loopback;
        if (lb < 0 || lb >= path.count || lb == path.count - 1) return -1.0;
        // A figure's loopback is a place in the LIST, so its phase is the index
        // over the segment count - the same share of the cycle `positionAt`
        // gives that point. A graph's is the point's own time.
        const double x = figure ? (double) lb / (double) (path.count - 1)
                                : (double) path.p[lb].x;
        switch (direction)
        {
            case kPathReverse:  return 1.0 - x;
            case kPathPingPong: return x * 0.5;
            default:            return x;
        }
    }

    void advance (const LfoPath& path) noexcept
    {
        ++sinceTrigger;
        if (done) return;
        acc += inc;
        if (acc < 1.0) return;

        if (mode == kPathLfoEnvelope)
        {
            // `[MAN]` p. 191: ENVELOPE "plays through a single cycle before
            // stopping". Reaching the end of the graph is what stops it, which
            // is also what the playhead running off the right of the display
            // shows - so a start phase makes a SHORTER envelope rather than a
            // displaced one. `[EAR]`, since the guide states the cycle and not
            // where a phase offset leaves it.
            const double loop = loopAccumulator (path);
            const double span = 1.0 - loop;
            if (loop < 0.0 || span <= 1.0e-9) { acc = 1.0; done = true; return; }
            // The overshoot is carried into the loop rather than dropped, so a
            // looped segment keeps the rate exactly instead of losing a
            // fraction of a sample every time round.
            acc = loop + std::fmod (acc - loop, span);
            return;
        }
        acc -= std::floor (acc);
    }

    [[nodiscard]] float slew (float& smoothed, float v) noexcept
    {
        if (! smoothPrimed) { smoothed = v; smoothPrimed = true; return v; }
        if (smoothG >= 1.0f) { smoothed = v; return v; }
        smoothed = safe01 (smoothed + (v - smoothed) * smoothG);
        return smoothed;
    }

    double sr = 48000.0;

    // the rate, and the two ways of stating it
    double freeHz = 1.0;        ///< the HZ knob
    double syncClocks = 0.0;    ///< MIDI clocks per cycle, 0 when sync is off
    double bpm = 120.0;         ///< the LIVE tempo, restated on every synced call
    double liveHz = 1.0;        ///< what the two above resolve to
    double inc = 0.0;           ///< accumulator turns per sample

    // where we are
    double acc = 0.0;
    double phaseOffset = 0.0;
    int64_t sinceTrigger = 0;
    int hint = 0;               ///< the segment `valueAt` landed in last; only a hint

    // the fade in, held in seconds and used in samples
    double delaySec = 0.0, delaySamples = 0.0;
    double riseSec = 0.0,  riseSamples = 0.0;

    // the slew
    double smoothMs = 0.0;
    float smoothG = 1.0f;       ///< 1 is off, and off is exact rather than fast
    float smoothedX = 0.0f, smoothedY = 0.0f;   ///< one state per axis; a graph uses Y
    bool  smoothPrimed = false;

    int  direction = kPathForward;
    int  mode = kPathLfoFree;
    bool done = false;
    bool figure = false;         ///< read as a figure (type Path) rather than a graph
    bool riseFromCentre = false; ///< the JP-8000 fade law, for type Normal
};

} // namespace megasynth
