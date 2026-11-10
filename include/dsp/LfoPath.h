/*  MegaSynth - a drawable LFO path.

    The author, sending Serum's LFO editor: *"honestly why dont we concentrate on
    building the exact same LFO system as serum has? Its a way way better LFO
    that we have. You have complete control over everything."*

    He is right, and the difference is not a longer list of waveforms. A shape
    selector offers five curves and nothing between them; a PATH is the shape
    itself - a list of points you place, with a bend on the segment between each
    pair - so an LFO can be a ramp, a staircase, a swung pulse, a pitch sequence,
    or a curve nobody has a name for. Everything else in the module (rate, rise,
    delay, smooth, phase, direction) is a transform ON the path, which is why the
    path comes first and alone in this file.

    WHAT THIS FILE IS NOT. It holds no JUCE, no parameters, no editing and no
    drawing: it is the shape and how to read it. The editor that draws points is
    in the panel package, the parameters are generated, and the per-voice state
    lives in the engine. Keeping them apart is what lets the shape be tested by
    arithmetic rather than by ear or by screenshot.

    Provenance: `[MAN]` for the behaviours the Serum 2 user guide states
    outright - the three trigger modes, the three directions, the loopback point,
    what RISE and DELAY do (pp. 188-193, docs/serum-guide/) - and `[EAR]` for the
    curve law, which no document states and which is fitted to the shape the
    handle draws.
*/
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace megasynth
{

/** How many points one path may hold.

    THIRTY-TWO IS A CHOICE ABOUT THE PATCH, not about the editor. The path is
    stored in a program, and a program is a row of a bank file that has to stay
    diffable and hand-editable - this repo has already lost a day to a bank the
    tooling corrupted. Thirty-two points is a sixteen-step sequence with a point
    at each end of every step, which is the densest thing anybody draws by hand,
    and it keeps a path under 200 characters in the bank. */
inline constexpr int kMaxPathPoints = 32;

/** HOW MANY DRAWABLE LFOs THE MACHINE HAS. [MAN] Serum 2 user guide p. 188:
    "Serum offers ten low frequency oscillators... When you initialize a patch,
    only LFO 1 to LFO 6 are visible." Six is what a new patch shows and six is
    what the author asked for - "we should have 6 lfos. you can get so much control that
    way over your sound" - and the number is the only thing that changes to go
    further. */
inline constexpr int kNumPathLfos = 6;

/** One point on the path, and the bend of the segment that LEAVES it.

    THE CURVE BELONGS TO THE SEGMENT, AND THE SEGMENT IS NAMED BY ITS LEFT POINT.
    Serum draws the bend handle at the middle of a segment, which reads as if the
    curve were a thing between two points; storing it on the left point instead
    means a path is one array with no second list to keep in step, and deleting a
    point cannot leave a curve behind with nothing to bend. */
struct PathPoint
{
    float x = 0.0f;      ///< position in the cycle, 0..1, ascending - or, when the
                         ///< path is read as a FIGURE, a free horizontal coordinate
    float y = 0.5f;      ///< level, 0..1; 0.5 is the centre line the display draws
    float curve = 0.0f;  ///< bend of the segment leaving this point, -1..+1, 0 straight
};

/** WHERE THE LFO IS ON A FIGURE: two coordinates, both 0..1. A Path-type LFO
    ([MAN] p. 191) puts out BOTH, as its X and its Y sources. */
struct PathXY
{
    float x = 0.5f;
    float y = 0.5f;
};

/** THE CURVE LAW. `[EAR]` - no document states it, and it is fitted to the shape
    the handle draws rather than recovered from anything.

    A segment runs from t = 0 to t = 1 and the bend warps the TIME, not the
    level: that is what keeps both ends exactly on their points however hard it
    is bent, which is the property that makes a drawn path predictable. Positive
    is slow-then-fast (ease in), negative fast-then-slow (ease out), and the two
    are mirror images so that bending a segment one way and back returns the
    identical shape rather than something a rounding error away from it.

    THE EXPONENT IS BOUNDED. An unbounded power turns a segment into a step and a
    step drawn by accident is a click; 5 is the steepest bend that still reads as
    a curve on a 90 px display.

    THIS IS THE GRAPH'S LAW ONLY. When the same points are read as a FIGURE the
    bend is geometry rather than timing - see `kFigureBulge` and `positionAt`. */
[[nodiscard]] inline float pathCurve (float t, float curve) noexcept
{
    if (curve > -1.0e-4f && curve < 1.0e-4f) return t;
    const float k = 1.0f + std::fabs (curve) * 4.0f;      // 1 .. 5
    return curve > 0.0f ? std::pow (t, k)
                        : 1.0f - std::pow (1.0f - t, k);
}

/** THE FIGURE'S BEND LAW. `[EAR]` - the guide shows a Path LFO whose segments
    are curved lines with a hollow handle sitting OFF the chord
    (serum2_screenshots/lfos.png), and states no formula.

    A segment of a figure is a quadratic curve: its two points and one control
    point pushed off the chord's middle, perpendicular to it, by `curve` times
    this fraction of the chord's length. Positive bulges to the LEFT of the
    direction of travel, negative to the right, and 0 is the straight chord. The
    handle the editor draws sits ON the curve at its parametric middle, which is
    half the control point's offset - so a handle dragged off the chord reads
    back a curve of twice its own displacement, and the panel's copy of this law
    (synthkit NativePanel::figurePoint) has to agree with it to the number or a
    figure SOUNDS different from the picture that was drawn.

    0.75 puts a fully bent segment's handle three eighths of the chord's length
    off the chord, which is about the deepest bow in the screenshot and is still
    a curve rather than a loop. What would replace it: a measurement of Serum's
    own handle travel against its chord, which no document gives. */
inline constexpr float kFigureBulge = 0.75f;

/** WHICH WAY THE PATH IS READ. `[MAN]` Serum 2 user guide p. 191, DIRECTION. */
enum PathDirection { kPathForward = 0, kPathReverse = 1, kPathPingPong = 2 };

/** A drawable LFO shape: a list of points, and how to read one value out of it.

    ONE LIST, TWO READINGS. Read as a GRAPH (type Normal) a point's x is a time
    in the cycle and `valueAt` returns the level at a phase. Read as a FIGURE
    (type Path) the same points are free positions on the display, the ORDER of
    the list is the order the LFO visits them, and `positionAt` returns where
    the LFO is. The author, on Serum's tabs: "the tabs has X and Y connections when not
    in NORMAL and SH mode" - a figure is two sources, and this is the structure
    that makes it so without a second store or a second serialisation.

    INVARIANTS, and they are enforced by `tidy()` rather than assumed:
      - at least two points;
      - every x, y in 0..1 and every curve in -1..+1;
      - read as a graph only: x ascending, with the first at 0 and the last at
        1, so the cycle is closed and a phase of 0.999 and a phase of 0.001 are
        neighbours. `tidy (true)` keeps the order instead, for a figure, whose
        traversal order IS the point order and must not be sorted by x.

    A path that violated any of these would not crash - it would read a segment
    of negative width and hand back a value from somewhere else in the cycle,
    which is exactly the class of fault that is inaudible until it is a click in
    somebody's mix. `valueAt` clamps its way through an unsorted list rather
    than trusting the caller, because the type can change under a held note and
    a figure read as a graph for one block must not put out a NaN. */
struct LfoPath
{
    PathPoint p[kMaxPathPoints] {};
    int count = 2;

    /** WHERE ENVELOPE MODE JUMPS BACK TO once it has played through.

        `[MAN]` p. 191: *"It's possible to loop a segment of the LFO while in
        envelope mode using the loopback point... This causes the LFO to play
        through, then cycle back to the selected loopback point."* A negative
        value means no loopback: the LFO holds its last value, which is the
        behaviour that makes ENVELOPE an envelope. */
    int loopback = -1;

    /** The shape every new LFO starts as: one full cycle of a triangle, which is
        two segments and reads instantly as an LFO. A sine would need curves on
        both segments to approximate and would be a worse thing to start editing
        from - a beginner's first drag on a triangle does what they expect. */
    static LfoPath triangle() noexcept
    {
        LfoPath l;
        l.count = 3;
        l.p[0] = { 0.0f, 0.0f, 0.0f };
        l.p[1] = { 0.5f, 1.0f, 0.0f };
        l.p[2] = { 1.0f, 0.0f, 0.0f };
        return l;
    }

    /** A sine, as four curved quarters. Serum's SHAPE menu opens on one and it
        is what most players reach for, so it is worth having exactly rather than
        as a triangle with the corners knocked off. */
    static LfoPath sine() noexcept
    {
        LfoPath l;
        l.count = 5;
        // FOUR QUARTERS, NOT TWO HALVES. A sine is not a half-cycle repeated: it
        // rises, falls to the centre, falls again and comes back, and drawing it
        // as two segments gives a shape that never goes below the centre line at
        // all - which measured 0.60 out of a possible 1.0 against a real sine.
        //
        // THE BEND IS SOLVED, not eyeballed. `pathCurve(t, -c)` is 1 - (1-t)^k
        // with k = 1 + 4c, and matching it to sin(t*pi/2) at the halfway point
        // gives k = ln(0.2929)/ln(0.5) = 1.7716, so c = (k - 1)/4. Measured
        // against a true sine across the whole cycle the worst error is 1.1 % -
        // closer than a 90 px display can show, and tools/path_probe.cpp asserts
        // it rather than trusting it.
        //
        // The quarters alternate: a quarter that leaves a peak or a trough eases
        // IN, and one that arrives at the centre line eases OUT.
        constexpr float k = 0.1929f;
        l.p[0] = { 0.00f, 0.5f, -k };
        l.p[1] = { 0.25f, 1.0f,  k };
        l.p[2] = { 0.50f, 0.5f, -k };
        l.p[3] = { 0.75f, 0.0f,  k };
        l.p[4] = { 1.00f, 0.5f,  0.0f };
        return l;
    }

    /** PUT THE PATH BACK INSIDE ITS OWN RULES. Called after any edit and after
        loading, because a path arrives from three places - the editor, a saved
        program, and a bank file somebody may have typed into - and only one of
        those is under our control. */
    /** @param keepOrder true for a FIGURE: the points are clamped and nothing
                         else, because the order of the list is the traversal
                         and x is a free coordinate rather than a time. The
                         processor is what knows the LFO's type, so it is the
                         processor that says which; the engine's own setLfoPath
                         keeps the order it is handed. */
    void tidy (bool keepOrder = false) noexcept
    {
        count = std::clamp (count, 2, kMaxPathPoints);
        for (int i = 0; i < count; ++i)
        {
            p[i].x = std::clamp (p[i].x, 0.0f, 1.0f);
            p[i].y = std::clamp (p[i].y, 0.0f, 1.0f);
            p[i].curve = std::clamp (p[i].curve, -1.0f, 1.0f);
        }
        if (keepOrder) { if (loopback >= count) loopback = -1; return; }
        std::stable_sort (p, p + count,
                          [] (const PathPoint& a, const PathPoint& b) { return a.x < b.x; });
        p[0].x = 0.0f;
        p[count - 1].x = 1.0f;
        // NO ZERO-WIDTH SEGMENTS. Two points at the same x is a vertical jump,
        // which is a legitimate thing to draw (a step sequencer is made of them)
        // - but the READER has to divide by the segment's width, so the width is
        // floored rather than the points being moved. A floor of 1e-5 of a cycle
        // is under a tenth of a sample at any rate this LFO can run.
        if (loopback >= count) loopback = -1;
    }

    /** THE VALUE AT A PHASE, 0..1 in and 0..1 out.

        Linear from the left, not a binary search: a path has at most 32 points
        and this is called once per sample per voice, so the branch predictor
        beats the algorithm at this size. `from` is a hint - the segment the last
        call landed in - which makes the common case (phase advancing a little)
        a single comparison. */
    [[nodiscard]] float valueAt (float phase, int* from = nullptr) const noexcept
    {
        const float ph = std::clamp (phase, 0.0f, 1.0f);
        int i = 0;
        if (from != nullptr && *from >= 0 && *from < count - 1
            && p[*from].x <= ph && ph <= p[*from + 1].x)
            i = *from;
        else
        {
            while (i < count - 2 && p[i + 1].x <= ph) ++i;
            if (from != nullptr) *from = i;
        }
        const float x0 = p[i].x, x1 = p[i + 1].x;
        const float w = x1 - x0;
        if (w < 1.0e-5f) return p[i + 1].y;             // a vertical jump reads as its top
        // CLAMPED, not trusted. A figure whose points are not in time order can
        // be read here for the block between a type change and the processor's
        // re-tidy, and `pathCurve` of a t outside 0..1 is a pow of a negative
        // number, which is a NaN on its way to the DAC.
        const float t = pathCurve (std::clamp ((ph - x0) / w, 0.0f, 1.0f), p[i].curve);
        return p[i].y + (p[i + 1].y - p[i].y) * t;
    }

    /** THE POSITION AT A PHASE, read as a FIGURE. `[MAN]` p. 191 lists Path
        among the types and the screenshot shows what it is: a shape drawn in
        two dimensions that the LFO travels along, once per cycle.

        Parametric by SEGMENT: a phase of 0 is the first point, a phase of 1 the
        last, and the count - 1 segments each take an equal share of the cycle
        whatever their length. `[EAR]` - equal time per segment rather than
        constant speed along the curve, because it is what makes a figure
        editable: moving one point changes how fast that segment is crossed and
        nothing about the timing of the others, which is the property a
        sequence drawn as a figure depends on. The figure is OPEN: the cycle
        ends on the last point and the next one starts on the first, and SMOOTH
        is what softens the jump when the two are not the same place - draw the
        last point on the first to close it.

        Each segment is the quadratic curve `kFigureBulge` describes; `from` is
        the segment hint `valueAt` also takes. */
    [[nodiscard]] PathXY positionAt (float phase, int* from = nullptr) const noexcept
    {
        const int segs = count - 1;
        if (segs < 1) return { p[0].x, p[0].y };
        const float ph = std::clamp (phase, 0.0f, 1.0f);
        const float u = ph * (float) segs;
        int i = (int) u;
        if (i > segs - 1) i = segs - 1;
        if (i < 0) i = 0;
        if (from != nullptr) *from = i;
        const float t = std::clamp (u - (float) i, 0.0f, 1.0f);
        return figurePoint (p[i], p[i + 1], t);
    }

    /** ONE SEGMENT OF A FIGURE at parameter t, 0..1: the quadratic curve from
        `a` to `b` bulged by `a.curve` - see kFigureBulge. Static so the probe
        and the reader read one function. */
    [[nodiscard]] static PathXY figurePoint (const PathPoint& a, const PathPoint& b, float t) noexcept
    {
        const float cx = b.x - a.x, cy = b.y - a.y;           // the chord
        const float mx = (a.x + b.x) * 0.5f, my = (a.y + b.y) * 0.5f;
        // The control point: off the chord's middle, along its LEFT normal, by
        // curve * bulge * |chord|. The normal of (cx, cy) is (-cy, cx), whose
        // length is |chord| already, so the scale is the bare product.
        const float k = a.curve * kFigureBulge;
        const float px = mx - cy * k, py = my + cx * k;
        const float s = 1.0f - t;
        return { s * s * a.x + 2.0f * s * t * px + t * t * b.x,
                 s * s * a.y + 2.0f * s * t * py + t * t * b.y };
    }

    /** THE PHASE THE DIRECTION ACTUALLY READS.

        `[MAN]` p. 191. Forward is the phase itself; Reverse is the cycle read
        backwards; Ping Pong is one cycle forward and one back, which takes TWICE
        the time - so it is computed from a phase that has already been doubled
        by the caller rather than by folding here, or the rate would silently
        halve the moment you chose it. */
    [[nodiscard]] static float directedPhase (float phase, int direction) noexcept
    {
        const float ph = phase - std::floor (phase);
        switch (direction)
        {
            case kPathReverse:  return 1.0f - ph;
            case kPathPingPong: return ph < 0.5f ? ph * 2.0f : 2.0f - ph * 2.0f;
            default:            return ph;
        }
    }
};

} // namespace megasynth
