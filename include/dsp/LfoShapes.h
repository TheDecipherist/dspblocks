/*  MegaSynth - the LFO shape library: the presets behind the SHAPE menu.

    The author, pointing at Serum's SHAPE menu: *"there is literally countless
    different patterns."* A drawable path is only as useful as the shapes you
    can START from, and drawing a sixteen-step trance gate by hand is thirty
    clicks nobody makes twice. So the library holds them, built in code as
    `LfoPath` values, and choosing one LOADS it into the graph - which the
    player then edits like anything else they drew.

    THE STRUCTURE IS THE GUIDE'S, THE SHAPES AND THE NAMES ARE OURS.
    `[MAN]` docs/serum-guide/15-exploring-sound-modulation.md, SHAPE: *"a
    pop-up menu allowing you to load an LFO preset. Note that this overwrites
    the current LFO graph. The menu also offers the ability to save the current
    LFO graph as a user-defined preset."* The menu the screenshot shows
    (serum2_screenshots/individual_shots/lfo_presets_menu.png) is: Default;
    Basic >; Rhythmic >; Sidechain >; Random (Curved); Random (Flat);
    Wavetable A/B/C to LFO; Save Shape. We keep that structure because it is
    how a player already thinks about these shapes - a waveform, a rhythm, a
    pump, or a throw of the dice. The shapes themselves are geometry on a bar
    grid (a gate is a gate) and cannot belong to anyone; the NAMES are Xfer's,
    so every name here is `[EAR]` and our own.

    ONE CYCLE IS ONE BAR. Every Rhythmic and Sidechain shape is drawn so that
    the path's 0..1 is a bar of 4/4: a shape called "Backbeat" has its hits at
    0.25 and 0.75, and it is right when the LFO's rate is 1 bar. That is a
    convention, not a law, and it is the only one that lets the shapes name
    beats at all.

    WHAT THIS FILE IS NOT. No JUCE, no parameters, no menu: it is the shapes
    and how to find one. The editor's `applyShape` loads one into a graph, the
    panel builds the menu from `shapeNames()`, and tools/shapes_probe.cpp checks
    every shape by arithmetic. Building them in code rather than in a data file
    is deliberate: a shape is a dozen numbers with a REASON behind each, and the
    reason lives beside the number here where a JSON row could not carry it.
*/
#pragma once

#include "LfoPath.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace megasynth {
namespace shapes {

/** THE MENU'S SUBMENUS. `[MAN]` Basic, Rhythmic and Sidechain are the three
    submenus of fixed shapes the guide's menu shows; Random is two GENERATORS
    below them (randomCurved / randomFlat) and holds no fixed entry, which is
    why it is not a category here. A menu builder that wants the word gets it
    from categoryName(). */
enum ShapeCategory { kShapeBasic = 0, kShapeRhythmic = 1, kShapeSidechain = 2, kNumShapeCategories = 3 };

[[nodiscard]] inline const char* categoryName (ShapeCategory c) noexcept
{
    switch (c)
    {
        case kShapeBasic:     return "Basic";
        case kShapeRhythmic:  return "Rhythmic";
        case kShapeSidechain: return "Sidechain";
        default:              return "";
    }
}

/** One library entry.

    `grid` is the bar grid the shape is drawn on - 4, 8, 12 or 16 cells per
    cycle - and 0 for a shape that has none (the Basic waveforms). It is stored
    rather than inferred so the probe can hold every Rhythmic shape to its own
    grid: a gate edge a hair off the grid is a flam nobody can hear until the
    LFO is driving an amplitude at 1/16 and the mix is full of clicks. */
struct Shape
{
    const char*   name;
    ShapeCategory category;
    int           grid;
    LfoPath       path;
};

// ------------------------------------------------------------------ builders
namespace detail {

/** A path assembled point by point, IN ORDER, so that what comes out is already
    inside LfoPath's rules and `tidy()` has nothing to do to it. The probe
    asserts that: a shape tidy() had to move was built wrong, not built
    tolerantly. */
struct Build
{
    LfoPath l;
    Build() noexcept { l.count = 0; }

    Build& pt (float x, float y, float curve = 0.0f) noexcept
    {
        if (l.count < kMaxPathPoints) l.p[l.count++] = { x, y, curve };
        return *this;
    }

    [[nodiscard]] LfoPath done (int loopback = -1) noexcept
    {
        l.loopback = loopback;
        if (l.count < 2) { l.count = 2; l.p[0] = { 0.0f, 0.5f, 0.0f }; l.p[1] = { 1.0f, 0.5f, 0.0f }; }
        return l;
    }
};

[[nodiscard]] inline float cell (int k, int grid) noexcept { return (float) k / (float) grid; }

/** The level a pattern character stands for; negative means no hit. Four
    weights are enough to write a drum loop: a kick, a snare, a hat and a ghost
    note read as X, x, - and . on the page, which is how the patterns below
    can be checked by eye against the grid they name. */
[[nodiscard]] inline float hitLevel (char c) noexcept
{
    switch (c)
    {
        case 'X': return 1.00f;
        case 'x': return 0.65f;
        case '-': return 0.35f;
        default:  return -1.0f;
    }
}

/** HITS THAT DECAY TO THE NEXT HIT. This is the shape Serum's Rhythmic
    submenu is made of and the reason it sounds like a rhythm rather than a
    tremolo: each hit jumps to its level and falls away exponentially until the
    next one, so a rest after a hit is a long tail and a run of hits is a row of
    short ones. `pattern` has one character per grid cell. `decay` is the bend
    of each falling segment: negative is fast-then-slow, which is what a decay
    is; -0.6 is k = 3.4 in pathCurve, about the fall of a plucked amplitude. */
[[nodiscard]] inline LfoPath hits (const char* pattern, int grid, float decay = -0.6f) noexcept
{
    Build b;
    if (hitLevel (pattern[0]) < 0.0f) b.pt (0.0f, 0.0f);
    for (int k = 0; k < grid && pattern[k] != '\0'; ++k)
    {
        const float lv = hitLevel (pattern[k]);
        if (lv < 0.0f) continue;
        const float x = cell (k, grid);
        if (k > 0) b.pt (x, 0.0f);
        b.pt (x, lv, decay);
    }
    b.pt (1.0f, 0.0f);
    return b.done();
}

/** FLAT GATES: on or off per cell, with a vertical edge wherever the pattern
    changes. This is the trance gate and the chopper, and it is what the guide's
    Shift-click "draw step segments" produces by hand. A point is spent only
    where the level changes, which is what keeps a sixteen-cell pattern inside
    thirty-two points. */
[[nodiscard]] inline LfoPath gates (const char* pattern, int grid, float on = 1.0f, float off = 0.0f) noexcept
{
    Build b;
    float cur = pattern[0] == '1' ? on : off;
    b.pt (0.0f, cur);
    for (int k = 1; k < grid && pattern[k] != '\0'; ++k)
    {
        const float nv = pattern[k] == '1' ? on : off;
        if (nv == cur) continue;
        const float x = cell (k, grid);
        b.pt (x, cur);
        b.pt (x, nv);
        cur = nv;
    }
    b.pt (1.0f, cur);
    return b.done();
}

/** A SIDECHAIN PUMP. `dips` per bar; each one drops from open (1) to `floor`
    on its beat and recovers.

    `recover` is the bend of the recovery: a compressor releases fast-then-slow,
    so the natural pump is negative; a positive bend stays low longer and
    surges back late, which is the "lazy" pump people reach for on pads.
    `snapAt`, as a fraction of the dip's own period, is where the level is back
    at 1 - anything under 1 leaves the rest of the beat open and flat, which is
    the fast pump. `hold`, also a fraction, keeps the level AT the floor before
    the recovery starts: that is the duck-and-hold, the shape that makes room
    for a whole kick rather than only its transient. */
[[nodiscard]] inline LfoPath pump (int dips, float floor, float recover,
                                   float snapAt = 1.0f, float hold = 0.0f) noexcept
{
    Build b;
    for (int k = 0; k < dips; ++k)
    {
        const float x0 = cell (k, dips), period = 1.0f / (float) dips;
        b.pt (x0, 1.0f);
        if (hold > 0.0f) { b.pt (x0, floor); b.pt (x0 + hold * period, floor, recover); }
        else              b.pt (x0, floor, recover);
        if (snapAt < 1.0f) b.pt (x0 + snapAt * period, 1.0f);
    }
    b.pt (1.0f, 1.0f);
    return b.done();
}

/** A deterministic generator with no state outside the caller's hands.
    xorshift32: three shifts, a period of 2^32 - 1 and nothing to link. The
    seed is what makes "Random" a shape you can get BACK - the same seed draws
    the same path on every machine, so a patch that stores the seed stores the
    shape. */
struct Rng
{
    std::uint32_t s;
    explicit Rng (std::uint32_t seed) noexcept : s (seed != 0u ? seed : 0x9E3779B9u) {}
    std::uint32_t next() noexcept { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    /** Uniform in [0, 1). Twenty-four bits so it is exact in a float. */
    float unit() noexcept { return (float) (next() >> 8) * (1.0f / 16777216.0f); }
};

} // namespace detail

// ------------------------------------------------------------ the generators

/** RANDOM (CURVED): `points` points at random positions, random levels and a
    random bend on every segment. `[EAR]` Serum's is a wandering curve; ours
    keeps the bends inside +-0.8 so no segment collapses into a step, because a
    step drawn by chance is a click by chance. The same seed gives the same
    path; a different seed gives a different one. */
[[nodiscard]] inline LfoPath randomCurved (std::uint32_t seed, int points) noexcept
{
    const int n = std::clamp (points, 2, kMaxPathPoints);
    detail::Rng r (seed);
    float xs[kMaxPathPoints];
    xs[0] = 0.0f;
    xs[n - 1] = 1.0f;
    for (int i = 1; i < n - 1; ++i) xs[i] = r.unit();
    std::sort (xs + 1, xs + (n - 1));
    LfoPath l;
    l.count = n;
    l.loopback = -1;
    for (int i = 0; i < n; ++i)
    {
        const float y = r.unit();
        const float c = r.unit() * 1.6f - 0.8f;
        l.p[i] = { xs[i], y, i == n - 1 ? 0.0f : c };
    }
    return l;
}

/** RANDOM (FLAT): `steps` flat steps at random levels, joined by vertical
    jumps - a sample-and-hold sequence you can see and edit. Two points per
    step, so at most sixteen steps in a thirty-two point path; `steps` is
    clamped to 1..16 and the path holds twice that many points. */
[[nodiscard]] inline LfoPath randomFlat (std::uint32_t seed, int steps) noexcept
{
    const int n = std::clamp (steps, 1, kMaxPathPoints / 2);
    detail::Rng r (seed);
    LfoPath l;
    l.count = 2 * n;
    l.loopback = -1;
    for (int k = 0; k < n; ++k)
    {
        const float y = r.unit();
        l.p[2 * k]     = { detail::cell (k, n),     y, 0.0f };
        l.p[2 * k + 1] = { detail::cell (k + 1, n), y, 0.0f };
    }
    return l;
}

// --------------------------------------------------------------- the library

/** The whole library, built once. Every shape is `[EAR]` - ours, drawn to a
    purpose named in its comment - and every one is built tidy. */
inline const std::vector<Shape>& library()
{
    using detail::Build;
    using detail::hits;
    using detail::gates;
    using detail::pump;

    static const std::vector<Shape> lib = []
    {
        std::vector<Shape> v;
        v.reserve (40);

        // ---------------------------------------------------------- BASIC
        // The waveforms a shape selector used to offer, now as paths you can
        // pull about. The sine and triangle are LfoPath's own so the library
        // cannot drift from the shape a new LFO opens on.
        v.push_back ({ "Sine",     kShapeBasic, 0, LfoPath::sine() });
        v.push_back ({ "Triangle", kShapeBasic, 0, LfoPath::triangle() });
        v.push_back ({ "Saw Down", kShapeBasic, 0, Build().pt (0.0f, 1.0f).pt (1.0f, 0.0f).done() });
        v.push_back ({ "Ramp Up",  kShapeBasic, 0, Build().pt (0.0f, 0.0f).pt (1.0f, 1.0f).done() });
        v.push_back ({ "Square",   kShapeBasic, 0,
                       Build().pt (0.0f, 1.0f).pt (0.5f, 1.0f).pt (0.5f, 0.0f).pt (1.0f, 0.0f).done() });
        // A quarter-width pulse: the same edges as the square with the high
        // part cut to a beat of the bar, which is the gate a player wants when
        // "square" turns out to be too much on.
        v.push_back ({ "Pulse 25%", kShapeBasic, 0,
                       Build().pt (0.0f, 1.0f).pt (0.25f, 1.0f).pt (0.25f, 0.0f).pt (1.0f, 0.0f).done() });
        // THE SAMPLE-AND-HOLD, AS EIGHT STEPS YOU CAN SEE. A real S&H throws a
        // new value every clock and never repeats; this is one throw of eight,
        // frozen, so that the legacy S&H selector lands on something drawn
        // rather than on nothing. The levels are chosen to wander the whole
        // range without two neighbours close enough to read as a held step.
        {
            constexpr float lv[8] = { 0.60f, 0.15f, 0.85f, 0.40f, 0.95f, 0.25f, 0.70f, 0.05f };
            Build b;
            for (int k = 0; k < 8; ++k)
            {
                b.pt (detail::cell (k, 8),     lv[k]);
                b.pt (detail::cell (k + 1, 8), lv[k]);
            }
            v.push_back ({ "Eight Steps", kShapeBasic, 8, b.done() });
        }
        // An exponential fall and its mirror. -0.75 is k = 4 in pathCurve, so
        // the level is at a quarter by a third of the way across: a plucked
        // decay rather than a linear fade.
        v.push_back ({ "Decay",   kShapeBasic, 0, Build().pt (0.0f, 1.0f, -0.75f).pt (1.0f, 0.0f).done() });
        v.push_back ({ "Rise",    kShapeBasic, 0, Build().pt (0.0f, 0.0f,  0.75f).pt (1.0f, 1.0f).done() });
        // Ease in, then ease out: a smoothed ramp, for a sweep that neither
        // lurches off the line nor slams into the top.
        v.push_back ({ "S Curve", kShapeBasic, 0,
                       Build().pt (0.0f, 0.0f, 0.6f).pt (0.5f, 0.5f, -0.6f).pt (1.0f, 1.0f).done() });

        // ------------------------------------------------------- RHYTHMIC
        // One cycle is one bar. Patterns read left to right across the grid
        // named beside them; X is a full hit, x a medium one, - a light one.
        v.push_back ({ "Four on the Floor", kShapeRhythmic, 4,  hits ("XXXX", 4) });
        v.push_back ({ "Backbeat",          kShapeRhythmic, 4,  hits (".X.X", 4) });
        v.push_back ({ "Off Beats",         kShapeRhythmic, 8,  hits (".X.X.X.X", 8) });
        // Every third sixteenth: the dotted-eighth cross-rhythm that lands back
        // on the downbeat every three bars. It is the one pattern here that
        // does not resolve inside its own cycle, and that is the point of it.
        v.push_back ({ "Dotted Eighths",    kShapeRhythmic, 16, hits ("X..X..X..X..X..X", 16) });
        // Swing on a triplet grid: the first and third triplet of every beat,
        // which is the shuffle every drummer plays and no straight grid can
        // draw.
        v.push_back ({ "Triplet Swing",     kShapeRhythmic, 12, hits ("X.XX.XX.XX.X", 12) });
        // A drum loop as levels: kick on 1 and 3, snare on 2 and 4, hats on
        // the off eighths and a ghost before the turnaround.
        v.push_back ({ "Drum Loop",         kShapeRhythmic, 16, hits ("X.-.x.-.X.-.x.x.", 16, -0.7f) });
        // Eight hits that fall away across the bar: the breakdown, where the
        // pattern keeps playing and the room drains out of it.
        {
            Build b;
            for (int k = 0; k < 8; ++k)
            {
                const float lv = 1.0f - 0.1f * (float) k;
                if (k > 0) b.pt (detail::cell (k, 8), 0.0f);
                b.pt (detail::cell (k, 8), lv, -0.6f);
            }
            b.pt (1.0f, 0.0f);
            v.push_back ({ "Breakdown", kShapeRhythmic, 8, b.done() });
        }
        // And the mirror: eight hits that climb, for the bar before a drop.
        {
            Build b;
            for (int k = 0; k < 8; ++k)
            {
                const float lv = 0.3f + 0.1f * (float) k;
                if (k > 0) b.pt (detail::cell (k, 8), 0.0f);
                b.pt (detail::cell (k, 8), lv, -0.6f);
            }
            b.pt (1.0f, 0.0f);
            v.push_back ({ "Riser Hits", kShapeRhythmic, 8, b.done() });
        }
        // Two bursts of four sixteenths with a rest between: the multipulse,
        // and the short decay is what keeps four hits in a beat from smearing
        // into one.
        v.push_back ({ "Multipulse",        kShapeRhythmic, 16, hits ("XXXX....XXXX....", 16, -0.85f) });
        // Eight rising saws to the bar. A ramp per eighth reads as a rhythm on
        // a filter and as a pitch-slide sequence on an oscillator.
        {
            Build b;
            for (int k = 0; k < 8; ++k)
            {
                b.pt (detail::cell (k, 8),     0.0f);
                b.pt (detail::cell (k + 1, 8), 1.0f);
            }
            v.push_back ({ "Up Saws", kShapeRhythmic, 8, b.done() });
        }
        // FLAT GATES. The trance gate is the pattern every gate plugin ships as
        // its first preset for a reason: it moves on both halves of the bar and
        // never rests two cells in a row.
        v.push_back ({ "Trance Gate",       kShapeRhythmic, 16, gates ("1101101101101101", 16) });
        v.push_back ({ "Off-Beat Gate",     kShapeRhythmic, 8,  gates ("01010101", 8) });
        v.push_back ({ "Triplet Gate",      kShapeRhythmic, 12, gates ("110110110110", 12) });
        // Open for the first half, chopped to sixteenths for the second: the
        // half-bar stutter.
        v.push_back ({ "Half-Bar Stutter",  kShapeRhythmic, 16, gates ("1111111110101010", 16) });
        // RESTART SHAPES: the loopback is SET. `[MAN]` p. 191 - in ENVELOPE
        // mode the LFO plays through once and cycles back to the loopback
        // point. So these have an intro that plays once - a long hit on the
        // downbeat - and a figure after it that loops, and the loopback sits on
        // the first point of the figure. In FREE or RETRIG mode the whole bar
        // simply repeats and the loopback is idle.
        {
            // Intro: one hit decaying across the first beat, landing at point
            // 1. Figure: eighths from beat two; point 2 is its first hit and
            // the loopback.
            Build b;
            b.pt (0.0f, 1.0f, -0.5f);
            b.pt (0.25f, 0.0f);
            for (int k = 2; k < 8; ++k)
            {
                if (k > 2) b.pt (detail::cell (k, 8), 0.0f);
                b.pt (detail::cell (k, 8), 0.7f, -0.6f);
            }
            b.pt (1.0f, 0.0f);
            v.push_back ({ "Bar Restart", kShapeRhythmic, 8, b.done (2) });
        }
        {
            // Intro over the first half; an eighth-note figure on the sixteenth
            // grid loops from 0.5, point 2 again.
            Build b;
            b.pt (0.0f, 1.0f, -0.4f);
            b.pt (0.5f, 0.0f);
            for (int k = 8; k < 16; k += 2)
            {
                if (k > 8) b.pt (detail::cell (k, 16), 0.0f);
                b.pt (detail::cell (k, 16), 0.8f, -0.75f);
            }
            b.pt (1.0f, 0.0f);
            v.push_back ({ "Half-Bar Restart", kShapeRhythmic, 16, b.done (2) });
        }

        // ------------------------------------------------------ SIDECHAIN
        // Every pump starts OPEN and dips on its beats. The floor is 0 for the
        // pumps a kick would drive and higher for the gentler ones, so a
        // player choosing by name gets the depth the name promises.
        v.push_back ({ "Pump Quarters",      kShapeSidechain, 4, pump (4, 0.0f, -0.6f) });
        // Back to open by the eighth, then flat: the snap pump, the one that
        // lets the off-beat through untouched.
        v.push_back ({ "Pump Quarters Snap", kShapeSidechain, 4, pump (4, 0.0f, -0.7f, 0.5f) });
        // Stays low and surges back late: the lazy pump, for pads under a kick.
        v.push_back ({ "Pump Quarters Lazy", kShapeSidechain, 4, pump (4, 0.1f,  0.5f) });
        v.push_back ({ "Pump Eighths",       kShapeSidechain, 8, pump (8, 0.0f, -0.6f) });
        v.push_back ({ "Pump Halves",        kShapeSidechain, 2, pump (2, 0.0f, -0.5f) });
        // Down, HELD for a sixteenth, then up: room for the whole kick and not
        // only its transient.
        v.push_back ({ "Duck and Hold",      kShapeSidechain, 4, pump (4, 0.0f, -0.6f, 1.0f, 0.25f) });
        // One long duck to the bar with a slow, nearly linear return.
        v.push_back ({ "Long Duck",          kShapeSidechain, 1, pump (1, 0.15f, -0.25f) });
        // Dips on two and four only, gently: the backbeat duck, a pump that
        // sits behind a snare rather than a kick.
        {
            Build b;
            b.pt (0.0f, 1.0f).pt (0.25f, 1.0f).pt (0.25f, 0.3f, -0.6f)
             .pt (0.5f, 1.0f).pt (0.75f, 1.0f).pt (0.75f, 0.3f, -0.6f).pt (1.0f, 1.0f);
            v.push_back ({ "Duck Backbeat", kShapeSidechain, 4, b.done() });
        }

        return v;
    }();
    return lib;
}

[[nodiscard]] inline int shapeCount() noexcept { return (int) library().size(); }

[[nodiscard]] inline const Shape& shapeAt (int i) noexcept
{
    const auto& lib = library();
    return lib[(size_t) std::clamp (i, 0, (int) lib.size() - 1)];
}

[[nodiscard]] inline ShapeCategory categoryOf (int i) noexcept { return shapeAt (i).category; }

/** The library index of a name, or -1. Linear, and the library has forty
    entries: not a lookup anyone does per sample. */
[[nodiscard]] inline int indexOf (const char* name) noexcept
{
    const auto& lib = library();
    for (size_t i = 0; i < lib.size(); ++i)
        if (std::strcmp (lib[i].name, name) == 0) return (int) i;
    return -1;
}

/** THE MENU'S "DEFAULT". `[MAN]` the first entry of the guide's menu, and it
    is the triangle for the reason LfoPath::triangle gives: two segments, and a
    beginner's first drag does what they expect. */
[[nodiscard]] inline LfoPath defaultShape() noexcept { return LfoPath::triangle(); }

/** THE LEGACY SIX. The LFO's SHAPE control today is the `lfoN_wave` parameter,
    six frozen positions labelled tri / saw / ramp / square / S&H / wave, and
    it stays: its range is frozen data and hosts persist automation by it.
    What changes is what choosing one DOES - it loads a shape - so each
    position needs a shape to load. Positions 0..3 are the waveforms they say;
    S&H is the eight frozen steps (the nearest drawable thing to a random
    hold), and "wave", which on the Virus was the door to the oscillator
    wavetables, opens on the sine here because that is the shape the wavetable
    door most often led to and the one Serum's own menu opens on. */
inline constexpr int kNumLegacyWaves = 6;

[[nodiscard]] inline int legacyWaveShape (int wave) noexcept
{
    switch (std::clamp (wave, 0, kNumLegacyWaves - 1))
    {
        case 0:  return indexOf ("Triangle");
        case 1:  return indexOf ("Saw Down");
        case 2:  return indexOf ("Ramp Up");
        case 3:  return indexOf ("Square");
        case 4:  return indexOf ("Eight Steps");
        default: return indexOf ("Sine");
    }
}

/** THE SHAPE MENU. `lfoN_wave` is a frozen parameter whose first six positions
    were the Virus-era waveforms; the menu keeps them where they are and lists
    the rest of the library after them, so a stored program's position still
    names the shape it named. Position -> library index, and the count. The author:
    "there is literally countless different patterns" - the menu is every
    shape the library has, in the order the library declares them. */
[[nodiscard]] inline int menuShapeCount() noexcept { return shapeCount(); }

[[nodiscard]] inline int menuShape (int position) noexcept
{
    if (position < kNumLegacyWaves) return legacyWaveShape (position);
    int seen = kNumLegacyWaves;
    for (int i = 0; i < shapeCount(); ++i)
    {
        bool legacy = false;
        for (int w = 0; w < kNumLegacyWaves; ++w) legacy = legacy || legacyWaveShape (w) == i;
        if (legacy) continue;
        if (seen == position) return i;
        ++seen;
    }
    return indexOf ("Triangle");
}

} // namespace shapes
} // namespace megasynth
