// MOVED OUT OF MEGASYNTH, 2026-09-26. This is the filter block of the global
// FILTER component (synthkit/components/filter). It lives here because dspblocks
// is the declared owner of DSP and a second copy anywhere else is the mistake
// that cost thirty per-unit SessionStrip copies.
//
// It moved VERBATIM. Only the namespace changed, from `megasynth` to
// `dsp::filter`, and megasynth keeps a one-line shim at its old path so every
// consumer compiles unchanged and the audio is bit-identical. Nothing in here
// was rewritten, re-derived or re-tuned; every law and its provenance tag is the
// one that was measured in megasynth.
//
// What is NOT here, on purpose: the cutoff SUM (stages 1-4) and the coefficient
// struct. Those are in synthkit/components/filter/dsp/, because they are the
// module's arithmetic rather than the block, which is the split the hardware had.

// THE SUPER FILTER: one filter, assembled from six machines' best ideas.
//
// `~/projects/supersynth/docs/super-filter.md` is the specification and the
// reason it works is not obvious: THE SIX MACHINES' SIGNATURES SIT AT SIX
// DIFFERENT STAGES OF A FILTER. They are not six opinions about the same thing,
// so they stack instead of competing.
//
//   1. control domain   Nord Lead 3  [FW]  the cutoff control IS a note number
//   2. the sum          Matrix-1000  [FW]  cutoff is a seven-term sum
//   3. one conversion   JP-8000      [FW]  the summed cents convert ONCE
//   4. shared curve     Supernova    [FW]  the same curve as oscillator pitch
//   5. shape            Elka Synthex [SCH] the modes are weighted sums of taps
//   6. slope            Nord + OB-Xa [FW]+[SCH] a gain shift, trimmed cells
//   7. nonlinearity     Access Virus [FW]  ONE asymmetric limiter, on FEEDBACK
//
// Stages 1-4 live in `CutoffTerms` (Engine.h) and in makeCoeffs, because they
// are arithmetic on the way IN. Stages 5-7 are here, because they are the block.
//
// THERE IS NO MODEL SELECTOR AND THERE ARE NO FOUR MODE BUTTONS. The author: "we take
// everything we like and fuse it into one ... some modes and a cutoff, res and
// mod amount. not 100 buttons lol." LP, BP, HP and notch are POINTS ON SHAPE,
// the per-anchor trims are internal like the chips they model, and the body
// compensation is always on. Exposing any of them is the "100 buttons" failure.
//
// ---------------------------------------------------------------------------
// WHAT THIS HAS TO SOLVE, in the author's words: "one of the biggest issues almost all
// vst instruments have is nothing happens in bandpass when you touch cutoff."
//
// THE CAUSE IS ENERGY, NOT TONE. A lowpass sweep removes or restores every
// harmonic above the corner, so loudness and brightness move together. A
// constant-Q bandpass passes roughly the same energy wherever it sits, so the
// timbre shifts and nothing builds - and most SVFs make it worse by scaling the
// bandpass tap by 1/Q, which is quiet and dull at low resonance and thin at
// high. A single notch is subtle BY DEFINITION.
//
// So: bandwidth is its OWN control (WIDTH), the body compensation is per SHAPE
// and measured against each shape ITSELF rather than by blending toward a shared
// reference, and the notch is the input MINUS a normalised bandpass, which nulls
// exactly at the corner and leaves the resonance sitting on the shoulders either
// side of the null instead of filling it in.
#pragma once
#include "Oversampler.h"

#include <algorithm>
#include <cmath>

namespace dsp { namespace filter {

// =================================================== stage 7: the limiter ===
/** [FW] Access Virus P:0x3B87D. THE ANALOGUE MODES DIFFER FROM THE DIGITAL ONES
    BY EXACTLY ONE THING: a three-way asymmetric limiter on the feedback term.
    The firmware tests the term against 0x7FF000 and branches into a clip-high
    limb, a POSITIVE limb and a NEGATIVE limb; the digital modes have no such
    branch at all.

    The two cubic limbs are the catalogue's (`synthcatalog/filters/access/
    virus.h`): `x - 0.25 x^3` going up and `x - 0.18 x^3` going down. They are
    what makes this asymmetric, and asymmetry is the whole point - a symmetric
    saturator adds odd harmonics and nothing else, while an asymmetric one adds
    even ones and a level-dependent offset, which is what a real resonance cell
    does when it is driven into one rail before the other.

    WHERE THE CATALOGUE'S VERSION IS NOT COPIED, and it is deliberate: it returns
    the threshold itself above 0.99951 while the positive limb is only at 0.7497
    there, so the curve JUMPS by 0.25 at the threshold. Inside a feedback loop a
    discontinuity of that size is not a saturation, it is a square wave
    generator. Each limb is held at its own turning point instead - the value
    where its cubic stops being monotonic - which keeps the function continuous,
    monotonic and bounded, and keeps the asymmetry exactly where the firmware
    puts it: +0.7698 up against -0.9072 down, a ratio of 1.178.

    BOUNDED IS LOad-BEARING. The feedback term can never exceed these, so the
    loop cannot diverge however hard it is driven - which is why this filter has
    no NaN guard and needs none. The Synthex's own catalogue entry records the
    opposite lesson: its first build had a LINEAR feedback limit and returned NaN
    for the whole filter section as soon as the resonance probe reached the top
    of the knob, because a third of that machine's resonance travel is past unity
    loop gain BY DESIGN. */
inline constexpr float kLimitPosCubic = 0.25f;   // [FW] the positive limb
inline constexpr float kLimitNegCubic = 0.18f;   // [FW] the negative limb
/** Each limb's turning point, `x = 1/sqrt(3c)`, and the value it holds there. */
inline constexpr float kLimitPosX = 1.1547005f, kLimitPosY = 0.7698004f;
inline constexpr float kLimitNegX = 1.3608276f, kLimitNegY = 0.9072184f;

[[nodiscard]] inline float asymmetricLimit (float x) noexcept
{
    if (x >= 0.0f)
        return x > kLimitPosX ? kLimitPosY : x - kLimitPosCubic * x * x * x;
    return x < -kLimitNegX ? -kLimitNegY : x - kLimitNegCubic * x * x * x;
}

/** ================================================= DRIVE'S MAKE-UP GAIN ====
    AND IT IS FOR THE TYPES WHOSE LIMITER SITS IN THE SIGNAL PATH. The author,
    2026-09-27: *"why does the DRIVE filter knob when turned up lower the
    volume?"*

    THE FAULT, MEASURED. A 110 Hz saw at 0.5 peak, a 1 kHz corner, slope 24,
    lowpass, width 0.5, DRIVE 0 -> 1, rms and peak:

        type            res 0.0          res 0.2          res 0.6
        SUPER   (0)   +0.00 / +0.00   +0.52 / +0.07   +3.60 / +0.83 dB
        SVF     (1)   +0.00 / +0.00   -2.22 / -6.01   -7.79 / -6.27
        CASCADE (2)   -5.45 / -7.55   -2.84 / -6.02   -2.17 / -6.66
        TWIN    (3)   -5.45 / -7.55   -2.84 / -6.02   -2.17 / -6.66

    (TWIN reads CASCADE's numbers because inside THIS block type 3 IS type 2:
    `setAlt` and `processAlt` both branch on `kTypeSvf` and fall through to the
    cascade, and the Andromeda pair lives in `dsp/TwinFilter.h`, which CONTAINS a
    SuperFilter rather than being one. Not a fault of this change and not fixed by
    it; worth knowing before reading the two rows as independent evidence.)

    THE CAUSE IS ONE EXPRESSION AND IT IS NOT THE LIMITER'S POSITION. All four
    types hand the limiter `dGain` going in and divide `dInv = 1/dGain` back out
    coming away; what differs is WHERE the limiter sits.

      * SUPER limits the FEEDBACK TERM. Compressing a NEGATIVE feedback term
        REDUCES the feedback, so the filter gets louder and more resonant as DRIVE
        rises - `docs/super-filter.md` specifies exactly that, and the +3.60 dB is
        it working. At resonance 0 there is no feedback term, so DRIVE correctly
        does nothing at all.
      * CASCADE and TWIN limit the SUMMING NODE, which is in the SIGNAL path.
        `L(u g) / g` has a small-signal gain of exactly 1 at every `g` and a
        large-signal gain BELOW 1 that falls as `g` rises: everything the limiter
        takes is lost and nothing gives it back. At resonance 0 - where the node
        sees nothing but the input, the feedback term being zero - it is a
        compressor on the dry signal and nothing else. -5.45 dB of rms and -7.55
        of peak for turning a knob called DRIVE, with no resonance in the picture
        to justify it.
      * SVF limits the BANDPASS INTEGRATOR STATE, inside the loop. At resonance 0
        its cell is not engaged at all - `sat` is `1 - d` and `d` is 1 there - so
        DRIVE correctly does nothing, and at 0.6 it costs 7.79 dB.

    THE POSITIONS ARE RIGHT AND THE MISSING PART IS THE MAKE-UP, and that was
    checked rather than assumed, because the other answer was available. [SCH] the
    Polysix's D7/D8 diode pair really is on the summing node and [supersynth]
    reaches the same node with a `tanh`; [supersynth]'s placement on `ic1` really
    is what bounds the SVF at `k = 0`. What no machine in
    `~/projects/synthcatalog/filters` has is the `1/g` that follows, and [MAN]
    JD-800 I-96 states the opposite law outright: *"higher values of Drive will
    increase the overall volume as well. [Level] is used to compensate."* A drive
    raises level and the machine gives you a Level to take it back. There is no
    Level here, so the make-up is where the two meet.

    AND THE TWO FAULTS ARE NOT THE SAME FAULT, which one measurement settles and
    which is why this block is long. The 40 Hz passband and the peak over it, same
    signal, with the make-up OFF:

        type       DRIVE 0            DRIVE 0.5          DRIVE 1.0
        res 0.6
        SUPER     0.5801 +10.44 dB   0.5939  +8.26 dB   1.1018  +1.27 dB
        SVF       0.1234 +22.52      0.1234 +15.73      0.1234  +8.42
        CASCADE   0.2945 +10.95      0.2940  +6.33      0.2845  +0.74
        res 0.2
        SUPER     0.7731  +1.40      0.7771  +1.36      0.8621  +0.64
        SVF       0.4692 +11.29      0.4692  +7.10      0.4692  +2.47
        CASCADE   0.5551  +1.58      0.5486  +1.04      0.3920  -0.64

      * CASCADE AND TWIN LOSE A PASSBAND. 0.5551 -> 0.3920 is 3.0 dB of body gone
        at resonance 0.2. That is a gain the limiter took, and a make-up is the fix.
      * THE SVF DOES NOT LOSE A PASSBAND AT ALL. 0.1234, 0.1234, 0.1234 - not one
        count, at either resonance, across the whole travel. What DRIVE removes
        there is the PEAK, 22.52 dB down to 8.42, and the rms falls because at
        Q 12 the peak IS most of the rms. That is not a gain the limiter took, it
        is a resonance being limited - which is what a limiter on the bandpass
        integrator state is FOR.

    SO THE SVF'S MAKE-UP IS A COMPRESSOR'S MAKE-UP AND THE FILE SAYS SO RATHER
    THAN PRETENDING OTHERWISE. It cannot restore a body that never moved; it
    restores the average level that limiting the peak costs, and it therefore
    lifts the body ABOVE where it started. That is a standard device - peak
    limiting plus make-up is what a limiter IS - and on this type DRIVE becomes
    "the resonance is progressively limited and the level is made up", so the
    filter gets denser and more even instead of quieter. It is NOT the same control
    as SUPER's DRIVE, which adds harmonics and level together.

    WHAT WOULD MAKE IT THE SAME CONTROL, PROPOSED AND NOT DONE: move the SVF's
    limiter from the integrator state to the DAMPING term, where compressing it
    would RAISE the resonance as DRIVE rises - the mirror of what SUPER does with
    its feedback term, and the only placement on this topology where more drive
    means more of everything. That is a design pass and not a patch:
    [supersynth]'s placement on `ic1` is the only thing bounding a type that
    reaches Q 501 on its 12 dB anchor, so moving it wants its own stability gate
    before it moves. Filed here rather than done.

    AND NO MAKE-UP CAN MAKE DRIVE LOSSLESS EVERYWHERE, ON ANY TYPE, WHICH IS THE
    LAST THING TO KNOW BEFORE READING THE NUMBERS. A limiter in a resonant loop
    removes a self-oscillation, and at settings where the oscillation IS the
    output the level has to fall. SUPER - bit-identical territory, the one that
    behaves, and therefore the reference for what is acceptable - loses 3.22 dB
    somewhere on a 8640-point surface at resonance 0.6 or under, and 30.97 dB at
    the top of the resonance knob (a 12 kHz corner, highpass, slope 18). So the
    law is applied where it is meaningful: AT RESONANCE 0 there is no resonance to
    remove, so any loss there is pure gain loss and must go. Over 240 settings of
    cutoff, shape, width, slope and drive at resonance 0:

        worst loss at resonance 0     before      after
        SUPER                         +0.00 dB    +0.00 dB   (untouched)
        SVF                           +0.00       +0.00      (cell not engaged)
        CASCADE / TWIN                -9.39       -3.86      (most it gains +3.32)

    and over the whole 8640-point surface the worst loss anywhere falls from
    -15.49 to -7.25 dB on the SVF and from -18.38 to -12.86 on CASCADE, against
    SUPER's own -30.97. */
inline constexpr float kDriveBottomGain = 0.35f;

/** HOW MUCH OF DRIVE'S OWN GAIN SURVIVES THE CELL, per topology, and that is all
    the make-up is. `dInv` divides the whole of `dGain` back out; this multiplies a
    FRACTION of it back in on the OUTPUT. `q = 0` is exactly what the filter did
    before and `q = 1` is [MAN] the JD-800's raw law.

    ON THE OUTPUT AND NOT ON THE NODE, which took one wrong turn to establish. The
    obvious fix is to divide by less - `L(u g) / g^(1-q)` - and on CASCADE that is
    WRONG: the node's output IS the loop's forward path, so a make-up of 2.87 there
    multiplies the loop gain by 2.87 as well, and `kCascadeLoopMax`'s rail is
    computed without it. `body4` and `body2` are outside the loop and are already
    where this file puts every gain that gives back what a loop took.

    IT IS A POWER OF THE GAIN RATIO, which makes the make-up LINEAR IN DECIBELS
    against the DRIVE control: `dGain` is exponential in DRIVE by construction, so
    `(dGain/g0)^q` is `q * 23.1 dB * drive`. The LOSS is not linear - on the node
    cell almost all of it arrives in the top quarter of the travel - so a make-up
    shaped like the loss would be a curve fitted to one operating level, where a
    make-up shaped like the CONTROL is one number that cannot mis-track a level it
    was not measured at. The price is a bulge: the level rises through mid-travel
    and comes back near where it started at the top. Rising is allowed and falling
    is not, which is the law.

    AND THE CASCADE'S NUMBER IS FIXED BY A GATE RATHER THAN BY TASTE, which is the
    best thing about it. Larger is better for the complaint and the 6400-setting
    stability sweep says how much larger is allowed:

        q      worst loss at res 0   most it gains   9e's 6400 settings
        0.00        -9.39 dB            +0.00          0 over bound
        0.24        -3.86               +3.32          0
        0.25        -3.63               +3.56          4 over
        0.27        -3.17               +4.03          8 over
        0.32        -2.02               +5.03         40 over
        0.41        +0.00               +6.61         40 over

    Every one of the offenders is `cut 20, shape 2, DRIVE 1` on types 2 and 3 - a
    highpass at a 20 Hz corner is not filtering anything, so a broadband gain there
    is simply a gain, and 1.28 rms comes out of a 0.231 rms input. That is the same
    failure `kBodyHp1` and `kBodyBp4` both record, reached by a third route. 0.24 is
    the largest value that leaves the sweep clean, and at it CASCADE's own +3.32 dB
    of maximum rise is inside SUPER's +3.60 at the reference setting - so the fix
    lands the broken types on the behaviour of the one that works.

    THE SVF'S NUMBER IS FIXED BY THE LAW INSTEAD, because the sweep does not
    constrain it: its make-up is gated by `1 - d`, which is 0 at the low-corner
    highpass settings that the sweep trips on, and 0.80 still returns 0 over bound.
    0.40 is the smallest value for which no resonance loses anywhere in DRIVE's
    travel at the reference setting - 0.34 still leaves 0.58 dB at resonance 0.6. */
inline constexpr float kDriveMakeupCascade = 0.24f;   // [EAR], and the sweep's bound
inline constexpr float kDriveMakeupSvf     = 0.40f;   // [EAR], and the law's

/** HOW FAR A STORED PROGRAM MOVES, because this is the one change here that moves
    any audio at all and a consumer has to be able to plan for it. Only a program on
    type 1, 2 or 3 WITH DRIVE ABOVE ZERO moves; everything else is bit-identical.
    CASCADE at a 1 kHz corner, slope 24, lowpass, width 0.5, the same 110 Hz saw:

        DRIVE   res 0.2            res 0.6
        0.15    0.1596 -> 0.1756   0.0998 -> 0.1098   +0.83 dB
        0.20    0.1594 -> 0.1810   0.0995 -> 0.1130   +1.11
        0.35    0.1584 -> 0.1979   0.0981 -> 0.1226   +1.93
        0.50    0.1561 -> 0.2145   0.0953 -> 0.1310   +2.77
        1.00    0.1153 -> 0.2180   0.0782 -> 0.1479   +5.54

    and the SVF, whose make-up is resonance-scaled so its rows are not flat: +1.02
    dB at DRIVE 0.20 and resonance 0.2, +1.69 at 0.6, rising to +5.13 and +8.47 at
    DRIVE 1.

    WHO THAT REACHES, AND IT IS A LOT. megasynth's `envelope_laws_hold` check 4 is
    the standing bit-exact hash of 599 stored entries against the bank's own birth
    certificate, and it goes RED: 344 of 599 render differently. Split by topology,
    measured one exponent at a time, 134 move on the node cell alone and 228 on the
    SVF's alone, so 18 of them use both. With BOTH exponents set to 0 the hash is
    green at 599 of 599 - which is the measurement that says the one-pole anchor,
    the withdrawn six-pole and everything else in this file's 2026-09-27 pass move
    NOTHING, and that this make-up is the whole of the movement.

    Solera moves too and by less: `voicingOfLegacyModel` hands its legacy Ladder
    model `{ kTypeCascade, 24, 0.50, 0.15 }` and its legacy Nord model
    `{ kTypeSvf, 12, 0.50, 0.15 }`, so every patch that predates `fType` and was one
    of those two gains about 0.8 dB, and its own bank carries 17 CASCADE, 12 SVF and
    1 TWIN programs of which only the ones with DRIVE up move at all.

    THAT IS INTENDED AND IT IS THE POINT. Every one of those programs was voiced
    against a DRIVE knob that ducked; a program whose level rises when the fault is
    removed was quieter than its author meant it to be. But 344 of 599 is not a
    number anyone should discover from a red gate, so it is written here: RE-
    CAPTURING `tools/env_reference.txt` IS A DECISION ABOUT A BANK AND NOT A
    CONSEQUENCE OF A FIX. It belongs to whoever voiced the bank, after listening,
    and this file does not take it. */

/** WHAT IT COSTS: one `std::pow` per sample in `setAlt`, which already calls two,
    and it is not measurable. Type 2 measures 29.1 ns/sample against dspblocks
    HEAD's 34.1 - the one extra call disappears inside what the block gained from
    losing the six-pole's 48 bytes of state. Type 0 never reaches it at all. */

/** [EAR] the ceiling, for the same reason `kBodyMakeupMax` has one: a make-up that
    chases a limiter all the way stops restoring a body and starts amplifying a
    rail. Neither exponent reaches it - the larger one tops out at 14.29^0.40 =
    2.90 - so it is a guard rather than a shape. */
inline constexpr float kDriveMakeupMax = 4.0f;

/** DRIVE's make-up, and it is exactly 1.000 at the bottom of the travel by
    construction: `driveGain` IS `kDriveBottomGain` there, the ratio is 1, and every
    power of 1 is 1. Nothing a stored program holds at DRIVE 0 moves by a bit, on
    any type, at any oversampling - measured over 11.2 million samples per setting
    row rather than argued.

    `engaged` is how much of the limiter cell reaches the output on this topology:
    1 on the node cell, which runs at every resonance, and `1 - d` on the SVF's
    state cell, which is the same factor that mixes the cell in. It scales the
    EXPONENT rather than the result, so a half-engaged cell gets half the make-up
    in decibels, which is the domain the whole law lives in.

    AND SUPER NEVER REACHES THIS FUNCTION. There is no third constant and no branch
    that could get here from `set` or `core`; the make-up is called from
    `svfTapWeights` and from the cascade limb of `setAlt`, both of which type 0
    returns before. */
[[nodiscard]] inline float driveMakeup (float driveGain, float fraction, float engaged) noexcept
{
    const float q = fraction * std::clamp (engaged, 0.0f, 1.0f);
    return std::min (kDriveMakeupMax,
                     std::pow (std::max (1.0f, driveGain / kDriveBottomGain), q));
}

// ================================== the anchors: two of them, then a third ===
/** [SCH] THE OB-Xa VOICE CARD CARRIES TWO CEM3320s - A5 "2 POLE VCF" and A6
    "4 POLE VCF" - BOTH PERMANENTLY POWERED, with a CD4053 selecting between
    their OUTPUTS on the 2P4P line. [MAN] Oberheim states it in as many words:
    "each voice of the OB-Xa contains two filters, a 2-Pole and a 4-Pole", and
    the service manual gives them SEPARATE TRIM SETS, T11-T14. Sixteen filter
    chips in an eight-voice synthesiser.

    So the two slopes differ in CUTOFF CALIBRATION and RESONANCE BEHAVIOUR, not
    only in steepness, and switching between them is a jump between two
    instruments. That is the "scream": 12 dB/oct leaves the upper harmonics
    alive, so the resonance sits on top of a bright body instead of a dark one.
    A pure linear blend between them gives a filter that is never quite either,
    which is why SLOPE is continuous WITH DETENTED ANCHORS and each anchor
    carries its own trims.

    Both anchors run EVERY SAMPLE, both hold their own state, and the travel
    crossfades between their outputs - which is what the CD4053 does. Turning one
    off to save the arithmetic would make un-detenting click, and would make this
    one filter with a steepness control again.

    AND A CELL WHOSE OUTPUT IS DISCARDED IS SAFE TO ARRIVE, WHICH IS STRUCTURAL
    RATHER THAN HOPED FOR. It is the obvious worry about running a cell nobody is
    listening to - that it drifts, and that the instant its weight leaves zero a
    state built for some other setting is mixed in at full level - and it does not
    happen here for a reason that is worth stating once: SLOPE REACHES NO STATE AND
    NO COEFFICIENT. Every anchor's poles, loop gain, body and weights are built
    from the CURRENT cutoff, resonance, shape, width and drive, every sample,
    whether that anchor is heard or not; `slopeDb` enters this filter only through
    `morph` and `morph1`, which pick the output mix. So a cell that has been
    discarded for minutes is at every instant in exactly the state it would be in
    if it had been heard all along. A weight is not a state.

    MEASURED, because a structural argument is still an argument: a held 150 Hz saw
    with the slope jumped in ONE sample - 24 to 6, 6 to 24, 12 to 24, 24 to 12,
    9 to 18 - against a run that had been at the destination from the start, over
    every type, three cutoffs, four resonances, four shapes and both drive ends.
    24,576,000 samples compared as raw bit patterns, 0 differing words. Not "no
    audible click": the same samples. `dsp_superfilter_tests` keeps that check.

    THERE ARE THREE OF THEM SINCE 2026-09-27 and the law did not change: the
    ONE-POLE cell below is a third separately trimmed instrument, it runs every
    sample beside these two, and the travel is two legs of the same crossfade.
    What it costs is in the measured table beside `setOversampling`.

    A 36 dB CELL SAT HERE FOR HALF A DAY AND WAS WITHDRAWN BY DECISION, NOT BY
    A FAULT, and the reason is arithmetic rather than taste. One octave above
    the corner, relative to the passband, a slope step removes about the same
    number of decibels whichever step it is - what differs is what it removes
    them FROM:

        poles   dB/oct   1 octave up   2 octaves up
          1        6        -7.0 dB      -12.3 dB
          2       12       -14.0         -24.6
          4       24       -28.0         -49.2
          6       36       -41.9         -73.8

    1 -> 2 takes 7 dB off material sitting at -7, which is plainly audible.
    4 -> 6 takes 14 dB off material already at -28, which nobody can hear was
    there. The author, 2026-09-27, having listened to the six-pole that was measurably
    real and correctly trimmed: *"I dont really hear a big difference"*, and
    then *"isnt it a waste then?"* - and it cost x1.65 CPU at EVERY slope
    including 12 and 24, where its weight was exactly zero. So the third anchor
    went to the OTHER end of the travel, where the same arithmetic says the step
    is worth hearing. The measurements that fixed the six-pole's four trims were
    right; the position on the control was wrong. `~/projects/arcturus/
    synth-design.md` keeps that record rather than deleting it.

    THE FOUR TRIMS, and each one is a different claim with a different source.
    The one-pole cell below declares TWO of them and says out loud why the other
    two do not exist for it. */
struct FilterAnchor
{
    /** T13 / T14, INIT FREQ. [EAR], sized on the OB-Xa catalogue entry's [DER]
        "~18 cents of never-corrected per-card spread": the two chips are trimmed
        to the same pitch and land 18 cents apart, so ours sit +/- 9. A shared
        curve cannot produce two different corners at one slope setting, which is
        what `megasynth_filter` check 4a measures. */
    float cutoffTrimCents = 0.0f;

    /** The LOOP GAIN the top of the resonance knob reaches, and the two anchors
        take it from two different manuals, which is the most direct statement
        available that these are two trimmed parts:

          * 4-pole: [MAN] Elka Synthex p7 - the filter self-oscillates at 7 on a
            knob marked 0..10 and the manual names the result a SINE. So unity
            loop gain arrives at 0.7 of the control and the top three tenths are
            past it, by design.
          * 2-pole: [MAN] Oberheim SM3 - the 2-pole is trimmed to sit JUST UNDER
            self-oscillation at maximum RESONANCE, so a to-spec OB-Xa's 2-pole
            does not oscillate at all, and the catalogue entry puts a NUMBER on
            "just under": kTwoPoleMaxQ = 8. A loop gain of 1 - 1/8 reaches
            exactly that Q, where the 0.98 this carried first was a guess at the
            same words and reached Q 50 - measured, an 11x peak on a 0.4 input,
            which is not a filter trimmed to the edge, it is one over it. [MAN]
            SM1 p11 adds that the 4-pole has NO resonance trim, so its resonance
            is whatever the chip gives.

        Two manuals, two calibrations, and neither is derivable from the other. */
    float loopGainAtFullResonance = 1.0f;

    /** [FW] Nord Lead 3: EACH SLOPE RUNS THE SAME BLOCK WITH A DIFFERENT
        INTERNAL HEADROOM SHIFT - `asr #4` for 24 dB against `asr #5` for 12,
        with the matching `asl` on the way out. The three blocks of a triple
        differ in NOTHING ELSE. So the slope is a GAIN SHIFT and not a pole
        count, and a shift with a matching shift back is transparent to a LINEAR
        filter and NOT transparent to one with a saturator in it.

        That is the whole content of the term here: the RATIO is the firmware's
        (24 dB runs at twice the level of 12), the absolute is [EAR] and chosen
        so the limiter reaches a useful part of its curve. DRIVE therefore bites
        harder at 24 than at 12 - which is the Nord's finding and the OB-Xa's
        "two instruments" landing in the same place. */
    float headroom = 1.0f;

    /** How much of the resonance's own DC loss is given back. See kBodyExponent
        below; this is per anchor because the two topologies lose it differently
        and [MAN] says so out loud. */
    float bodyExponent = 0.0f;
};

/** [MAN] Oberheim, explicitly: "As the RESONANCE is increased, the overall
    volume of the sound will INCREASE in the 2-POLE mode and DECREASE in the
    4-POLE mode." There is no make-up amplifier between the 3320s and the
    pole-select 4053 on sheet 1520, so the level change is part of the sound.

    IT FALLS OUT OF THE TWO TOPOLOGIES RATHER THAN BEING IMPOSED. The 4-pole is
    a NEGATIVE feedback loop closed on the last pole, so its DC gain is
    1/(1+k) and rising resonance takes level away. The 2-pole is a POSITIVE
    feedback loop closed on its own bandpass tap, whose gain at DC is exactly
    zero - so the feedback term vanishes at DC, the DC gain stays at 1 whatever
    the resonance, and only the peak grows. Two directions, no calibration.

    WHAT IS CALIBRATED IS HOW MUCH OF THE 4-POLE'S LOSS COMES BACK, and it is an
    [EAR] number on top of a [MAN] law. Uncompensated the 4-pole loses 12.6 dB at
    the resonance the bank's "Acid Reso" carries, which is not a character, it is
    a program nobody can hear. `(1+k)^0.75` restores three quarters of it in the
    log domain and leaves 3.2 dB of the [MAN] direction intact, measured. The
    2-pole gets none, because it has none to give back.

    This is body compensation "measured per configuration against ITSELF" in the
    spec's sense: it is derived from THIS anchor's own DC gain, not from blending
    in a common reference - which is the mistake that makes every model converge
    exactly where its character is strongest. */
inline constexpr float kBodyExponent4 = 0.50f;   // [EAR] on top of [MAN]
inline constexpr float kBodyExponent2 = 0.0f;    // [MAN] nothing to give back
/** [EAR] where the makeup stops. See the note beside its use in `set`. */
inline constexpr float kBodyMakeupMax = 1.9f;

inline constexpr FilterAnchor kAnchor12 { +9.0f, 0.875f, 0.5f, kBodyExponent2 };
inline constexpr FilterAnchor kAnchor24 { -9.0f, 1.05f, 1.0f, kBodyExponent4 };

// ----------------------------------------- the third anchor, ONE pole, 6 dB ---
/** ONE POLE, 6 dB/OCTAVE, AT THE BOTTOM OF THE TRAVEL. The author, 2026-09-27, after
    reading the audibility arithmetic in `FilterAnchor` above: *"ok lets drop the
    6pole and do a 1 pole instead."*

    IT IS NOT AN INVENTION, WHICH IS THE ONE STRUCTURAL DIFFERENCE FROM THE CELL
    IT REPLACES. `~/projects/synthcatalog/filters` holds 45 entries over 26
    machines and had NOTHING at 36 dB/oct - the steepest voice filter in the tree
    is four poles. At 6 dB/oct it has four machines and three separate claims:

      * [FW]+[MAN] Novation Supernova II - A ONE-POLE IS WHAT A MACHINE WITH A
        SLOPE CONTROL REACHES FOR. `supernova2.md`: "12 dB is one SVF section;
        18 dB is that plus a TPT one-pole; 24 dB is a second SVF section in
        SERIES sharing the same damping coefficient." Adding or removing a single
        pole IS that machine's slope step, and this cell is the same move at the
        other end of the same control.
      * [MAN] Roland Jupiter-8, JX-8P and MKS-70 - A ONE-POLE IS A PLAYABLE
        POSITION, not a degenerate case. All three carry "a one-pole HPF ahead of
        the VCF, on its own panel slider, out of circuit at zero"
        (`jupiter8.md`, grade B, and the same line in `jx8p.md` and `mks70.md`).
        A first-order filter on a front panel, on three machines, over five years.
      * [MAN] TC Electronic TC 2290 - AND IT HAS NO RESONANCE. `tc2290.md`,
        grade A-: "discrete single-pole RC networks ... about 6 dB/oct ... No
        resonance. Single-pole RC networks." That is the law this cell has to
        obey, printed by a machine rather than argued from theory.

    WHAT IS OURS is the two fitted trims below and the two structural laws that
    come after them, and each one carries the measurement that fixed it.

    WHAT IT COSTS IS THE POINT OF THE CHANGE. One `onePole`, no feedback loop, no
    limiter, no crossing to solve - the cheapest cell in this file. The measured
    figure is in the table beside `setOversampling` and it is BELOW the
    two-anchor filter's, not above it: the six-pole's x1.67 is given back and a
    little more, because removing one transcendental crossing solve from `set`
    buys more than adding one pole to `process` costs. */

/** =============================== AND THE ARTEFACT THAT WENT WITH IT ========
    A THING A PLAYER COULD HEAR AND NO GATE COULD SEE, which is the fault this
    whole session turned out to be about. The author, 2026-09-27, playing the six-pole
    build: *"a tiny like bell/piano sound that plays on each key hit as well as
    the fm/pcm preset"*, then *"it changes depending on pitch of key. and is more
    noticeable with the 6 pole filter"*, then *"but shape morph between BP and HP
    and its so audible"*, and the detail that identified it: *"weird and sometimes
    its not there. I could trigger it a couple of times by turning filter on and
    off or clicking track on and off or changing poles."*

    IT IS A RECONFIGURATION ARTEFACT AND NOT A NOTE ARTEFACT. Intermittent,
    triggered by an abrupt change to the filter's coefficients rather than by a
    key, and ringing at a pitch that follows the key because it rings at the
    LOOP'S OWN CROSSING, which is a fixed fraction of the cutoff, and the cutoff
    follows the key. Filter on/off, TRACK on/off and the pole switch are all the
    same event: the cutoff or the weights jump in one sample.

    THE MEASUREMENT THAT FOUND IT, and the meter is the whole trick. The input is
    periodic with an integer period, so a run that SETTLED at the destination
    setting and a run that ARRIVED there are sample-aligned; their difference IS
    what the reconfiguration added and nothing else. An envelope test sees nothing
    - the ring is smaller than the steady peak at plenty of settings. A 150 Hz saw
    at 0.5, jumping shape, cutoff and width over five slopes, two cutoffs and three
    resonances; `over` is the transient's peak against the steady peak and `holds`
    is how long it stays a TENTH of it:

        build                     type 0         type 1         type 2/3
        six-pole, all slopes   +16.05 / 67.1  +13.18 / 30.0  +6.83 / 89.8 dB / ms
        six-pole, up to 24      +8.22 /  6.4  +13.18 / 30.0  +6.83 / 89.8
        this build              +8.22 /  6.4  +13.18 / 30.0  +6.83 / 89.8

    +16 dB FOR 67 ms IS A BELL AND +8 FOR 6 ms IS A CLICK, and the six-pole row's
    ring sits at 650 Hz for a 1 kHz corner - its own crossing at `tan(30 deg)` of
    the corner, nowhere near the note. Under an absolute floor rather than a
    relative one the same event measures 130 ms against this build's 22.7.

    AND THE WORST CASE IS MID-LEG, NOT AT THE DETENT, which is the thing a gate
    gets wrong. At slope 36 the steady peak IS the six-pole's own resonant output,
    so the ratio is small and the fault hides; at slope 30 the steady peak is a mix
    of two anchors while the transient is still the six-pole's full ring. A gate
    that visits only the printed numbers cannot see a crossfade's own worst case.

    SO IT WENT WITH THE ANCHOR, and the two rows that matter are the last two: at
    the slopes this build shares with the one it replaces the numbers are IDENTICAL,
    because those slopes are bit-identical. What is gone is the row above them, and
    it is gone three ways - the six-pole cell does not exist, SLOPE cannot reach
    past 24, and the one-pole has no loop to ring in. The one-pole contributes
    nothing to the worst case; that is at slope 24.

    WHAT REMAINS IS EACH LOOP'S OWN STEP RESPONSE and none of it is new. A resonant
    filter handed a two-octave cutoff jump in one sample rings, and CASCADE's 89.8
    ms at a 200 Hz corner is a slow SETTLE rather than a ring - which is why the
    bound that catches the fault is the amplitude on type 0 and not the duration on
    anything. One item is OPEN and is printed rather than hidden: the SVF's
    +13.18 dB for 30 ms on a WIDTH jump at resonance 0.9 is its own Q-32 pair kicked
    by a corner step, it is in every version of this filter, and nothing here moved
    it.

    THE CONSUMER OWNS THE OTHER HALF: smooth the cutoff. It arrives here already
    converted, once, from `CutoffTerms::hz()`, so a slew belongs in the module and
    not in this block, and a TRACK switch that steps the cutoff two octaves in one
    sample will click on any filter ever built.

    THE GATE IS IN `dsp_superfilter_tests` NOW and it fails the six-pole build: it
    jumps shape, cutoff and width on a held tone over every type and five slopes
    INCLUDING the mid-leg ones, and holds type 0 under 11 dB of the steady peak.
    Type 0 measures 8.22 here and 16.05 there. */

/** [EAR], MEASURED, AND IT CONTINUES THE EXISTING STEP IN THE OTHER DIRECTION.
    The 12 and 24 anchors sit at +9 and -9 cents, which is the OB-Xa catalogue
    entry's [DER] "~18 cents of never-corrected per-card spread" split either
    side of nothing. A third separately trimmed cell sits another 18 along that
    same line, and the six-pole went DOWN to -27; a one-pole is a step the other
    way, so it goes UP to +27.

    MEASURED THE WAY `cutoffTrimCents` WAS MEASURED FOR THE OTHERS: at width 0
    and resonance 0 an N-pole cascade of identical poles is exactly 2^(-N/2) at
    its own pole frequency, so the meter reads the half-power point for this
    cell, twice that in decibels for the 2-pole and four times for the 4-pole.
    At a 1200 Hz cutoff, width 0, resonance 0, DRIVE 0:

        slope  6   -3.01 dB corner   1219.247 Hz   +18.0 cents over the 2-pole
        slope 12   -6.02 dB corner   1206.628 Hz
        slope 24  -12.04 dB corner   1194.137 Hz   -18.0 cents under the 2-pole

    THE SEPARATIONS COME OUT AT EXACTLY THE DECLARED 18, at both pairs, which is
    the claim: the three cells sit on one line 18 cents apart and the trim reaches
    the audio rather than only the source. The absolute figures carry a +0.5 cent
    bias from taking the passband reference at 20 Hz rather than at DC, which
    cancels in the separations.

    AND THE TARGET IS -3.0103 dB PER POLE AND NOT -3, which is worth writing down
    because it is the one way to measure this wrong and get plausible numbers. A
    half-power point is 1/sqrt(2), which is -3.0103 dB; rounding it to -3N reads
    1216.368 / 1203.779 / 1191.318 Hz - every corner about 4 cents low, by the
    same amount for every N, because the offset in decibels and the local slope in
    decibels per octave both scale with N. The separations still come out at
    +18.0 and -18.0, so a reader checking only those would never see it. */
inline constexpr float kOnePoleTrimCents = +27.0f;

/** AND THE OTHER TWO TRIMS DO NOT EXIST FOR THIS CELL. `FilterAnchor` carries
    four and this anchor declares two, which is why it is two constants rather
    than a fifth `FilterAnchor`: writing 0.0 into `loopGainAtFullResonance` and
    `bodyExponent` would put a number that looks like a measurement where there
    is no measurement to make.

    ONE POLE CANNOT RESONATE, AND THE FILE HAS TO SAY SO RATHER THAN LEAVE IT TO
    BE DISCOVERED. Two poles are the minimum for a peak: a resonant loop needs
    its own phase to reach -180 degrees somewhere a player can hear, and one TPT
    one-pole only reaches -90 at Nyquist. [MAN] the TC 2290 states the same thing
    from the other side - "No resonance. Single-pole RC networks" - and [MAN] the
    Jupiter-8's one-pole HPF is a passive slider with no resonance control beside
    it. So there is no loop here, no `k`, no limiter and nothing for
    `loopGainAtFullResonance` to calibrate.

    `bodyExponent` goes with it. It gives back the DC gain a NEGATIVE-feedback
    loop loses as its resonance rises - `1/(1+k)`, [MAN] Oberheim's own direction
    - and a cell with no loop loses nothing, so there is nothing to give back.
    Its DC gain is exactly 1 at every resonance, which is measurable and is the
    2-pole's [MAN] law reached by having no feedback at all rather than by closing
    the loop on a tap that is zero at DC.

    WHAT THE RESONANCE KNOB THEREFORE DOES AT THE BOTTOM OF THE TRAVEL: nothing,
    and THE CROSSFADE IS WHAT TAKES IT AWAY. The travel's lower leg fades between
    the 2-pole's output and this one's, so the peak the 2-pole builds is
    attenuated linearly in amplitude as the knob approaches 6 and is gone at the
    detent. That is the CD4053 arrangement doing the work, which is the same
    answer the other two legs give; it needs no scaling of the resonance this
    cell is handed, because this cell is not handed any.

    MEASURED, a 1 kHz corner, width 0.5, lowpass, DRIVE 0, a swept sine at 0.001
    (small signal, so the output rail is out of play and cannot flatter the
    result), the LARGEST response anywhere within two octaves of the corner
    relative to the 40 Hz passband:

        slope     res 0.0   res 0.5   res 0.8   res 1.0
         12.0      -0.52     +0.91     +5.57    +13.11 dB
         10.8      -0.52     +0.91     +5.57    +13.11   <- the 12 detent
          9.0      -0.45     -0.02     +2.21     +8.12
          7.5      -0.25     -0.25     -0.24     -0.25
          7.2      -0.25     -0.25     -0.25     -0.25   <- the 6 detent
          6.0      -0.25     -0.25     -0.25     -0.25

    and at a played level of 0.4 the same table reads +11.20 at the top of the 12
    row, +6.78 at 9, and the identical -0.25 from 7.5 down.

    THERE IS NO PEAK AT ALL IN THE BOTTOM THREE ROWS AND THE NUMBER IS NEGATIVE,
    which is a stronger statement than "small". A one-pole lowpass is monotonic,
    so the largest response in the band IS its low edge, which sits 0.25 dB under
    the 40 Hz reference - a peak would have to read POSITIVE to exist. The bottom
    two rows are the detent rather than a tolerance: `slopeMorph6` returns exactly
    1.0f from 7.2 down, so the output IS this cell and the resonance reaches no
    coefficient it uses.

    AND IT IS ALREADY GONE AT 7.5, a quarter of the way up the leg, which is the
    crossfade being multiplicative rather than gentle: 0.74 per cent of a 3.6x
    peak riding on 99.26 per cent of a flat response lifts the result by 0.02 dB,
    which is under the one-pole's own 0.25 dB shelf. Monotonic the whole way, and
    a crossfade between a peaking response and a flat one cannot grow a peak of
    its own - both terms are non-negative and the flat one has no maximum to add. */

/** [FW]-SHAPED, [EAR]-SIZED, AND MEASURABLY INERT - WHICH IS THE FINDING.
    The Nord Lead 3's slope is a HEADROOM SHIFT, `asr #5` for 12 dB against
    `asr #4` for 24, a factor of two per step, and this continues that one step
    the other way: 0.25 where 12 is 0.5 and 24 is 1.0.

    THE WHOLE CONTENT OF THAT FINDING IS THE NONLINEARITY IT SHIFTS INTO, and
    this cell has none. `FilterAnchor::headroom` says it in as many words - "a
    shift with a matching shift back is transparent to a LINEAR filter and NOT
    transparent to one with a saturator in it" - and a one-pole with no feedback
    loop is exactly a linear filter. 0.25 is a power of two, so the shift and its
    inverse are exact in float and the pair is a true no-op rather than a nearly
    one.

    SO DRIVE DOES NOTHING AT SLOPE 6, AND THAT WAS MEASURED RATHER THAN ASSUMED.
    The other anchors are sized on the resonant peak's height over the passband as
    DRIVE goes 0 to 1; there is no peak here, so the output's own rms on a 110 Hz
    saw stands in. A 1 kHz corner, lowpass, width 0.5, played level:

        slope 24, res 0.75   peak +15.07 -> +2.68 dB   DRIVE moves it 12.39 dB
        slope 12, res 0.75   peak  +4.37 -> +2.03      DRIVE moves it  2.34
        slope  6, res 0.00   peak  -0.25 -> -0.25      DRIVE moves it  0.00
        slope  6, res 0.75   peak  -0.25 -> -0.25      DRIVE moves it  0.00
        slope  6, res 1.00   peak  -0.25 -> -0.25      DRIVE moves it  0.00
                             rms 0.540230 -> 0.540230 at all three

    AND IT IS NOT "0.00 dB TO TWO PLACES", IT IS THE SAME SAMPLES. Rendered twice
    at slope 6 with DRIVE at 0 and at 1, a 110 Hz saw, resonance at the top, one
    second at 48 kHz: 0 of 48000 output words differ by a single bit, at the
    lowpass and at the highpass both. The other branch of the crossfade is
    multiplied by `1.0f - 1.0f`, so nothing DRIVE touches reaches the output at
    all.

    IT IS IN THE AUDIO PATH ANYWAY, and that is a decision. The cell reads like
    the other two, a reader comparing the three sees one shape three times, and
    the day this cell is given a nonlinearity the shift is already in place with
    the right number. What must not happen is a reader concluding that DRIVE bites
    here: the table above is the answer and it is zero bits. */
inline constexpr float kOnePoleHeadroom = 0.25f;

/** AND IT CANNOT PRINT A SHAPE WITH TWO EDGES. That is the second law, it is
    arithmetic rather than a limitation, and it decides what SHAPE does at the
    bottom of the travel.

    A FIRST-ORDER NETWORK HAS EXACTLY TWO MAGNITUDE RESPONSES. With one pole the
    output is `a in + b lp`, which is `a` at Nyquist and `a + b` at DC, so a band
    - small at both ends - needs `a = 0` and `a + b = 0`, which is silence. There
    is no first-order bandpass and no first-order notch, at any width: WIDTH is a
    separation between two pole groups and one pole has nothing to separate from.

    [MAN] THE SYNTHEX ALREADY ANSWERS THIS, AND ITS ANSWER IS THAT THE 6 dB BAND
    IS ALREADY ON THE 12 dB ANCHOR. Its printed BP1 is 6 dB/oct and it is
    `2(y1 - y2)` - TWO taps, `elka/synthex.md` [MAN] p8 - which is exactly this
    filter's 12 dB anchor bandpass: `aBp2` is `sBp2 (t1 - t2)` = LP(hi) HP(lo), a
    6 dB/oct skirt on each side. So the bandpass at the bottom of the SLOPE travel
    arrives at the same printed mode it arrives at at 12, because there is nothing
    shallower to reach - a 3 dB/oct edge is half a pole.

    SO THE LOWER LEG DOES NOT TRAVEL AT THE TWO-EDGED SHAPES, and this table is
    that: full reach at the lowpass and the highpass, none at the bandpass and the
    notch, interpolated with the same `shapeFrac` as everything else so the morph
    stays level-continuous. The precedent is one section down - `kBandnessOneEdge`
    scales WIDTH's travel by the same edge count for the same structural reason,
    [MAN] the Supernova's WIDTH doing nothing at all on a plain LPF.

    AND IT IS WHAT MAKES THE CELL'S OWN VECTOR LEGAL. [MAN] Electrix Mo-FX,
    `electrix/mofx.md`: "two complementary one-pole splits in cascade, so that
    HIGH + MID + LOW is exactly the input at every frequency". A first-order
    lowpass and highpass SUM TO THE INPUT, exactly - so any blend of them is a
    shelf and the halfway blend is FLAT. A cell morphing its vector from `{0,1}`
    to `{1,-1}` passes through `{0.5, 0}`, which is half the input with no filter
    on it at all. This cell therefore does not morph between them: it IS the
    lowpass below the bandpass point and the highpass above it, and the jump sits
    at the bandpass, where its reach is exactly 0 and the output is continuous.

    WHAT A PLAYER GETS AT SLOPE 6, then, as SHAPE runs 0 to 3: a one-pole lowpass,
    crossfading into the 12 dB anchor's printed BP6, back out of it into a
    one-pole highpass, then into the 12 dB anchor's notch. Every point on it is
    either a first-order response or a [MAN] Synthex mode, and RESONANCE reaches
    the two middles and not the two ends - which is honest, because one pole has
    no resonance and a notch that cannot null is not a notch. */
inline constexpr float kOnePoleOneEdged[4] { 1.0f, 0.0f, 1.0f, 0.0f };

/** WHERE THE DETENTS SIT. The travel is continuous and the ANCHORS ARE FLAT: a
    fifth of each leg at each end is purely one chip, with a smootherstep
    between. A knob marked 6, 12 and 24 has to ARRIVE at all three rather than
    pass near them, or the calibrated instruments become one average that is
    never quite any of them. [EAR] for the width of the flat; [SCH] for why there
    has to be one.

    IT IS A FIFTH OF EACH LEG AND THE LEGS ARE NOT THE SAME WIDTH, which is
    worth stating because the knob is linear in dB/oct and the anchors are not
    evenly spaced on it. The lower leg spans 6 dB and the upper one 12, so the
    detents are 1.2 dB wide at the bottom and 2.4 dB wide at the top. In POLES
    the two legs are one step each; in dB/oct the first is half the second. The
    control is printed in dB/oct because that is what a player reads off a panel
    and what every stored program holds. */
inline constexpr float kSlopeDetent = 0.2f;

/** THE TRAVEL'S TWO ENDS, now that there are three anchors. 6 to 12 is the lower
    leg and 12 to 24 the upper one, each with the same detent at each end, so the
    knob ARRIVES at all three printed numbers and holds there. A consumer whose
    SLOPE parameter still runs 12..24 reaches exactly what it reached before -
    the lower leg costs a stored program nothing, because no stored program can
    ask for it.

    WHAT A CONSUMER HAS TO DO TO REACH IT, and it is one number in one place.
    megasynth declares SLOPE as `{ "slope", "Filter Slope", "float", 12.0f,
    24.0f, 24.0f, "dB/oct", ... }` in `src/generated/MegaSynthParams.h` from
    `data/`, and Solera as `f("fSlope","Filter Slope",12,24,24.0f,"dB")` in
    `PluginProcessor.cpp`; the minimum becomes 6 and the default stays 24, so
    every stored program lands on the 4-pole detent exactly where it landed
    before. The panel's third cap writes 6 the way its 2-POLE and 4-POLE caps
    write 12 and 24. NEITHER CHANGE IS MADE HERE, because a parameter range is
    the machine's and not the block's, and because changing megasynth's generated
    table is a separate job with its own gate
    (`a_direct_write_reaches_the_audio`). */
inline constexpr float kSlopeMinDb = 6.0f;
inline constexpr float kSlopeMaxDb = 24.0f;

/** WHERE THE JP-8000's DAMPING CORRECTION STARTS, as a fraction of this
    machine's own log sweep, and HOW MUCH it adds at the very bottom.

    `[FW]` `0x1D7A4` gives both: the threshold is corner word `0x3B3D` of
    `0xFFFF`, which is 0.232 of that machine's travel, and the multiplier
    `(0x4A4B - w/8) >> 14` reaches 1.16 at w = 0, which is 16 per cent more
    damping and about 1.3 dB less resonance. Expressed here as a fraction and a
    proportion so they read as what they are rather than as two magic numbers.
    `~/projects/synthcatalog/filters/roland/jp8000.md`, grade A-. */
inline constexpr float kJpEaseFrom = 0.232f;
inline constexpr float kJpEaseMax  = 0.16f;

[[nodiscard]] inline float slopeMorph (float slopeDb) noexcept
{
    const float t = std::clamp ((slopeDb - 12.0f) / 12.0f, 0.0f, 1.0f);
    const float u = std::clamp ((t - kSlopeDetent) / (1.0f - 2.0f * kSlopeDetent),
                                0.0f, 1.0f);
    return u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);
}

/** THE LOWER LEG, 12 DOWN TO 6, AND IT RETURNS THE ONE-POLE'S OWN WEIGHT - so it
    runs DOWNWARD where `slopeMorph` runs up. Same curve, same detents, so the
    knob feels identical either side of 12.

    IT IS A SEPARATE FUNCTION RATHER THAN A WIDER `slopeMorph`, and that is the
    whole promise of this addition: at 12 and above this returns exactly 0.0f,
    `slopeMorph` is untouched, and the filter takes the arithmetic it always
    took. The 12 and 24 positions do not move because nothing on their path
    changed. `t` is written as `(12 - slopeDb) / 6` rather than as one minus the
    other direction so that 12 gives t = 0 and the smootherstep returns a true
    zero rather than a subtraction that happens to land there. */
[[nodiscard]] inline float slopeMorph6 (float slopeDb) noexcept
{
    const float t = std::clamp ((12.0f - slopeDb) / 6.0f, 0.0f, 1.0f);
    const float u = std::clamp ((t - kSlopeDetent) / (1.0f - 2.0f * kSlopeDetent),
                                0.0f, 1.0f);
    return u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);
}

// ============================================================== the types ===
/** THREE FILTERS, AND THE ONE THAT IS ALREADY HERE IS THE FIRST OF THEM.
    The author, 2026-09-14: "I also liked the 2-pole, 4-pole filters and PWM stuff that
    the super synth had. We might need to add that as additional filters because
    they sounded amazing."

    `super-filter.md` says there is NO MODEL SELECTOR, and that still holds for
    what it was arguing against: a menu of six machines behind one knob, where
    the player picks a brand. This is not that. These are three TOPOLOGIES, and
    they differ in the one place a topology can - WHAT RESONANCE DOES TO THE
    BODY - which is the only filter question the shape morph cannot answer:

      SUPER   the passband LOSES as the resonance rises, then three quarters of
              it comes back. [MAN] Oberheim's own direction, compensated.
      SVF     the passband GAINS as the resonance rises, in both slopes, and the
              12 dB one screams. [FW] Clavia, two stages and two tables.
      CASCADE the passband LOSES and NOTHING GIVES IT BACK. [SCH]+[DS] the
              SSM2044, where the thinning is the instrument.

    Measured at a 1 kHz corner with the resonance at the top, the passband sits
    at -7.4 dB, +3.0 dB and -14.2 dB. Those are not three voicings of one filter.

    EVERY OTHER CONTROL KEEPS ITS MEANING INSIDE EVERY TYPE, which is the rule
    that stops a type list from becoming a second parameter set. SHAPE is still
    the continuous LP -> BP -> HP -> notch morph and still reduces to a printed
    machine mode at each of its four points; SLOPE is still two separately
    calibrated anchors with a detent at each end; WIDTH is still a separation in
    semitones about the corner; DRIVE is still the Virus's one limiter, in the
    loop. What changes is the loop they are all sitting in.

    TYPE 0 IS BIT-IDENTICAL TO WHAT THIS FILTER DID BEFORE THE ARGUMENT EXISTED,
    and it is the default, so no stored program moves by any amount at all. The
    probe measures that rather than asserting it: check 9a runs the factory
    bank's own 205 filter settings through both and prints the largest
    difference, which is 0.000 dB. */
inline constexpr int kTypeSuper = 0;   ///< the fused filter. unchanged, default
inline constexpr int kTypeSvf   = 1;   ///< [FW] Clavia Nord Lead 2, two SVF stages
inline constexpr int kTypeCascade = 2; ///< [SCH]+[DS] SSM2044, four poles, no makeup
/** [MAN]+[SCH] the Alesis Andromeda A6's PAIR: a 4-pole lowpass with its own
    three output faders and a 2-pole multimode beside it, both running on every
    voice. Wired 2026-09-17 at the author's ask, after they remembered the machine: "the
    cutoff on the andromeda was so amazingly big... I remember the filter felt
    very big and warm." The block is src/engine/TwinFilter.h, built on
    2026-09-15 from ~/projects/synthcatalog/filters/alesis and left without
    parameters until now.

    IT IS THE ONE TOPOLOGY WHOSE SHAPE CONTROL IS THE HARDWARE'S OWN: on the
    A6 the LP, BP and HP taps are three FADERS you mix rather than a selector,
    which is exactly what this machine's continuous SHAPE morph already is. The
    other three topologies approximate that morph; this one IS it. */
inline constexpr int kTypeTwin = 3;
inline constexpr int kTypeCount = 4;

// ------------------------------------------------- type 1: the two SVF stages
/** [FW] `~/projects/synthcatalog/filters/clavia/nordlead2.md`, grade A. TWO
    IDENTICAL TWO-POLE STATE-VARIABLE STAGES IN SERIES, and the filter TYPE on
    that machine is nothing but which taps the two stages read - four mixing
    coefficients the host writes per voice at `voice[124..136]`, `[FW] 0x1054de`.
    The DSP code does not change. Six modes out of one piece of code and eight
    numbers.

    This is also the filter supersynth shipped as its default and the one the author is
    asking for back: `Source/Voice.h`'s `SvfFilter` is two cascaded TPT
    state-variable stages with `poles` choosing one or two, which is the same
    topology reached from the other end. What supersynth does NOT have is the
    machine's own resonance arithmetic - it uses `k = 2(1 - res)`, a straight
    line - and that is where all four of the differences below live.

    THE TWO RESONANCE LAWS, AND THE SLOPE PICKS WHICH. `[FW] 0x104588` builds
    two 128-entry tables of the damping term `1/Q` in Q23 at boot:

        types 1-5 (24 dB):  d[i] = 0x0FFFFFFF / ((i*i   >> 4) + 32)
        type 0    (LP 12):  d[i] = 0x0FFFFFFF / ((i*i*i >> 7) + 32)

    At i = 0 both are `0x0FFFFFFF / 32 = 0x7FFFFF`, exactly 1.0, which is what
    pins the fixed-point convention and is why THIS FILTER IS NOT FLAT AT
    RESONANCE ZERO: Q is 1, not 0.707, so each stage carries a 1.25 dB bump the
    machine was built with. The quadratic bottoms at 0.0308, Q 32.5, and the
    cubic at 0.00199, Q near 500. Written here as the same integers divided by
    the same 32, which is the table exactly with the index made continuous.

    Give every mode one curve and the Lead 2's most characteristic sound - the
    screaming self-oscillating 12 dB the manual itself recommends for TB-303
    patches - simply does not exist, and the 24 dB modes gain a self-oscillation
    the machine does not have. That is the whole reason this is a type and not a
    trim: the two slopes are two different filters on this machine as surely as
    the OB-Xa's are two different chips. */
[[nodiscard]] inline float nordDampingQuadratic (float res01) noexcept
{
    const float i = std::clamp (res01, 0.0f, 1.0f) * 127.0f;
    return 32.0f / (32.0f + i * i * (1.0f / 16.0f));
}
[[nodiscard]] inline float nordDampingCubic (float res01) noexcept
{
    const float i = std::clamp (res01, 0.0f, 1.0f) * 127.0f;
    return 32.0f / (32.0f + i * i * i * (1.0f / 128.0f));
}

/** AND THE MACHINE DROPS ITS OWN INPUT AS THE RESONANCE RISES, ON THE WAY IN.
    `[FW] 0x104608`, table at `a4+16488`: `g = (d/10000 + 40) >> 1`, where `d` is
    the damping word the tables above just built. It runs 439 at resonance 0 down
    to 32 on the quadratic table and down to 20 on the cubic one - a factor of
    13.7 on the 24 dB slope and 22.0 on the 12 dB one, which is the catalog's own
    two figures falling out of one formula and is the check that this reading is
    right.

    IT IS NOT AN OUTPUT COMPENSATION AND MUST NOT BE MOVED TO THE OUTPUT. The
    oscillator hits the filter SOFTER at high resonance, so the resonant peak is
    what you hear rather than a peak riding on a body that is already at the
    rail. Leave it out and the resonant modes clip; normalise it on the output
    instead and the balance between body and peak is wrong. It is also the reason
    a Q of 500 is a usable setting rather than a fault: 500/22 is a peak of 27 dB
    over an input that arrived 27 dB down, which lands back at unity.

    The `>> 1` and the integer `d/10000` are the firmware's; `d` here is the same
    damping normalised to 1.0, so the word is `d * 0x7FFFFF`. Normalised by the
    439 at the top so that resonance zero is unity gain and this type does not
    arrive quieter than the other two. */
[[nodiscard]] inline float nordInputGain (float damping) noexcept
{
    const float word = std::clamp (damping, 0.0f, 1.0f) * 8388607.0f;
    return ((word * (1.0f / 10000.0f)) + 40.0f) * 0.5f * (1.0f / 439.0f);
}

/** [FW] TYPE 0 IS THE ODD ONE AND THE MACHINE TREATS IT AS A DIFFERENT FILTER,
    NOT A DIFFERENT TAP. Its first stage is tapped `(-0.625 HP, +0.875 LP)` and
    its damping is FORCED TO 1.0 instead of the resonance value - which is how
    you make one of two fixed stages get out of the way. Read as a response that
    pair is a SHELF: 0.875 at DC, -0.625 well above the corner, so the stage
    stops contributing slope and the cascade's asymptote is the second stage's
    12 dB alone. That is where the 12 dB comes from on a machine with four poles
    permanently in the path, and it is a far better answer than switching a stage
    off, because the shelf is still there and still coloured.

    So on the 12 dB anchor these two numbers are the first stage, always, and the
    SHAPE morph acts on the SECOND stage - the one carrying the resonance. At
    SHAPE = lowpass that is the machine's printed LP 12 exactly. */
inline constexpr float kNordShelfHp = -0.625f, kNordShelfLp = 0.875f;

/** THE TAP TABLE, `[FW] 0x1054de`, as this machine's four SHAPE anchors.

    Each anchor is (stage 1 HP, stage 1 LP, stage 2 HP, stage 2 LP) and every one
    of them is a printed Nord Lead 2 mode:

        SHAPE 0  lowpass   (0, 1, 0, 1)   [FW] type 1, LP 24 dB
        SHAPE 1  bandpass  (0, 1, 1, 0)   [FW] type 3, BP - a lowpass into a highpass
        SHAPE 2  highpass  (1, 0, 1, 0)   [FW] type 2, HP 24 dB
        SHAPE 3  notch     (1, 1, 0, 1)   [FW] type 4, Notch+LP - (HP + LP) IS the
                                          notch for an SVF, into a lowpass, and
                                          the manual calls the mode exactly that

    Interpolating the four coefficients is the same move the Synthex weights make
    one section up and for the same reason: the output is linear in them, so a
    continuous vector IS what a continuous mixer would write. `megasynth_filter`
    check 9b asserts the reduction to seven digits, so this stays a
    generalisation of a printed table rather than a different filter wearing its
    name. */
inline constexpr float kNordTaps[4][4] {
    { 0.0f, 1.0f, 0.0f, 1.0f },   // lowpass   [FW] LP 24
    { 0.0f, 1.0f, 1.0f, 0.0f },   // bandpass  [FW] BP
    { 1.0f, 0.0f, 1.0f, 0.0f },   // highpass  [FW] HP 24
    { 1.0f, 1.0f, 0.0f, 1.0f },   // notch     [FW] Notch+LP
};

/** WHERE THE IN-LOOP CELL SITS AND HOW FAR IT IS FADED IN. The PLACEMENT is
    supersynth's: on the bandpass integrator state, `ic1`, which is what keeps
    its SVF bounded at `k = 0`. The SHAPE of the limiter is this machine's own -
    the Virus's asymmetric three-way limiter, with DRIVE as its gain - because
    this filter has ONE saturator and a second tanh here would be a second
    nonlinearity in a block whose whole argument is that there is exactly one.

    THE FADE IS `1 - d` AND NOT supersynth's `res * res`, and the difference is
    not cosmetic. supersynth pairs `res * res` with `k = 2(1 - res)`, so its Q
    only gets large where the fade is also large. The Nord's table reaches Q 2.97
    at a QUARTER of the knob, where `res * res` is 0.0625 and the cell does
    almost nothing: measured, the resonant peak ran straight into the block's
    output rail and pinned at +15.6 dB from resonance 0.25 upward. `1 - d` reads
    the machine's own damping word, which is the amount of loop gain there
    actually is, so the cell engages exactly where the Q is. The peak then climbs
    +2.4 to +8.9 dB across the knob, monotonically, and never reaches the rail.

    WHAT WOULD RETIRE IT: a Nord Lead 2 DSP trace showing what, if anything, that
    machine does to bound a Q of 500. The catalog does not have one and says so. */

/** THE DROP IS NOT TAKEN BACK ON THE WAY OUT, AND THAT WAS TRIED AND MEASURED
    OUT RATHER THAN QUIETLY NOT DONE.

    The argument for taking it back is a good one and it is already in this file:
    `FilterAnchor::headroom` carries `[FW]` the Nord Lead 3's `asr #4` against
    `asr #5` with the matching `asl` on the way out, and the whole content of
    that finding is "a shift with a matching shift back is transparent to a
    LINEAR filter and NOT transparent to one with a saturator in it". The Lead
    2's `g = (d/10000 + 40) >> 1` looks like the same device on the same brand,
    and taking it back would put this type's passband where supersynth's SVF puts
    its own, which is flat across the whole resonance travel.

    IT DOES NOT SURVIVE THE MEASUREMENT, and the reason is the Q. Two stages in
    series MULTIPLY their Q, so at the top of the knob this anchor's peak is
    32.5^2, which is 60 dB. The drop takes 22.5 of that away. Restore the 22.5
    and the peak is 60 dB over the passband with nothing between it and the
    block's own rail: measured, the peak pinned at +15.6 dB for every resonance
    from 0.25 upward - the rail, not the filter - so the resonance knob stopped
    being a resonance knob at a quarter of its travel and became a distortion
    amount. Every partial restore does the same thing further up: at 0.5 of it
    the peak pins from resonance 0.5, at 0.6 from 0.5 as well.

    SO THE MACHINE'S OWN ARRANGEMENT IS THE ONLY ONE THAT WORKS, which is the
    catalog's point stated as a consequence rather than as an instruction: "that
    is not output level compensation, it is on the way IN, so the oscillator hits
    the filter softer at high resonance and THE RESONANT PEAK IS WHAT YOU HEAR".
    With the drop on the input and nothing on the output, the peak climbs +2.4 to
    +8.9 dB across the knob, monotonically, and never reaches the rail, while the
    passband falls 22.4 dB. Peak over body goes from 2.4 dB to 31.4.

    THAT IS THE TRADE THIS TYPE IS, and it is worth naming because it is the one
    thing supersynth's SVF did differently: supersynth kept its body because it
    used `k = 2(1 - res)`, a straight line that reaches Q 2 where this table
    reaches Q 18.7, and no input drop at all. That is a gentler filter and it is
    nobody's machine. A player who wants a four-pole that keeps its body while it
    resonates has one already and it is type 0's 12 dB anchor, whose passband
    measures +0.06 dB end to end - `[MAN]` Oberheim's own law that the 2-pole
    GAINS level as the resonance rises. */

// --------------------------------------------- type 2: four poles, no makeup
/** [SCH]+[DS] THE SSM2044, `~/projects/synthcatalog/filters/korg/polysix.md`
    grade A and `monopoly.md` B+: FOUR ONE-POLE SECTIONS WITH FEEDBACK TO THE
    INPUT NODE, and the catalog's finding about it is a negative one - "nothing
    compensates the passband, so it THINS as resonance rises". That thinning is
    the instrument. It is what a Polysix does, it is what supersynth's ladder
    does (`Source/Voice.h` `LadderFilter`, `k = res * (fourPole ? 4.0 : 2.2)`,
    measured here at -14.2 dB of passband at the top of the knob), and it is the
    one thing the SUPER type deliberately undoes.

    THE NODE IS CLAMPED AND THAT IS NOT OPTIONAL. `[SCH]` the Polysix's summing
    node is held by a D7/D8 diode pair, "without which two factory programs
    compute 1.4 MHz". supersynth reaches the same place with a `tanh` on the
    node. Ours is the Virus limiter with DRIVE's gain around it, for the same
    reason the SVF type gets it: one saturator in this machine, and the control
    that scales it is already on the panel.

    THE TWO ANCHORS ARE supersynth's OWN TWO NUMBERS, which is the only place in
    this file where supersynth is the source rather than the catalog: feedback
    from the fourth pole at a loop gain of 4.0 for the 24 dB anchor and from the
    SECOND pole at 2.2 for the 12 dB one. A two-pole loop reaches unity earlier,
    which is why the second number is not half the first.

    OPEN, MEASURED 2026-09-27, NOT FIXED: THIS TYPE'S 12 dB ANCHOR HAS NO
    RESONANCE. Taking negative feedback from a two-pole cascade's own LP output
    cannot produce a resonant peak, because two TPT one-poles only reach -180
    degrees AT Nyquist, so `1 + k G^2` never nulls anywhere a player can hear. The
    only peak it has is the one the sample of loop delay drags down from there,
    and it lands an OCTAVE ABOVE the corner and is under 2 dB tall: measured at a
    632 Hz corner, width 0.5, DRIVE 0, playing level, the biggest lift the
    resonance knob buys anywhere is +1.9 dB at 1318 Hz on the lowpass and +1.5 to
    +1.8 dB at 1266-1398 Hz on the other three. The knob's audible effect
    at slope 12 is that it takes the passband away, -0.1 to -9.3 dB, and gives
    nothing back. megasynth's own `filter_probe` check 9a has been printing it in
    the CASCADE column all along, where the peak falls from -0.09 to -8.15 dB as
    the resonance rises, and 9a asserts only the 24 dB row so nothing went red.

    THE 24 dB ANCHOR IS FINE and it is the one every response here reads at slope
    24: negative feedback from the fourth pole DOES reach -180 degrees at the
    corner, and the pole-mix taps all read `u`, so the resonance is in the
    bandpass, highpass and notch as well as the lowpass. That was checked before
    this note was written, small signal and at playing level, on all four.

    WHAT THE FIX IS, and why it is not in yet. The loop has to close on the
    anchor's own BANDPASS tap with POSITIVE feedback, which is at zero degrees at
    the geometric centre - `[MAN]` Oberheim SM3, and exactly what type 0's 12 dB
    anchor already does (`fb2 = t1 - t2`). Wired that way this anchor measures
    +11.5 to +15.2 dB of lift, with the peak back inside an octave of the corner
    instead of an octave above it (490-585 Hz for a 632 Hz corner). It is not
    committed because the raw 2.2 then exceeds unity loop gain and the filter
    self-oscillates with no input at resonance 0.7, which is below the 0.8 that
    check 9e treats as deliberate: measured 1.95 peak after 0.58 s of silence at a
    10.9 kHz corner, width 0.5. A single-pass closed-form crossing does not rail
    it and neither does the four-pass one type 0 uses, because the 2-pole loop
    carries the WHOLE sample of delay and the analogue prototype misplaces its
    magnitude by a few per cent at those corners. It wants the same solved
    crossing type 0's loop got, which is a design pass rather than a patch. */
inline constexpr float kCascadeK24 = 4.0f;   // [supersynth] feedback from pole 4
inline constexpr float kCascadeK12 = 2.2f;   // [supersynth] pole 2; see OPEN above

/** AND THE NODE CLAMP IS NOT DRIVE, WHICH TOOK A SURFACE SWEEP TO FIND.

    THE HISTORY FIRST, because this number recorded a decision and the decision
    was right when it was made. DRIVE's travel starts BELOW unity in type 0 -
    `kDriveMinGain` is 0.35 - and that is deliberate there: the limiter sits on a
    normalised feedback TERM, and at a gain of one it would already be on its own
    knee at any resonance that rings, so the resonance could never sing. Handing
    the same 0.35 to this type made its node clamp nearly transparent, and a
    nearly transparent clamp is not a clamp. Measured then: at a 12 kHz corner
    and resonance 0.60 the cascade returned 1.94 rms out of a 0.231 rms input and
    check 9e failed on eleven settings - the one-sample-delayed loop's own
    instability near sr/4, which type 0 spends two fixed-point solves avoiding.
    So the bottom of the travel went to ONE, which is `[supersynth]`'s plain
    `tanh (in)` exactly and is `[SCH]` the Polysix's D7/D8 diode pair holding a
    node ("without which two factory programs compute 1.4 MHz").

    THE RAIL BELOW THIS ONE IS WHAT WAS ACTUALLY DEFENDING AGAINST THAT, and the
    unity gain has been paying for a defence it does not provide. Both arrived in
    the same session, so the credit was never split. Re-measured 2026-09-27, same
    110 Hz saw at 0.231 rms, 12 kHz corner, resonance 0.60, LP, width 0.5:

        node floor 0.35, `kCascadeLoopMax` REMOVED   0.992 rms
        node floor 0.35, `kCascadeLoopMax` = 0.98    0.101 rms
        node floor 1.00, `kCascadeLoopMax` = 0.98    0.101 rms

    The first row is the same failure the 1.94 above records, smaller today only
    because other things in this type have moved since it was written. The point
    is the second and third rows, which are the same number: with the rail in, the
    node gain makes no difference to the runaway at all, at any corner from 8 kHz
    to 20 kHz. It is not part of that defence and never was.

    WHAT THE UNITY GAIN COST IS THE RESONANCE ITSELF, which is the failure type
    0's own comment names in as many words - "at a gain of exactly one the
    limiter already sits on its own knee at any resonance that rings, so the
    filter arrives pre-compressed and the resonance never sings". It reads as a
    working filter because the clamp only bites once there is signal: measured on
    a swept sine at a 632 Hz corner, width 0.5, DRIVE at zero, the lift the
    resonance knob buys between 0 and 0.9 at the peak is

        input amplitude 0.001 (small signal)   LP +28.7  BP +11.2  HP +28.7  NO +11.2 dB
        input amplitude 0.5   (a played note)  LP  +6.2  BP  +5.3  HP  +6.2  NO  +5.7 dB

    and a player only ever hears the second row. At 0.35 that row reads +13.8,
    +8.5, +13.6, +9.9, and the small-signal row does not move at all because the
    clamp is out of play there. The consumers drive it around there or above:
    solera hands its oscillator sum straight in, which peaks at 0.5 for one saw
    at the default balance, and megasynth scales by 0.35 into a filter it then
    sums three of. So the clamp was engaged at every setting a program reaches
    and DRIVE had nothing left to turn up.

    SO THE BOTTOM OF THE TRAVEL IS `kDriveMinGain`'s 0.35 - ONE limiter in this
    machine and now one gain travel into it, 0.35 to 5.0, on both topologies. The
    node is still clamped, which is what `[SCH]` asks for: at 0.35 the cell is a
    soft knee that reaches its hard limit at a node swing of 3.30 going up and
    3.89 going down, so a played note passes and a ringing loop is caught, and
    the node stays BOUNDED at 0.9072/0.35 = 2.592 whatever it is fed, which is
    the same number type 0's own worst case measures. What is NOT preserved is
    the literal `tanh (in)` staging, and that is deliberate: supersynth's own
    ladder is the one measured above at -14.2 dB of passband, and it has this
    same defect. */
inline constexpr float kCascadeNodeMin = kDriveBottomGain;  // = kDriveMinGain; see above
inline constexpr float kCascadeNodeMax = 5.0f;   // [EAR] the top of DRIVE's travel
/** [REF] the ceiling on this type's loop gain at its own crossing. Unity is the
    line between a loop that rings and one that grows and this sits just under
    it. It costs 1.2 dB of passband at the top of the knob against supersynth's
    unrailed version, and it is what keeps check 9e green.

    AND IT IS NOW THE ONLY THING STANDING BETWEEN THIS TYPE AND THE INSTABILITY
    NEAR sr/4, which `kCascadeNodeMin` above was wrongly credited with. Removing
    it returns 0.992 rms out of a 0.231 rms input at a 12 kHz corner and
    resonance 0.60, measured 2026-09-27. Do not take it out, and do not raise it
    toward 1 without re-running that measurement at 8, 12, 16 and 20 kHz. */
inline constexpr float kCascadeLoopMax = 0.98f;

/** One two-pole state-variable section, TPT/zero-delay form. `k` is the damping
    `1/Q` directly, which is the domain the Nord's two tables are written in, so
    a table word goes in without a conversion. */
struct SvfStage
{
    float ic1 = 0.0f, ic2 = 0.0f;
    void reset() noexcept { ic1 = ic2 = 0.0f; }
    void run (float x, float g, float k, float& lp, float& bp, float& hp) noexcept
    {
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float v1 = (ic1 + g * (x - ic2)) * a1;
        const float v2 = ic2 + g * v1;
        hp = x - k * v1 - v2; bp = v1; lp = v2;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
    }
};

// =========================================================== stage 5: shape =
/** [SCH] THE SYNTHEX IS ONE CEM3320, four OTA sections, pole taps through three
    4066 analogue switches into a SIX-RESISTOR SUMMING NETWORK (47K, 100K, 56K,
    56K, 220K, 120K with a 10K trimmer between the two 56Ks). Its four printed
    modes are WEIGHTED SUMS OF ONE FILTER'S OWN TAPS, which is why one filter
    prints three different slopes and why a mode switch on that machine keeps the
    same resonant character and phase relationships. Build them as four filters
    and each measures right while every transition is wrong.

    THE SWITCHES ARE WHY IT HAS FOUR DISCRETE MODES. Make the weights continuous
    and LP morphs through BP, HP and notch and every point between - a filter no
    analogue machine could build, derived entirely from one that did. That is the
    new thing in this section and it is one line of arithmetic: the output is
    linear in the weights, so interpolating the weight VECTOR is exactly what a
    continuous resistor network would do.

    AND THE WEIGHTS ARE THE SYNTHEX'S EXACTLY WHEN WIDTH IS ZERO. Every anchor
    vector below reduces to a printed Synthex mode at width 0 - LP24 is `y4`,
    BP12 is `4(y2 - 2y3 + y4)` [MAN] p9, BP6 is `2(y1 - y2)` [MAN] p8, HP is
    `in - 4y1 + 6y2 - 4y3 + y4`. The width terms are what generalises them to
    poles that are no longer all at one frequency, and `megasynth_filter` check
    2c asserts that reduction to seven digits. */

/** kShapeLp = 0, kShapeBp = 1, kShapeHp = 2, kShapeNotch = 3. The parameter's
    declared range is 0..3 and these are POINTS ON IT. */
inline constexpr int kShapeLp = 0, kShapeBp = 1, kShapeHp = 2, kShapeNotch = 3;

/** WIDTH'S FULL TRAVEL, in semitones of separation between the two pole pairs.

    [MAN] The mechanism is the Supernova II's, which is the only machine in the
    catalogue with a bandwidth control that is not a Q: its special filter types
    take a WIDTH of 0..127 SEMITONES, unipolar, and offset the second section
    through the same ratio table the cutoff uses. So a bandwidth in semitones, on
    the pitch curve, is precedent rather than invention.

    [EAR] THE SPAN IS 48 SEMITONES and it was measured rather than chosen. Four
    poles split two and two do not give a band with two sharp edges: each edge is
    a gentle two-pole, so at two octaves of pole separation the band's own -6 dB
    width grows only from 2.54 octaves to 3.03. The Supernova's control runs to
    127 semitones, so four octaves is well inside the precedent, and it is what
    makes the band's measured width move x1.7 across the travel instead of
    x1.2 - which is the difference between a control that opens the band and one
    that nudges it. */
inline constexpr float kWidthMaxSemis = 48.0f;

/** HOW MUCH OF THAT TRAVEL A SHAPE WITH ONE EDGE GETS.

    [MAN] The Supernova's WIDTH acts on its DUAL and PARALLEL types and does
    nothing at all on plain LPF, BPF or HPF, and the reason is structural rather
    than a limitation: a separation between two corners is the width of a BAND
    when the shape has two edges and the width of a KNEE when it has one. A band
    two octaves wide is a musical bandpass; a KNEE two octaves wide is a lowpass
    that is 14 dB down at its own cutoff, which is what the first build of this
    measured and it read exactly like a broken filter.

    So the split is scaled by how many edges the shape has: full at BP and at
    notch, a fraction of it at LP and at HP. [EAR] the fraction.

    THE FRACTION WAS 0.17 AND IT BOUGHT NOTHING AUDIBLE, and the arithmetic is
    closed form rather than a matter of taste. Four poles split symmetrically in
    LOG frequency have very nearly the magnitude of four coincident ones, because
    what the low pole loses at the centre the high pole has not yet started to
    take. At the corner itself, against the coincident case:

        bandness   half-split        corner droop
          0.17      0.34 oct           -0.48 dB
          0.30      0.60                -1.46
          0.44      0.88                -3.05   <- what ships
          0.50      1.00                -3.88
          1.00      2.00               -13.09   <- "read exactly like a broken
                                                   filter", the measurement above

    So the old number gave half a decibel at the TOP of the travel, and 0.12 dB
    at the 0.5 the knob defaults to. Solera's `--pcmsweep` measured 0.7 dB in
    audio and reported the control dead, which it effectively was on one-edged
    shapes.

    AND 0.17 WAS CHOSEN TO PASS A GATE. The argument for it over zero was that
    `controls_are_alive` sweeps every control on a program whose SHAPE is the
    lowpass, so a WIDTH reaching no audio there fails by that gate's standard -
    which made the shipped number the smallest one that turns a gate green rather
    than the one a player can hear. That is a real law put where nobody can reach
    it, the same mistake as the six-pole anchor, and the author settled both the same
    way: make it reach, or take it out.

    0.44 MAKES IT REACH, AND IT IS THE LARGEST THE STABILITY SWEEP ALLOWS. The author
    asked for 0.50 and the 6400-setting sweep refuses it: 7 settings leave their
    rms bound, every one of them a HIGHPASS at a 200 Hz corner with WIDTH wide
    open, played the 110 Hz saw the sweep uses - a fundamental just under the
    corner, with the highpass's lower pole split down to 100 Hz so it passes, and
    the shape's body gain (fitted at the reference width, `kBodyHp4`) still
    multiplying it. Bracketed rather than guessed: 0.44 gives 0 over bound, 0.45
    gives 2, 0.46 gives 3, 0.47 gives 5, 0.48 gives 7.

    So the same method that set DRIVE's make-up fraction sets this one - take the
    largest value the gate allows rather than the one that sounded right - and the
    cost of the difference is 0.8 dB: -3.05 dB of corner droop at full WIDTH
    instead of -3.88. Both are audible; only one of them is stable everywhere.

    WHAT WOULD RETIRE 0.44 AND ALLOW MORE: making the shape body a function of
    WIDTH. `kBodyHp4` and its neighbours are fitted at the reference width and
    applied at every width, which is exactly why a wide highpass overshoots. That
    is a real change to eight [EAR] constants that 205 stored programs were voiced
    against, so it is filed rather than done.

    It moves every stored program whose width is not dead centre; at the 0.5
    default that is -0.80 dB, and the machines' references are re-captured once for
    it together with the other corrections of this week.

    THE ONE-POLE'S REACH IS A SEPARATE TABLE and is deliberately not derived from
    this number - see `kOnePoleOneEdged`. WIDTH keeps a fraction of its travel on
    a one-edged shape and the one-pole keeps NONE of its travel on a two-edged
    one; those are different claims and they move independently. */
inline constexpr float kBandnessOneEdge = 0.44f;

// ============================================================ the block =====
/** One super filter. No patch, no parameters, no branches on a program: the
    processor computes every coefficient and this holds blocks, which is the
    split the hardware had and the one `Engine.h` argues for at the top.

    THE STRUCTURE, once, because everything above is a claim about it:

    Two ladders run in parallel, one per SLOPE anchor. Each is a cascade of TPT
    one-poles inside a resonance loop with the Virus limiter in it, and each
    reads its output as a weighted sum of its own taps.

      * the 24 dB anchor is FOUR poles, ordered [hi, hi, lo, lo], with NEGATIVE
        feedback taken from the last pole. Its loop reaches -180 degrees exactly
        at the geometric centre of the two corners, for any width.
      * the 12 dB anchor is TWO poles, ordered [hi, lo], with POSITIVE feedback
        taken from its own bandpass tap. Its loop is real and positive at that
        same centre, for any width - and its DC gain is one whatever the
        resonance, which is the [MAN] level law above.
      * the 6 dB anchor is ONE pole, at the geometric centre, with NO FEEDBACK
        AT ALL. It is the cheapest cell in the file - one `onePole`, no limiter,
        no crossing to solve - and the two trims it declares are a cutoff trim
        and a headroom shift. It cannot resonate and it cannot print a shape
        with two edges; both of those are laws with their own blocks above and
        their own measurements.

    WIDTH SPLITS THE POLES and RESONANCE SETS THE LOOP GAIN, and they are
    independent BY CONSTRUCTION rather than by calibration: the loop's phase
    condition lands on the geometric centre at every width, so moving WIDTH
    cannot move the resonant peak, and the loop gain is normalised for the split
    so moving WIDTH cannot change how much peak a given RESONANCE buys. That
    independence is the whole claim of this control and check 3 measures both
    halves of it. */
class SuperFilter
{
public:
    // ===================================================== OVERSAMPLING ======
    /** OFF / 2x / 4x, AND IT DEFAULTS TO OFF, so no stored program in either
        machine changes by any amount unless the player asks. That is measured
        rather than asserted, and it has been re-measured at every change since.
        TWO earlier versions of this file are kept under their own namespaces and
        run beside this one over the type / shape / slope / cutoff / resonance /
        width / drive surface, comparing RAW BIT PATTERNS - 11.232 million samples
        per row, 15 rows, 168.5 million in all, and the count of differing words
        is ZERO:

          * against the six-pole version that preceded the one-pole anchor, at
            slope 12, 24, 18, 14.4 and 21.6, off and at 2x and 4x
          * against the two-anchor version that preceded the oversampler at all,
            oversampling off
          * and for types 1-3, at DRIVE 0, which is what says DRIVE's new make-up
            reaches nothing a stored program holds at the bottom of that knob

        megasynth's `envelope_laws_hold` check 4 is the standing version of the
        same promise: 599 stored programs rendering to the same hash.

        The author, 2026-09-27, in `~/projects/arcturus/filters-design-rules.md`:
        *"oversampling is crucial in plugins ... to stop high-frequency harmonic
        generation from folding back into audible frequencies as harsh digital
        noise"*. This filter has those generators - the Virus limiter on the
        feedback term, DRIVE's gain into it, the topologies' own node clamps and
        the output cell - and they sit inside the loop, where the signal is
        loudest.

        WHAT IT IS WORTH, MEASURED. A sine at the filter's own corner at 0.5
        peak; a 32768-point FFT of the output with the tone on an exact bin; the
        energy in every bin under 20 kHz that is neither the fundamental nor one
        of its in-band harmonics, relative to the fundamental:

          cutoff 9k, res 0.90, DRIVE 1.00, slope 24, tone 9 kHz
            type          1x        2x        4x     2x buys   4x buys
            SUPER      -24.1 dBc -45.1 dBc -62.7 dBc  21.0 dB   38.6 dB
            SVF        -31.3     -52.1     -99.2      20.9      68.0
            CASCADE     -9.4     -45.0     -49.2      35.6      39.8

          cutoff 5k, res 0.75, DRIVE 0.50, slope 24, tone 5 kHz
            SUPER      -61.8     -79.8     -93.6      18.0      31.8
            SVF        -77.3     -99.9    -124.2      22.6      46.9
            CASCADE    -82.6     -80.0     -88.3      -2.6       5.7

          cutoff 2k, res 0.50, DRIVE 0.25, slope 24, tone 2 kHz
            SUPER     -131.3    -132.6    -129.2       1.3      -2.1
            SVF       -117.8    -126.8    -125.5       9.0       7.7
            CASCADE   -125.7    -131.7    -128.9       5.9       3.1

          cutoff 9k, res 0.90, DRIVE 1.00, slope 6, tone 9 kHz
            SUPER      -87.9 dBc -125.4 dBc -141.9 dBc  37.5 dB   53.9 dB

        SO 4x IS NOT A BETTER 2x, IT IS FOR ONE CORNER OF THE SURFACE, and that
        is a result the panel should carry rather than a caveat. At the
        aggressive setting 4x adds another 17.6 dB over 2x on the fused filter
        and 47 dB on the SVF, and the cascade - whose node clamp is a hard one -
        gets almost all of its win from 2x and only 4 dB more from 4x. At the
        third row NOTHING is audible at any factor: every figure is past
        -117 dBc and the differences between the columns are the float noise
        floor, not a measurement. The middle row is the one to read for the
        panel: 1x is already inaudible there at -62 to -83 dBc.

        AND THE 6 dB ANCHOR DOES NOT WANT IT AT ALL, which is the other half of
        the same mechanism read from the other end. At the identical aggressive
        setting the fused filter's 1x figure is -87.9 dBc at slope 6 against
        -24.1 at 24: sixty-four decibels of difference, from the same input, at
        the same corner, with DRIVE at the top. The reason is the whole content of
        the anchor: the in-loop limiter is the aliasing source and this cell has
        no loop, so the only nonlinearity a sample meets between the input and the
        output is the shared output rail. A player at the bottom of the SLOPE
        travel is already past -87 dBc with oversampling OFF, and the 37.5 dB that
        2x buys there is 37.5 dB below anything audible. The six-pole was the
        opposite case and wanted oversampling MORE than the 24 dB anchor did; this
        one wants it least of the three, and that is the sentence to put beside the
        panel's 6 dB detent.

        THE CPU, MEASURED, per voice at 48 kHz, `set` and `process` per sample
        the way `Engine.cpp:1490` calls them, 8 million samples each, -O2:

            two anchors, before any of this (5ea6047)    x1.000  (95.3 ns/smp)
            three anchors + the six-pole, off            x1.67   (159.1)
            three anchors + the six-pole, 2x             x1.97   (187.8)
            three anchors + the six-pole, 4x             x2.39   (227.4)
            the one-pole third anchor, off               x1.04   (99.4)
            the one-pole third anchor, 2x                x1.49   (141.8)
            the one-pole third anchor, 4x                x1.85   (176.2)
            and the same block sitting AT slope 6, off   x1.05   (100.3)

        Four runs of 8 million samples each, spread over the session; the spread
        between runs is under 1 per cent on every row.

        Oversampling costs less than its own ratio because `set` runs once per
        BASE sample whatever the factor, and `set` is the expensive half of this
        block - most of it is transcendentals in the crossing solves.

        AND THE THIRD ANCHOR IS NOW CHEAPER THAN NO THIRD ANCHOR AT ALL, which is
        not a rounding artefact and is worth naming. The six-pole cost x1.67 at
        EVERY slope, including 12 and 24 where its weight was exactly zero, and
        two thirds of that was `set` solving one more transcendental crossing -
        measured then, by handing the six-pole loop the four-pole's answer, which
        dropped the figure to x1.22. The one-pole has NO crossing to solve,
        because it has no loop: it adds one `tan` for its own trimmed corner and
        one `onePole` to `process`, and it removes six `onePole`s, seven weights
        and two fixed-point solves. THE THIRD ANCHOR NOW COSTS 5 PER CENT WHERE IT
        COST 68, and the whole block is x0.62 of what HEAD's was with oversampling
        off - so the change gives back more CPU than the six-pole ever spent, and
        it is given back OUTRIGHT rather than gated behind a switch, which was the
        point.

        THE STATE GOES WITH IT. The block is 1248 bytes where HEAD's is 1296 and
        the two-anchor filter before the oversampler was 280: the third anchor is
        7 floats now (a state, an output, a `g`, a body, a two-weight vector and
        the leg's own morph) where the six-pole's was 20, and everything else in
        the growth is the 16-tap kernel's two rings. Per voice, and megasynth holds
        three filters in each of 96 Voices, so the 48 bytes the anchor gives back
        is 13.8 kB across the instrument - which is not the reason to do it, but it
        is not nothing either.

        LATENCY. See `latencySamples()`, which is the thing a consumer has to
        read and act on. */
    void setOversampling (int factor) noexcept
    {
        const int f = (factor == 2 || factor == 4) ? factor : 1;
        if (f == os) return;
        os = f;
        sr = hostSr * (double) os;
        ovs.setFactor (os);
        reset();
    }
    [[nodiscard]] int oversampling() const noexcept { return os; }

    /** THE LATENCY, IN BASE SAMPLES, AND THIS IS THE ONE A CONSUMER MUST READ.

        ZERO WHEN OFF, `kOsTapsPerPhase` WHEN ON. Off is a true zero and not a
        rounded one: the oversampler is not called at all, so the sample that
        goes in comes out of the same call, which is what makes the off setting
        bit-identical to the filter that never had one.

        WHY A CONSUMER CANNOT IGNORE IT. This filter is PER VOICE in both
        machines that use it, and in Solera there are two paths that bypass it
        and sum beside it - the M1 PCM engine and, with the OWN switch, the FM
        stack. A filter that quietly gained 32 samples would make those layers
        arrive 0.67 ms EARLY and comb against the filtered voice, with the first
        notch at 750 Hz. That is not a latency report, it is a tone change.

        WHAT TO DO ABOUT IT, exactly. `dsp::AlignDelay` in `dsp/Oversampler.h`
        is the other half of this block and exists for nothing else:

            ::dsp::AlignDelay<32> pcmAlign, fmAlign;        // once, per voice
            filter.setOversampling (osFactor);              // when the player asks
            pcmAlign.prepare (filter.latencySamples());     // in the same breath
            fmAlign.prepare  (filter.latencySamples());
            ...
            out = filter.process (toFilter) + pcmAlign.process (pcm)
                                            + fmAlign.process (fm);

        A host that reports plugin latency adds the same number ONCE, at the
        top: every voice carries the same delay, so it is the instrument's
        latency and not a per-voice one.

        AND SWITCHING IT COSTS A RESET, which is deliberate and is stated here
        rather than discovered. `setOversampling` calls `reset()`, because the
        alternative - carrying the ladder states across a rate change - means
        every pole's `g` is suddenly wrong for the state beside it, which at
        high resonance is how a filter rings on a note nobody played. A player
        changing oversampling mid-note gets a re-struck filter; a player
        changing it between notes gets nothing at all.

        WHY 16 AND NOT 32 OR 8, MEASURED BEFORE IT WAS CHOSEN - and the
        measurement overturned the argument it was made to settle.

        THE ARGUMENT WAS THAT A SHORTER KERNEL BUYS LATENCY OUT OF THE ALIAS
        REJECTION, because a Kaiser window's transition width is `(A - 7.95) /
        (14.36 N)` of the rate it runs at and N is `TapsPerPhase * factor`, so
        the factor cancels and 80 dB across an 8 kHz transition costs about 30
        base samples however you arrange it. That is true about the KERNEL and
        false about the FILTER. Measured, the same aggressive setting as the
        table above, the fused filter at 4x:

            taps/phase   latency   alias 4x   passband at 8k / 16k / 19k / 20k
                 8        8 smp    -62.7 dBc   0.00  -1.10  -2.98  -3.79 dB
                16       16 smp    -62.7 dBc   0.00  +0.00  -0.43  -1.10
                32       32 smp    -62.7 dBc   0.00  +0.00  -0.00  +0.00

        THE ALIAS COLUMN DOES NOT MOVE AT ALL. What folds back audibly is
        generated well above the transition band, and an 8-tap kernel already
        kills that; the extra 24 taps buy nothing but flatness in the last
        octave. So the kernel length is a PASSBAND decision and not an aliasing
        one, and the trade is between the top of the band and the delay.

        16 IS WHERE IT LANDS. 32 costs twice the delay for 0.43 dB at 19 kHz,
        which nobody hears. 8 costs 1.10 dB at 16 kHz and 2.98 at 19 kHz, which
        is a filter that is dull when it is supposed to be open, and an
        oversampler that takes more out of the sound than the aliasing it
        removed was putting in has not helped. 16 base samples is 0.33 ms at
        48 kHz, and it is a whole number, which is the property that matters
        most - see the paragraph above.

        A LOW-LATENCY DESIGN WAS THE OTHER ANSWER AND WAS NOT TAKEN. An IIR
        polyphase-allpass half-band reaches the same stopband in 2-3 samples of
        group delay, and for a FILTER its phase distortion would not matter,
        because a filter is minimum-phase already. It is not here because its
        delay is FREQUENCY-DEPENDENT, so there is no integer for `AlignDelay` to
        take and the two Solera layers could not be aligned exactly at ANY
        setting - which is the actual requirement. A constant 16 that can be
        cancelled exactly beats a varying 2 that cannot be cancelled at all.

        WHAT OVERSAMPLING DOES TO THE SOUND, WHICH IS NOT NOTHING AND IS NOT THE
        OVERSAMPLER. The kernel is transparent - +0.008 dB at 100 Hz and flat to
        within 0.002 dB from 1 kHz to 16 kHz, measured with an identity shaper.
        The FILTER is not: a TPT one-pole's corner is bilinear-warped by
        `tan(pi f / sr)`, and running at a higher rate unwarps it, so a filter
        with a high corner gets DARKER - closer to the analogue prototype it is
        a model of. Measured at a 20 kHz cutoff, resonance 0, DRIVE 0, width 0:

            f         off vs 2x   off vs 4x
             1 kHz      -0.028      -0.037 dB
             4 kHz      -0.416      -0.547
             8 kHz      -1.589      -2.022
            12 kHz      -3.226      -3.905
            16 kHz      -4.321      -4.927

        and at a cutoff a program actually sits at, 2 kHz, the same sweep reads
        -0.019, +0.002 and +0.371 dB. So the effect is confined to corners near
        the top of the range, where the warping was the thing that was wrong.
        It is still a sound change, and it is the reason the default is off. */
    [[nodiscard]] int latencySamples() const noexcept
    { return latencyForFactor (os); }

    /** THE SAME ANSWER WITHOUT AN INSTANCE, because the consumer that needs it most does
        not have one. In a polyphonic synth the filter lives in the VOICE, and the thing
        that has to report latency to the host and align a bus that bypasses the voices
        is the PROCESSOR - which owns no filter at all. Asking voice zero would work
        until a machine has no voices allocated yet, which is exactly when a host asks. */
    [[nodiscard]] static constexpr int latencyForFactor (int factor) noexcept
    { return factor <= 1 ? 0 : Oversampler::kLatency; }

    void prepare (double sampleRate) noexcept
    {
        hostSr = sampleRate;
        sr     = hostSr * (double) os;
        ovs.setFactor (os);
        reset();
    }

    void reset() noexcept
    {
        ovs.reset();
        for (auto& s : z4) s = 0.0f;
        for (auto& s : z2) s = 0.0f;
        z1 = 0.0f;
        fb4 = fb2 = 0.0f;
        for (auto& s : svf) s.reset();
        for (auto& s : zc4) s = 0.0f;
        for (auto& s : zc2) s = 0.0f;
        fbc4 = fbc2 = 0.0f;
    }

    /** WHICH TOPOLOGY. Sticky, so a processor that resolves it once per block
        does not have to thread it through the per-sample `set`. It is a separate
        setter AND a trailing argument on `set` for exactly that reason: the
        coefficient builder can write it where it writes everything else, and a
        probe can state it inline. Out-of-range clamps rather than asserts,
        because a stored program from a future bank must not be able to crash
        this one. */
    void setType (int t) noexcept { type = std::clamp (t, 0, kTypeCount - 1); }
    [[nodiscard]] int getType() const noexcept { return type; }

    /** Every coefficient, once per sample. The cutoff arrives in Hz because it
        has ALREADY been converted - once, from the cents sum, on the far side of
        `CutoffTerms::hz()`. Nothing in here converts anything again.

        `type_` DEFAULTS TO -1, WHICH MEANS "KEEP", not "SUPER". A caller that
        never heard of types keeps the one it was given and every existing call
        site compiles and sounds unchanged; a caller that passes one is setting
        it. Passing 0 explicitly is what selects the fused filter. */
    void set (float cutoffHz, float resonance01, float shape03, float width01,
              float drive01, float slopeDb, int type_ = -1) noexcept
    {
        if (type_ >= 0) type = std::clamp (type_, 0, kTypeCount - 1);
        if (type != kTypeSuper)
            return setAlt (cutoffHz, resonance01, shape03, width01, drive01, slopeDb);
        morph = slopeMorph (slopeDb);
        // THE LOWER LEG'S WEIGHT, and it is exactly 0 for every slope at or above
        // 12 - which is what makes the 12 and 24 positions bit-identical to the
        // filter that had two anchors. See `slopeMorph6`.
        morph1 = slopeMorph6 (slopeDb);
        res   = std::clamp (resonance01, 0.0f, 1.0f);
        shape = std::clamp (shape03, 0.0f, 3.0f);
        drive = std::clamp (drive01, 0.0f, 1.0f);

        // WIDTH: half the separation up and half down, in semitones, so the
        // geometric centre of the band stays exactly where CUTOFF put it. `spr`
        // is that half-interval as a ratio and is `r` throughout the weights.
        // The travel is scaled by the shape's own edge count - see
        // kBandnessOneEdge - so a lowpass keeps its corner where the knob put it.
        shapeIndex = std::min ((int) shape, 2);
        shapeFrac  = shape - (float) shapeIndex;
        const float edges[4] { kBandnessOneEdge, 1.0f, kBandnessOneEdge, 1.0f };
        const float bandness = edges[shapeIndex] * (1.0f - shapeFrac)
                             + edges[shapeIndex + 1] * shapeFrac;
        const float halfSemis = std::clamp (width01, 0.0f, 1.0f)
                              * kWidthMaxSemis * 0.5f * bandness;
        spr  = std::exp2 (halfSemis / 12.0f);
        rSq  = spr * spr;
        // AND THE SAME EDGE COUNT GATES THE ONE-POLE'S REACH, which is the second
        // half of the law in `kOnePoleOneEdged`'s block above: a two-edged shape
        // has no first-order member, so the lower leg of SLOPE simply does not
        // travel at the bandpass or the notch and both hold the 12 dB anchor's
        // own printed [MAN] Synthex mode. It is `1 - bandness`'s own table rather
        // than `1 - bandness`, because `kBandnessOneEdge` is 0.17 and not 0 -
        // WIDTH keeps a third of its travel on a one-edged shape and this keeps
        // none of its travel on a two-edged one, which are different claims.
        const float oneEdged = kOnePoleOneEdged[shapeIndex] * (1.0f - shapeFrac)
                             + kOnePoleOneEdged[shapeIndex + 1] * shapeFrac;
        morph1 *= oneEdged;

        const float nyq = (float) (sr * 0.45);
        // The corner in radians per sample, which is also one sample of feedback
        // delay in radians. Both corrections below are functions of it.
        const float w = 6.28318531f * std::min (cutoffHz, nyq) / (float) sr;
        // ONE `exp2` PER ANCHOR AND NOT ONE PER POLE. The trim is in cents on
        // the same curve everything else is on - it IS the pitch curve, which is
        // Arcturus non-negotiable 3 in its strongest form.
        const float fc12 = std::clamp (cutoffHz * std::exp2 (kAnchor12.cutoffTrimCents / 1200.0f),
                                       kFloorHz, nyq);
        const float fc24 = std::clamp (cutoffHz * std::exp2 (kAnchor24.cutoffTrimCents / 1200.0f),
                                       kFloorHz, nyq);
        // AND THE ONE-POLE'S, ON THE SAME CURVE AND WITH NO PEAK TRIM. `peakTrim`
        // below corrects where a resonant LOOP crosses -180 degrees; this cell
        // has no loop, so there is nothing to correct and its corner is exactly
        // where CUTOFF put it at every resonance. WIDTH does not reach it either:
        // one pole has no separation to split, so it sits at the geometric centre
        // the split is taken about - which is, again, exactly where CUTOFF put it.
        const float fc01 = std::clamp (cutoffHz * std::exp2 (kOnePoleTrimCents / 1200.0f),
                                       kFloorHz, nyq);
        g1 = poleG (fc01);
        // THE LOOP IS A SAMPLE LATE, AND THAT DETUNES IT. There is a saturator
        // in the feedback, so the loop cannot be resolved in closed form the way
        // each individual pole is - which is why both of the catalogue's
        // analogue ladders read the previous sample too. The cost is a known
        // one: the extra sample of phase drags the frequency at which the loop
        // reaches -180 degrees DOWNWARD, and dspblocks' own LadderFilter says so
        // out loud, "a one-sample-delayed loop detunes the resonant peak
        // downward as the cutoff rises".
        //
        // Measured here before it was corrected: the resonant peak sat 2.2
        // semitones under the knob at width zero and 4.1 under it wide open. So
        // the fault was not only a detune, it was a detune THAT MOVED WITH
        // WIDTH, which reads as the band sliding flat as it opens.
        //
        // THE CORRECTION IS ON THE FEEDBACK, NOT ON THE POLES, and that is the
        // whole of what took two attempts. Moving the poles up to put the peak
        // back also moves the CORNER, which at resonance zero was already
        // exactly where the knob put it - and the two anchors need different
        // amounts of it, four poles having twice the group delay of two, so
        // their corners walked 103 cents apart where their trims declare 18.
        // Scaling the pole correction by the resonance fixed the corner and left
        // the peak sliding again, because the phase condition does not care how
        // much loop gain there is.
        //
        // [REF] So the feedback signal gets a one-sample PHASE LEAD instead -
        // `2 fb[n-1] - fb[n-2]`, a linear extrapolation, which cancels the
        // delay's phase to first order and moves no pole at all. Its magnitude
        // is sqrt(5 - 4 cos w), which is 1.017 at a 1 kHz corner and is divided
        // straight back out of the loop gain below, so it costs nothing either.
        // AND A LADDER'S RESONANT PEAK SITS BELOW ITS OWN CORNER, which is a
        // second thing entirely and was mistaken for the first for an afternoon.
        // The loop's magnitude RISES as the frequency falls - the cascade is a
        // lowpass, so |G| climbs toward DC - and that more than pays for the
        // phase turning away from -180. So the response's maximum lands under
        // the corner even with the delay perfectly corrected: measured, 1.8
        // semitones under at width zero and 3.9 under wide open.
        //
        // IT IS TWO EFFECTS AND ONLY ONE OF THEM IS FITTED.
        //
        // WHERE THE LOOP CROSSES is solved for exactly, below and here. With the
        // poles at r and 1/r either side of the corner, the four-pole loop
        // reaches -180 degrees where
        //
        //     2 atan(x/r) + 2 atan(x r) = pi - w
        //
        // and the tangent addition formula turns that into a quadratic in x
        // whose positive root is one square root: x = (-ct + sqrt(c^2t^2+4))/2
        // with c = r + 1/r and t = tan(w/2). The `w` is the sample of feedback
        // delay, in radians, so `x` carries the delay's own contribution and
        // `1/x` is the exact amount the poles have to move to put the crossing
        // back on the corner. The two-pole loop closes on its own BANDPASS tap,
        // which is at zero degrees rather than 180, so it carries the whole `w`
        // instead of half of it.
        //
        // WHAT IS LEFT OVER after that is the |G| rise, and it is the only part
        // with a fitted constant in it. It is scale free, so it depends on the
        // pole split alone, through the phase slope at the crossing, (r^2+1)/4r.
        // One constant fitted at r = 1 and checked at r = 4 predicted the second
        // to within two per cent.
        //
        // SCALED BY THE RESONANCE, because the thing it moves is the PEAK and at
        // resonance zero there is no peak - only a corner, which was already
        // exactly where the knob put it. An unscaled version moved every corner
        // in the machine up, by more and more as the cutoff rose, and broke the
        // one law this filter shares with the oscillators.
        // THE DELAY'S PHASE IS TAKEN AT THE CROSSING, NOT AT THE CORNER, and
        // getting that wrong is what made the first version of this explode.
        // One sample of delay costs 2 pi f / fs radians at the frequency being
        // evaluated, and the crossing is not at the corner - at a high corner it
        // sits well below it, at x = 0.2 of it or less. Charging the crossing
        // the corner's delay makes x come out far too small, the cascade's
        // magnitude there far too small with it, and `1/mag` then hands the loop
        // a gain of tens: measured, 5.7 rms out of a 0.231 rms input at a 12 kHz
        // corner and resonance 0.25, which is a low resonance setting.
        //
        // So the equation is transcendental, `2 atan(x/r) + 2 atan(x r) =
        // pi - x w`, and it is solved by fixed point from x = 1. HALF-STEPPED,
        // because the plain map is NOT a contraction near a quarter of the
        // sample rate - it oscillates between 0.007 and 0.986 and never settles,
        // which left the loop gain at whichever end the last pass happened to
        // land on. Averaging each step with the one before it converges in three
        // passes at every corner from 20 Hz to Nyquist.
        const float c = spr + 1.0f / spr;
        float x4 = 1.0f, x2 = 1.0f;
        // AND THERE ARE TWO SOLVES HERE AND NOT THREE, which is where most of
        // this change's CPU comes back. A six-pole cell sat beside these two for
        // half a day and needed its own crossing - `3 atan(x/r) + 3 atan(x r) =
        // pi - x w`, starting at tan(30 deg) - because its loop reaches -180
        // degrees well under its corner. The one-pole that replaced it has NO
        // LOOP, so it has no crossing, no magnitude to normalise and no
        // fixed-point iteration: it is one `tan` for its corner and nothing else.
        for (int pass = 0; pass < 4; ++pass)
        {
            const float t4 = std::tan (std::min (1.5533f, x4 * w * 0.5f));
            const float t2 = std::tan (std::min (1.5533f, x2 * w));
            const float a4 = c * t4, a2 = c * t2;
            x4 = 0.5f * (x4 + 0.5f * (std::sqrt (a4 * a4 + 4.0f) - a4));
            x2 = 0.5f * (x2 + 0.5f * (std::sqrt (a2 * a2 + 4.0f) - a2));
        }
        const float phaseSlope = (rSq + 1.0f) / (4.0f * spr);
        // CLAMPED, because `1/x4` is a correction and not a law. As the corner
        // climbs, one sample of delay is a larger and larger share of the loop's
        // phase, and past about a sixth of the sample rate the poles would have
        // to move further than there is room for: at a 14 kHz corner the
        // uncorrected form asks for a trim of 122, which puts every pole hard
        // against the Nyquist clamp and takes the filter somewhere it does not
        // come back from - `tripwires` reported non-finite audio, which is the
        // loudest way this repo has of saying a number went somewhere it should
        // not. Past the ceiling the peak keeps its delay-induced offset, which
        // is what a filter running out of room does anyway.
        const float peakTrim = std::min (kMaxPeakTrim, 1.0f + res
                             * ((1.0f + kPeakTrim * phaseSlope) / x4 - 1.0f));
        // AND THE CROSSING IS SOLVED AGAIN ONCE THE POLES HAVE MOVED, because
        // the trim has just moved the crossing and with it the delay the
        // crossing sees. Leaving it out cost a coupling between WIDTH and
        // RESONANCE - the peak grew 33 per cent across WIDTH's travel instead of
        // 22 - because the error is larger at a wide split than a narrow one.
        float x4b = x4, x2b = x2;
        for (int pass = 0; pass < 3; ++pass)
        {
            const float t4 = std::tan (std::min (1.5533f, x4b * w * peakTrim * 0.5f));
            const float t2 = std::tan (std::min (1.5533f, x2b * w * peakTrim));
            const float a4 = c * t4, a2 = c * t2;
            x4b = 0.5f * (x4b + 0.5f * (std::sqrt (a4 * a4 + 4.0f) - a4));
            x2b = 0.5f * (x2b + 0.5f * (std::sqrt (a2 * a2 + 4.0f) - a2));
        }
        // BOTH RAILS ON BOTH POLES, and the low one is not a formality. It was
        // floored and never ceilinged, on the reasoning that the low pole is the
        // low one - and `peakTrim` moves BOTH poles up, so at an 18 kHz corner
        // it landed at 24.3 kHz on a 48 kHz rate. Past Nyquist `tan` turns
        // negative, the TPT one-pole's G comes out above 1, its own pole leaves
        // the unit circle and the state diverges to a NaN in 1554 samples.
        // `tripwires` caught it as "8 non-finite" and `megasynth_filter` did
        // not, because every law on that page is measured at a 1 kHz corner.
        gHi4 = poleG (std::clamp (fc24 * peakTrim * spr, kFloorHz, nyq));
        gLo4 = poleG (std::clamp (fc24 * peakTrim / spr, kFloorHz, nyq));
        gHi2 = poleG (std::clamp (fc12 * peakTrim * spr, kFloorHz, nyq));
        gLo2 = poleG (std::clamp (fc12 * peakTrim / spr, kFloorHz, nyq));
        // ONE TRIM FOR BOTH ANCHORS and not one each, which is a decision. The
        // two cascades have different group delays and would want different
        // corrections, and giving them different ones walks their CORNERS apart
        // by 103 cents where their own trims declare 18 - so the correction is
        // the block's, applied to the cutoff word once, and the 2-pole's peak
        // sits a little lower than the 4-pole's. Two chips fed one control
        // voltage differ exactly like that.
        //
        // AND THE FEEDBACK IS NOT PHASE-LED, which was tried and taken out again
        // rather than quietly dropped. `2 fb[n-1] - fb[n-2]` is a linear
        // extrapolation that cancels the delay's phase to first order without
        // moving a pole, and at a 1 kHz corner it is exactly right and costs
        // nothing - the correction there is 0.13 of a degree. IT IS A HIGH
        // FREQUENCY BOOST, and a filter's corner does not stay at 1 kHz: at
        // 9 kHz on a 48 kHz rate its magnitude at the corner is 1.835 while it
        // gives back only 30 degrees of the 66 the delay costs, so the loop is
        // both louder and still late. Measured on the factory bank, every lead
        // in it lost more than half its energy above 4 kHz - Blade Lead 0.062 to
        // 0.027, Sync Solo 0.198 to 0.076 - and "Virus Ripper" fell under the
        // floor its own name is checked against. Found by `megasynth_bank`,
        // which is the only thing here that renders a whole bank of programs
        // with the cutoff up where a lead puts it.

        // RESONANCE -> LOOP GAIN, NORMALISED WHERE THE LOOP ACTUALLY CROSSES.
        //
        // THIS IS THE ONE THAT KEEPS THE FILTER FROM BLOWING UP, and it took the
        // audio to find. Normalising at the CORNER is right only if that is
        // where the loop reaches its phase condition, and the one sample of
        // feedback delay costs 360 f/fs degrees - at a 1 kHz corner that is 7.5
        // and it does not matter, at 8 kHz it is 60 and it matters enormously.
        // The crossing moves DOWN, the cascade's magnitude is much larger down
        // there, and the loop gain the knob asked for arrives multiplied. A
        // 110 Hz saw at 0.35 measured 3.24 out at cutoff 8 kHz and resonance
        // 0.85, and 4.7 at 14 kHz - the "unstable near sr/4" that dspblocks'
        // own LadderFilter warns about, in this filter, at settings a player
        // reaches by turning two knobs up.
        //
        // So the crossing is SOLVED FOR rather than assumed. With the poles at
        // r and 1/r either side, the four-pole loop needs
        //
        //     2 atan(x/r) + 2 atan(x r) = pi - w
        //
        // and the tangent addition formula turns that into a quadratic in x
        // whose positive root is one square root: x = (-ct + sqrt(c^2t^2+4))/2
        // with c = r + 1/r and t = tan(w/2). The two-pole loop closes on its own
        // bandpass tap, which is at zero degrees rather than 180, so it carries
        // the whole w instead of half of it. Evaluating the cascade's magnitude
        // AT x and dividing that out is what makes the resonance knob mean the
        // same loop gain at every cutoff and every width.
        //
        // It also makes the filter quieter and less resonant as the corner nears
        // Nyquist, which is correct and is what a four-pole cascade running out
        // of room actually does.
        const float hi4 = x4b / spr, lo4 = x4b * spr;
        const float hi2 = x2b / spr, lo2 = x2b * spr;
        const float mag4 = 1.0f / ((1.0f + hi4 * hi4) * (1.0f + lo4 * lo4));
        const float mag2 = lo2 / (std::sqrt (1.0f + hi2 * hi2) * std::sqrt (1.0f + lo2 * lo2));
        // AND THE ONE-POLE HAS NO MAGNITUDE TO NORMALISE, because normalising a
        // loop's magnitude at its own crossing is what makes a RESONANCE knob
        // mean the same thing at every cutoff, and this cell has no resonance to
        // calibrate. See `kOnePoleNoResonance`'s block above.
        // THE RESONANCE EASES OFF AT THE BOTTOM OF THE SWEEP.
        //
        // `[FW]` JP-8000 `0x1D7A4`: for a corner word at or below `0x3B3D` the
        // host multiplies its damping by `(0x4A4B - w/8) >> 14` and passes it
        // through untouched above that threshold. Up to 16 per cent more
        // damping and about 1.3 dB less resonance at the low end, and nothing
        // at all over the rest of the range.
        //
        // WHAT IT IS FOR, in the catalog's own words: a machine taming its own
        // filter where it would otherwise get unruly, and a large part of why
        // that resonance stays musical at the bottom. A digital SVF's Q rises
        // as the corner falls - the same loop gain buys a sharper peak when the
        // pole is closer to DC - so a knob calibrated in the middle of the
        // range screams at the bottom of it.
        //
        // The threshold in OUR units: that machine's corner word is its own
        // pitch curve, and `0x3B3D` of `0xFFFF` is 0.232 of it, which on this
        // machine's 20 Hz..20 kHz log sweep is 20 * 1000^0.232 = 78 Hz. Below
        // that the damping is scaled the same way, linearly in the word.
        // `~/projects/synthcatalog/filters/roland/jp8000.md`, grade A-.
        const float ease = [&]
        {
            const float pos = std::log2 (std::max (kFloorHz, cutoffHz) / 20.0f) / std::log2 (1000.0f);
            if (pos >= kJpEaseFrom) return 1.0f;
            const float t = std::clamp (pos / kJpEaseFrom, 0.0f, 1.0f);
            return 1.0f / (1.0f + kJpEaseMax * (1.0f - t));
        }();
        k4 = ease * res * kAnchor24.loopGainAtFullResonance / std::max (1.0e-4f, mag4);
        k2 = ease * res * kAnchor12.loopGainAtFullResonance / std::max (1.0e-4f, mag2);

        // The body compensation, per anchor, from that anchor's own DC gain -
        // AND FROM THE RESONANCE KNOB ALONE. `k4` carries the width
        // normalisation with it, so reading `k4` here made the makeup grow as
        // the band opened and WIDTH measured as x1.44 on the height of the
        // resonant peak. The makeup is for the RESONANCE's own loss, so it reads
        // the resonance: this is the split-free value of k, which is what the
        // knob would produce with the poles together.
        const float kFlat4 = res * kAnchor24.loopGainAtFullResonance * 4.0f;
        const float kFlat2 = res * kAnchor12.loopGainAtFullResonance * 2.0f;
        // AND THERE IS NO THIRD ONE. The one-pole's DC gain is exactly 1 at every
        // resonance because there is no feedback to lose it - so there is nothing
        // for a body makeup to give back, which is `bodyExponent`'s own block
        // above stated as the line that is NOT written here.
        // AND IT IS CAPPED. The makeup exists to give back what the resonance
        // takes off the PASSBAND, and past about half travel the 4-pole is no
        // longer losing a passband, it is adding its own oscillation - so the
        // makeup stops restoring anything and simply amplifies it. Uncapped, a
        // 110 Hz saw at 0.35 came out at 1.68 with the resonance at the top.
        body4 = std::min (kBodyMakeupMax, std::pow (1.0f + kFlat4, kAnchor24.bodyExponent));
        body2 = std::min (kBodyMakeupMax, std::pow (1.0f + kFlat2, kAnchor12.bodyExponent));

        // DRIVE is a gain INTO the limiter and its reciprocal on the way out, so
        // a small feedback term passes through untouched and a large one
        // compresses. That is what makes DRIVE do nothing at low resonance and
        // everything at high - which is the proof that it is in the feedback
        // path and not after the filter.
        //
        // AND THE LIMITER SEES THE FEEDBACK TERM `k * tap`, NOT THE TAP. [FW]
        // that is what the Virus limits - P:0x3B87D tests a feedback TERM
        // against 0x7FF000 - and it is also the only version that keeps WIDTH
        // out of the resonance. The tap's own level scales with the loop's
        // magnitude at the centre, which is r^2/(r^2+1)^2 and therefore falls as
        // the poles split, so a limiter on the raw tap compresses hard at a
        // narrow width and barely at all at a wide one: measured, the resonant
        // peak grew x2.29 across WIDTH's travel and the independence check
        // failed on it. `k * tap` is loop gain times the loop signal, which is
        // the same number at every width.
        // The travel is EXPONENTIAL between two gains, and the bottom one is
        // BELOW unity on purpose. At a gain of exactly one the limiter already
        // sits on its own knee at any resonance that rings, so the filter
        // arrives pre-compressed and the resonance never sings: measured, the
        // 4-pole's peak grew only from 1.49 to 1.95 between resonance 0.5 and
        // 0.8, where the knob's top three tenths are supposed to be where it
        // screams. At 0.35 the cell is close to transparent with DRIVE down and
        // the loop is still BOUNDED - 0.9072/0.35, which is what stops it
        // diverging - so nothing about the safety argument changes.
        dGain = kDriveMinGain * std::pow (kDriveMaxGain / kDriveMinGain, drive);
        dInv  = 1.0f / dGain;

        weights();
    }

    /** One sample in, one out - one BASE sample, whatever the oversampling.

        THE OFF PATH IS THE OLD CALL AND NOTHING ELSE. No ring, no filter, no
        branch inside the loop: `core` is called with the sample it was handed
        and its answer is returned. That is what makes the off setting
        bit-identical over the 168.5 million samples the A/B above compares, and
        not merely equal to a tolerance. */
    [[nodiscard]] float process (float x) noexcept
    {
        if (os == 1) return core (x);
        // THE WHOLE PER-SAMPLE PATH IS OVERSAMPLED, not just the limiter, and
        // that is the point: the aliasing sources here are the Virus limiter on
        // the summing node, DRIVE's saturator around it, the topologies' own
        // clamps and the output cell, and they are spread through the loop
        // rather than sitting at one stage. `set` has already been called at the
        // internal rate, so every pole, every crossing and every loop gain in
        // `core` is the one that belongs at `hostSr * os`.
        return ovs.process (0, x, [this] (float v) noexcept { return core (v); });
    }

private:
    /** One sample at the INTERNAL rate. Both anchors ran here before there were
        three; all three run here now, and the detented morph picks. */
    [[nodiscard]] float core (float x) noexcept
    {
        if (type != kTypeSuper) return processAlt (x);
        // ---- the 24 dB chip: four poles, NEGATIVE feedback from the last -----
        {
            const float u = x * kAnchor24.headroom
                          - asymmetricLimit (k4 * fb4 * dGain) * dInv;
            const float t1 = onePole (z4[0], u,  gHi4);
            const float t2 = onePole (z4[1], t1, gHi4);
            const float t3 = onePole (z4[2], t2, gLo4);
            const float t4 = onePole (z4[3], t3, gLo4);
            fb4 = t4;
            out4 = (w4[0] * u + w4[1] * t1 + w4[2] * t2 + w4[3] * t3 + w4[4] * t4)
                 * body4 / kAnchor24.headroom;
        }
        // ---- the 12 dB chip: two poles, POSITIVE feedback from its bandpass --
        {
            const float u = x * kAnchor12.headroom
                          + asymmetricLimit (k2 * fb2 * dGain) * dInv;
            const float t1 = onePole (z2[0], u,  gHi2);
            const float t2 = onePole (z2[1], t1, gLo2);
            fb2 = t1 - t2;
            out2 = (w2[0] * u + w2[1] * t1 + w2[2] * t2)
                 * body2 / kAnchor12.headroom;
        }
        // ---- the 6 dB cell: ONE pole, and no feedback at all -----------------
        // It runs here at every slope for the same reason the other two do:
        // turning a cell off makes un-detenting click, and the state it would
        // have to build on arrival is a state it can only have by having run. The
        // crossfade is between OUTPUTS, which is what the CD4053 does.
        //
        // THE HEADROOM SHIFT IS A NO-OP HERE AND IS WRITTEN ANYWAY. There is no
        // nonlinearity between the two multiplies, and 0.25 is a power of two, so
        // this pair is exactly transparent - see `kOnePoleHeadroom`, which
        // measures DRIVE's travel at slope 6 as 0.00 dB. It is here so the three
        // cells read the same and so the shift is already in place, with the
        // Nord's own number, the day this cell is given something to shift into.
        {
            const float u = x * kOnePoleHeadroom;
            const float t1 = onePole (z1, u, g1);
            out1 = (w1[0] * u + w1[1] * t1) * body1 / kOnePoleHeadroom;
        }
        // THE OUTPUT CELL, and it is the chip's rail rather than a control.
        // [DS] the 3320's variable-gain cell clips at 10-14 V p-p and the OB-Xa
        // catalogue entry clamps its own states at +/-12 for the same reason: a
        // real filter's output is bounded by what it is built out of, and ours
        // was bounded by nothing at all. At full resonance the four-pole
        // self-oscillates, the body makeup and the shape's own body multiply
        // that oscillation, and a 0.231 rms input came back at 4.47.
        //
        // THIS IS NOT WHERE DRIVE LIVES AND MUST NOT BECOME IT. DRIVE is in the
        // FEEDBACK path, which is super-filter.md's whole point about the Virus;
        // this is a rail, it has no control, and at any level a program actually
        // reaches it is doing nothing - 0.7 per cent of compression at an output
        // of 0.5 and 3 per cent at 1.0. What it does is stop the top of the
        // resonance knob from being a clip.
        // THE TRAVEL IS TWO LEGS AND THE MIX PICKS ONE. `morph1` is exactly 0
        // at every slope at or above 12, so that branch is the expression this
        // filter has always evaluated, unchanged, down to the order of the two
        // multiplies - which is what makes the 12 and 24 positions bit-identical
        // rather than merely very close. Writing it as one three-term sum would
        // have been tidier and would have added `out1 * 0.0f` to the result:
        // harmless for every value but a negative zero, and a promise that
        // holds for every value but one is not a promise.
        const float y = morph1 > 0.0f
                      ? out2 * (1.0f - morph1) + out1 * morph1
                      : out2 * (1.0f - morph) + out4 * morph;
        return asymmetricLimit (y * kOutputCell) / kOutputCell;
    }

public:
    /** The tap weights the last `set` resolved, for the gate that asserts they
        reduce to the Synthex's printed modes at width zero. Nothing in the audio
        path reads these. */
    [[nodiscard]] const float* fourPoleWeights() const noexcept { return w4; }
    [[nodiscard]] const float* twoPoleWeights()  const noexcept { return w2; }
    [[nodiscard]] const float* onePoleWeights()  const noexcept { return w1; }

    /** The four SVF tap coefficients the last `set` resolved, in the Nord's own
        order - stage 1 HP, stage 1 LP, stage 2 HP, stage 2 LP - for the gate that
        asserts they reduce to that machine's printed tap table at each of the
        four SHAPE points. Nothing in the audio path reads these. */
    [[nodiscard]] const float* svfTaps() const noexcept { return tap24; }
    /** What the two Nord tables and the input drop resolved to at the last
        `set`, for the gate that states them as numbers. */
    [[nodiscard]] float svfDamping24() const noexcept { return d24; }
    [[nodiscard]] float svfDamping12() const noexcept { return d12; }
    [[nodiscard]] float svfInputGain24() const noexcept { return gIn24; }
    [[nodiscard]] float svfInputGain12() const noexcept { return gIn12; }

    /** [EAR] the one constant in the peak-position correction above, fitted to
        the measured offset at two pole splits two octaves apart. It came out the
        same to within six per cent at both, which is why it is one number and
        not a table. */
    static constexpr float kPeakTrim = 0.285f;
    /** [REF] the ceiling on that correction. Five semitones and a bit, which
        is as far as the poles can usefully be moved before they are simply
        being pushed at Nyquist. */
    static constexpr float kMaxPeakTrim = 1.35f;
    /** [EAR] where the output cell's own knee sits, chosen so it is transparent
        at every level a program reaches and bounds the block at 0.9072/0.35. */
    static constexpr float kOutputCell = 0.35f;
    /** [EAR] a floor rather than a clamp at zero: a one-pole at 0 Hz is a
        latch, and an envelope that closes the filter all the way would freeze
        the state rather than silence it. */
    static constexpr float kFloorHz = 8.0f;
    /** [EAR] the two ends of DRIVE's own travel, as a gain into the limiter.
        Below unity at the bottom so the cell is nearly transparent and the
        resonance can sing; far enough above it at the top that the term is well
        past both turning points at any resonance that rings. Exponential
        between, because a gain knob lives in the log domain. */
    static constexpr float kDriveMinGain = kDriveBottomGain;
    static constexpr float kDriveMaxGain = 5.0f;

private:
    /** The TPT one-pole. Zero-delay WITHIN the stage, which is what keeps the
        corner where it was asked for as the cutoff sweeps: a naive one-pole
        detunes downward as the corner rises and misbehaves near sr/4, and an
        envelope sweep spends its attack exactly there. The LOOP around the
        cascade is a sample late, and has to be - there is a saturator in it, and
        a nonlinearity cannot be resolved in closed form. Both of the
        catalogue's analogue ladders (Synthex, OB-Xa) do the same. */
    [[nodiscard]] float poleG (float hz) const noexcept
    {
        const float g = std::tan (3.14159265358979f * hz / (float) sr);
        return g / (1.0f + g);
    }
    [[nodiscard]] static float onePole (float& z, float x, float G) noexcept
    {
        const float v = (x - z) * G;
        const float y = v + z;
        z = y + v;
        return y;
    }

    // ------------------------------------------------ the alternate topologies
    /** Types 1 and 2 share this coefficient prologue with type 0 and then part
        company. It is duplicated rather than hoisted, and deliberately: hoisting
        it would reorder the floating-point arithmetic in the type-0 path, and
        the one promise this change makes is that type 0 is bit-identical to what
        the machine did before the argument existed. Nine lines is a cheap price
        for a promise that can be measured. */
    void setAlt (float cutoffHz, float resonance01, float shape03, float width01,
                 float drive01, float slopeDb) noexcept
    {
        morph = slopeMorph (slopeDb);
        res   = std::clamp (resonance01, 0.0f, 1.0f);
        shape = std::clamp (shape03, 0.0f, 3.0f);
        drive = std::clamp (drive01, 0.0f, 1.0f);

        shapeIndex = std::min ((int) shape, 2);
        shapeFrac  = shape - (float) shapeIndex;
        const float edges[4] { kBandnessOneEdge, 1.0f, kBandnessOneEdge, 1.0f };
        const float bandness = edges[shapeIndex] * (1.0f - shapeFrac)
                             + edges[shapeIndex + 1] * shapeFrac;
        const float halfSemis = std::clamp (width01, 0.0f, 1.0f)
                              * kWidthMaxSemis * 0.5f * bandness;
        spr = std::exp2 (halfSemis / 12.0f);
        rSq = spr * spr;

        const float nyq = (float) (sr * 0.45);
        dGain = kDriveMinGain * std::pow (kDriveMaxGain / kDriveMinGain, drive);
        dInv  = 1.0f / dGain;

        // WIDTH SPLITS THE TWO SECTIONS, exactly as it splits the pole pairs in
        // type 0 and for the same reason: the geometric centre of the split is
        // where CUTOFF put it, so opening the band cannot move the corner. On
        // the SVF's 12 dB anchor the high section is the SHELF and the low one
        // carries the resonance, so WIDTH there is the distance between the
        // colour and the corner - which is a real control on that arrangement
        // rather than a courtesy.
        const float fHi = std::clamp (cutoffHz * spr, kFloorHz, nyq);
        const float fLo = std::clamp (cutoffHz / spr, kFloorHz, nyq);

        if (type == kTypeSvf)
        {
            // THE SVF'S `g` IS THE RAW TANGENT, not the one-pole's `g/(1+g)`.
            // Two different normalisations of the same prewarp, and mixing them
            // up detunes the corner by an octave at the top of the range.
            gSvfHi = std::tan (3.14159265358979f * fHi / (float) sr);
            gSvfLo = std::tan (3.14159265358979f * fLo / (float) sr);
            d24 = nordDampingQuadratic (res);
            d12 = nordDampingCubic (res);
            gIn24 = nordInputGain (d24);
            gIn12 = nordInputGain (d12);
            sat24 = 1.0f - d24;
            sat12 = 1.0f - d12;
            svfTapWeights();
        }
        else // kTypeCascade
        {
            gHi4 = poleG (fHi); gLo4 = poleG (fLo);
            gHi2 = gHi4;        gLo2 = gLo4;
            kc4 = res * kCascadeK24;
            kc2 = res * kCascadeK12;
            // The node's own gain pair, replacing the feedback term's. See
            // kCascadeNodeMin above: this is a clamp on a summing node and not a
            // limiter on a normalised term, and the two want different numbers.
            dGain = kCascadeNodeMin * std::pow (kCascadeNodeMax / kCascadeNodeMin, drive);
            dInv  = 1.0f / dGain;
            // AND A RAIL ON THE LOOP GAIN, WHICH IS A SAFETY AND NOT A
            // CALIBRATION - the distinction matters, because type 0 normalises
            // its loop gain at the crossing on EVERY setting and that is what
            // makes its resonance knob mean one thing at every cutoff. Doing
            // that here would be lending this type type 0's calibration, and the
            // raw knob IS the SSM2044's character: on that chip the resonance
            // does not mean the same thing at every corner.
            //
            // What is not character is divergence. A one-pole cascade with a
            // sample of delay in its loop reaches -180 degrees BELOW its corner,
            // and the cascade's magnitude down there is much larger than at the
            // corner, so the loop gain the knob asked for arrives multiplied - at
            // a 12 kHz corner, measured, 1.94 rms out of a 0.231 rms input at
            // resonance 0.60, and 1.74 at 20 kHz. So the crossing is solved for
            // in closed form, ONE pass and no iteration because this is a rail
            // rather than a law, and the gain is pulled back only where the loop
            // would otherwise reach unity. WHERE THAT IS, measured: at the top of
            // the resonance knob it engages at every corner - the four-pole loop
            // reaches 1.14 at a 1 kHz corner - and it reaches further down the
            // knob as the corner rises, which is exactly the shape of the problem
            // it is for. The visible cost is 1.2 dB: at resonance 1.0 and a 1 kHz
            // corner the passband reads -12.94 dB where supersynth's own ladder,
            // unrailed and unstable, reads -14.15.
            // The corner in radians per sample, which is also one sample of
            // feedback delay in radians - the same quantity type 0's `w` is.
            const float wc = 6.28318531f * std::min (cutoffHz, nyq) / (float) sr;
            const float cc = spr + 1.0f / spr;
            const float tt = std::tan (std::min (1.5533f, wc * 0.5f));
            const float aa = cc * tt;
            const float xx = 0.5f * (std::sqrt (aa * aa + 4.0f) - aa);
            const float hi = xx / spr, lo = xx * spr;
            const float magC = 1.0f / ((1.0f + hi * hi) * (1.0f + lo * lo));
            const float loop = kc4 * magC;
            if (loop > kCascadeLoopMax)
            {
                const float pull = kCascadeLoopMax / loop;
                kc4 *= pull; kc2 *= pull;
            }
            // THE PER-SHAPE BODY STAYS ON AND THE PER-RESONANCE BODY DOES NOT,
            // and that split is the whole of this type. They are two different
            // compensations that the SUPER type happens to multiply together:
            // one matches the four SHAPES to each other so the morph does not
            // change loudness as it passes through, which every type needs and
            // which no machine's character depends on; the other gives back what
            // the RESONANCE took off the passband, which is exactly the thing
            // `~/projects/synthcatalog/filters/korg/polysix.md` says the SSM2044
            // does not have. Seeding these at 1 and letting `weights()`
            // multiply the shape body in is how one line says both.
            body4 = body2 = 1.0f;
            weights (false);
            shapeBody (kCascadeBody4, kCascadeBody2);
            // AND DRIVE'S MAKE-UP, FULLY ENGAGED, because this type's limiter is on
            // the summing node and the node runs at every resonance - including 0,
            // which is where the fault was worst. Both anchors take the same
            // number because both nodes take the same clamp with the same gain.
            //
            // ON THE OUTPUT AND NOT ON THE NODE. `u` is the loop's forward path, so
            // a make-up there multiplies the loop gain by the same amount and
            // walks straight past `kCascadeLoopMax`'s rail, which is computed
            // without it. `body4` and `body2` are outside the loop.
            const float mk = driveMakeup (dGain, kDriveMakeupCascade, 1.0f);
            body4 *= mk; body2 *= mk;
        }
    }

    /** The four SVF tap coefficients, interpolated between the Nord's own four
        printed pairs. Linear, because the output is linear in them - the same
        argument the Synthex weights make one section up. */
    void svfTapWeights() noexcept
    {
        const int i = shapeIndex; const float f = shapeFrac;
        for (int n = 0; n < 4; ++n)
            tap24[n] = kNordTaps[i][n] * (1.0f - f) + kNordTaps[i + 1][n] * f;
        // [EAR], MEASURED, and its own numbers rather than the ladder's. Check 9c
        // prints the white-noise rms of each SVF shape and fails if any of them
        // drifts more than 1.5 dB from the lowpass, which is the same gate and
        // the same tolerance check 6 puts on type 0. Borrowing kBodyBp4 here
        // would have been the tidy thing and would have been wrong: an SVF
        // highpass tap and a four-pole binomial highpass are not the same
        // response and do not lose the same energy.
        body4 = body2 = 1.0f;
        shapeBody (kSvfBody4, kSvfBody2);
        // AND DRIVE'S MAKE-UP, PER ANCHOR AND SCALED BY ITS OWN `1 - d`. See the
        // `driveMakeup` block: this type's limiter sits on the bandpass integrator
        // state, so what DRIVE takes is the resonant PEAK, and the factor that says
        // how much of the cell is engaged is the same `sat` that mixes it in. At
        // resonance 0 both are 0, the make-up is exactly 1.000, and DRIVE still
        // does nothing here - which is [FW] this machine's own behaviour.
        //
        // WHAT IT RESTORES IS AN AVERAGE AND NOT A BODY, and `driveMakeup`'s block
        // has the measurement: this type's 40 Hz passband does not move by one count
        // across DRIVE's whole travel, so there is no lost body here to give back.
        // The make-up is a compressor's, and it lifts the body above where it
        // started - which is [MAN] the JD-800's law reached by the other mechanism.
        body4 *= driveMakeup (dGain, kDriveMakeupSvf, sat24);
        body2 *= driveMakeup (dGain, kDriveMakeupSvf, sat12);
    }

    /** The per-shape body, for the types that have their own table. Interpolated
        with the same fraction as the taps, so the morph is level-continuous
        through every point between - which is the property check 2a measures and
        which a table lookup would break at each of the three joins. */
    void shapeBody (const float (&b4)[4], const float (&b2)[4]) noexcept
    {
        const int i = shapeIndex; const float f = shapeFrac;
        body4 *= b4[i] * (1.0f - f) + b4[i + 1] * f;
        body2 *= b2[i] * (1.0f - f) + b2[i + 1] * f;
    }

    /** The in-loop cell, on the BANDPASS INTEGRATOR STATE. supersynth's own
        placement and the reason its SVF survives `k = 0`; the limiter itself is
        the Virus's, which is the one saturator this machine has, and DRIVE is
        its gain exactly as it is in type 0. */
    void saturateState (float& ic1, float amount) const noexcept
    {
        if (amount <= 0.0f) return;
        const float lim = asymmetricLimit (ic1 * dGain) * dInv;
        ic1 += amount * (lim - ic1);
    }

    [[nodiscard]] float processAlt (float x) noexcept
    {
        if (type == kTypeSvf)
        {
            // ---- the 24 dB anchor: TWO resonant stages, the quadratic table --
            // [FW] Nord Lead 2 types 1-5. Q tops out at 32.5 and this anchor
            // never self-oscillates, which is the machine's own statement and is
            // the opposite of the 12 dB one below it.
            {
                float lp, bp, hp;
                const float u = x * gIn24;
                svf[0].run (u, gSvfHi, d24, lp, bp, hp);
                const float mid = tap24[0] * hp + tap24[1] * lp;
                saturateState (svf[0].ic1, sat24);
                svf[1].run (mid, gSvfLo, d24, lp, bp, hp);
                saturateState (svf[1].ic1, sat24);
                out4 = (tap24[2] * hp + tap24[3] * lp) * body4;
            }
            // ---- the 12 dB anchor: a SHELF into one resonant stage -----------
            // [FW] Nord Lead 2 type 0. The first stage's damping is forced to
            // 1.0 and its taps are the shelf, so it stops contributing slope;
            // the second carries the CUBIC table and reaches Q 500, and this is
            // the anchor that screams. Both stages still run - the machine never
            // switches one off, it gets it out of the way.
            {
                float lp, bp, hp;
                const float u = x * gIn12;
                svf[2].run (u, gSvfHi, 1.0f, lp, bp, hp);
                const float mid = kNordShelfHp * hp + kNordShelfLp * lp;
                svf[3].run (mid, gSvfLo, d12, lp, bp, hp);
                saturateState (svf[3].ic1, sat12);
                out2 = (tap24[2] * hp + tap24[3] * lp) * body2;
            }
        }
        else // kTypeCascade
        {
            // FOUR ONE-POLES, FEEDBACK TO THE INPUT NODE, and the node is where
            // the limiter sits - which is [SCH] the Polysix's D7/D8 diode pair
            // and supersynth's `tanh` on the same node, reached twice.
            {
                const float u = asymmetricLimit ((x - kc4 * fbc4) * dGain) * dInv;
                const float t1 = onePole (zc4[0], u,  gHi4);
                const float t2 = onePole (zc4[1], t1, gHi4);
                const float t3 = onePole (zc4[2], t2, gLo4);
                const float t4 = onePole (zc4[3], t3, gLo4);
                fbc4 = t4;
                out4 = (w4[0] * u + w4[1] * t1 + w4[2] * t2 + w4[3] * t3 + w4[4] * t4)
                     * body4;
            }
            {
                const float u = asymmetricLimit ((x - kc2 * fbc2) * dGain) * dInv;
                const float t1 = onePole (zc2[0], u,  gHi2);
                const float t2 = onePole (zc2[1], t1, gLo2);
                fbc2 = t2;
                out2 = (w2[0] * u + w2[1] * t1 + w2[2] * t2) * body2;
            }
        }
        // The same output rail as type 0, and for the same reason: a filter's
        // output is bounded by what it is built out of. It is the ONE thing
        // every type shares downstream of its own loop.
        //
        // THE THIRD ANCHOR IS TYPE 0's ALONE. Types 1-3 are three machines' own
        // arrangements and none of those machines has a first-order position to
        // model, so a third anchor there would be an invention wearing a brand
        // name - which is the one thing this file's schema forbids. Their SLOPE
        // keeps its one leg and 12 is the bottom of it: below 12 `slopeMorph` is
        // already clamped at 0, so they hold their 12 dB anchor exactly as they
        // did before this leg existed.
        const float y = out2 * (1.0f - morph) + out4 * morph;
        return asymmetricLimit (y * kOutputCell) / kOutputCell;
    }

    /** [EAR], MEASURED, AND ON A DIFFERENT METER FROM TYPE 0's - which is the
        one thing to know about these eight numbers.

        `kBodyBp4` and its three neighbours were fitted to a WHITE-NOISE rms at
        the reference width. These were fitted to the C3 SAWTOOTH `megasynth_filter`
        check 6 actually uses, because that is what this filter sees and because
        check 6's own header says so out loud: "white noise would ask a different
        question and answer it wrongly - a highpass keeps 23 of 24 kHz of white
        noise and almost none of a saw's energy". Measured on the saw, the four
        shapes of each new type land within 0.1 dB of each other where type 0's
        own numbers leave it 8.97 dB apart and one nudge from the gate.

        TYPE 0's ARE NOT BEING CORRECTED. They are what 205 stored programs were
        voiced against, and the eight numbers here are what a type gets for
        arriving after the freeze rather than before it.

        WHAT WOULD RETIRE THESE: nothing external. They are a loudness match
        between four shapes of one filter, so the only thing that changes them is
        a better meter, and check 9c prints the four numbers they came from every
        run. */
    static constexpr float kSvfBody4[4] { 1.0f, 2.213f, 3.281f, 1.039f };
    static constexpr float kSvfBody2[4] { 1.0f, 1.980f, 2.227f, 1.078f };
    static constexpr float kCascadeBody4[4] { 1.0f, 1.908f, 4.281f, 0.920f };
    static constexpr float kCascadeBody2[4] { 1.0f, 1.564f, 3.503f, 1.066f };

    /** THE WEIGHT VECTORS, and every one of them is a Synthex mode generalised.

        Read them as responses rather than as numbers. With the four poles
        ordered [hi, hi, lo, lo] the taps are prefix products, so

            t2                  = LP^2 at the HIGH corner
            t4                  = LP^4 spanning both corners
            t2 - 2 t3 + t4      = LP^2(hi) * HP^2(lo), a band from lo to hi
            t0 - 4t1 + 6t2 ...  = the fourth-order highpass

        and the scalars in front of them are what puts each one at unity where it
        belongs: the bandpass at its centre, the highpass at Nyquist, the lowpass
        at DC. At width zero every scalar collapses to the Synthex's own printed
        integer, which is the check that this is a generalisation and not a
        different filter wearing its name.

        THE HIGHPASS CARRIES A WIDTH CORRECTION and it is the one term with no
        machine behind it. A binomial highpass nulls DC because its weights sum
        to zero, and that survives the poles being split - but its FIRST-order
        term does not, so a wide highpass would leak a 6 dB/oct skirt at about
        -16 dB instead of rolling off at 24. Moving `dHp` between the last two
        weights nulls that term too and costs one divide. A resistor network with
        the poles detuned would have to be retrimmed in exactly the same way. */
    void weights (bool applySuperBody = true) noexcept
    {
        // The bandpass normalisers: the loop's own magnitude at the centre,
        // inverted, so the peak sits at unity however wide the band is. r = 1
        // gives 4 and 2, which are the Synthex's `4(y2-2y3+y4)` and `2(y1-y2)`.
        const float sBp4 = (rSq + 1.0f) * (rSq + 1.0f) / (rSq * rSq);
        const float sBp2 = (rSq + 1.0f) / rSq;
        const float dHp4 = 2.0f * (1.0f / rSq - 1.0f);   // 0 at width 0
        const float dHp2 =        (1.0f / rSq - 1.0f);   // 0 at width 0

        // The four anchor vectors, then a straight interpolation between the two
        // the SHAPE control is sitting between. The output is linear in the
        // weights, so this IS a continuous summing network and not a crossfade
        // of four filters - which is the distinction the Synthex's card makes.
        const float aLp4[5] { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
        const float aBp4[5] { 0.0f, 0.0f, sBp4, -2.0f * sBp4, sBp4 };
        const float aHp4[5] { 1.0f, -4.0f, 6.0f, -4.0f + dHp4, 1.0f - dHp4 };
        // NOTCH IS THE INPUT MINUS THE NORMALISED BANDPASS, which is what an SVF
        // does (`notch = x - k*bp`), what the Nord Lead 3 calls BR (`lp + hp`)
        // and what [MAN] the Supernova's Notch type is (parallel lp + hp, its
        // second section offset by WIDTH). Three machines, one answer.
        //
        // AND IT IS WHY THE NOTCH IS NOT SUBTLE. The bandpass is at unity at the
        // centre by construction, so the difference NULLS there exactly - and
        // both terms are shaped by the same resonant loop, so the resonance
        // lands on the shoulders either side of a null it cannot fill. "A notch
        // must be plural": open WIDTH and the single null becomes a rejection
        // band with a resonant edge on each side of it.
        const float aNo4[5] { 1.0f, 0.0f, -sBp4, 2.0f * sBp4, -sBp4 };
        const float aLp2[3] { 0.0f, 0.0f, 1.0f };
        const float aBp2[3] { 0.0f, sBp2, -sBp2 };
        const float aHp2[3] { 1.0f, -2.0f - dHp2, 1.0f + dHp2 };
        const float aNo2[3] { 1.0f, -sBp2, sBp2 };

        const int   i = shapeIndex;
        const float f = shapeFrac;
        const float* const p4[4] { aLp4, aBp4, aHp4, aNo4 };
        const float* const p2[4] { aLp2, aBp2, aHp2, aNo2 };
        for (int n = 0; n < 5; ++n) w4[n] = p4[i][n] * (1.0f - f) + p4[i + 1][n] * f;
        for (int n = 0; n < 3; ++n) w2[n] = p2[i][n] * (1.0f - f) + p2[i + 1][n] * f;

        // BODY PER SHAPE, always on, measured. `megasynth_filter` check 5 prints
        // the white-noise rms of each anchor shape at the reference width and
        // these are what it measured, normalised so LP is 1.000 - so the four
        // shapes arrive at the same loudness and nothing goes thin. It is
        // interpolated with the same fraction as the weights, so the morph is
        // level-continuous through every point between.
        //
        // WIDTH IS NOT COMPENSATED AND MUST NOT BE. A band that widens has to
        // pass more energy or it is not opening, and that is the entire answer
        // to "nothing happens in bandpass when you touch cutoff". The reference
        // is the width knob's own default.
        // THE OPT-OUT IS FOR THE OTHER TYPES AND FOR NOTHING ELSE. Type 2 shares
        // this tap network and does NOT share these numbers, because they were
        // measured on THIS anchor pair with its trims and its makeup, and
        // "measured per configuration against itself" is not a slogan - lending
        // them out is the blend-toward-a-shared-reference mistake wearing a
        // different coat. Default true, so type 0's arithmetic is untouched.
        if (! applySuperBody) return;
        const float shapeBody4[4] { 1.0f, kBodyBp4, kBodyHp4, kBodyNo4 };
        const float shapeBody2[4] { 1.0f, kBodyBp2, kBodyHp2, kBodyNo2 };
        body4 *= shapeBody4[i] * (1.0f - f) + shapeBody4[i + 1] * f;
        body2 *= shapeBody2[i] * (1.0f - f) + shapeBody2[i + 1] * f;

        // THE ONE-POLE'S VECTOR, AND IT IS THE ONE VECTOR IN THIS FILE THAT IS
        // NOT INTERPOLATED. `kOnePoleOneEdged`'s block has the whole argument:
        // a first-order lowpass and highpass sum to the input, so interpolating
        // between them passes through half the raw input, and the cell is
        // therefore the lowpass below the bandpass point and the highpass above
        // it. The switch is at `shape == 1`, which is exactly where the cell's
        // reach is 0, so nothing in the output jumps.
        //
        // IT LIVES BELOW THE OPT-OUT because the one-pole is type 0's alone, for
        // the same reason the six-pole was: types 1-3 are three machines' own
        // arrangements and none of them has a first-order position to model, so
        // an anchor there would be an invention wearing a brand name. Their SLOPE
        // holds its 12 dB anchor below 12 exactly as it held its 24 dB one above
        // 24 before this leg existed.
        const bool hp1 = shape >= 1.0f;
        w1[0] = hp1 ?  1.0f : 0.0f;
        w1[1] = hp1 ? -1.0f : 1.0f;
        body1 = hp1 ? kBodyHp1 : 1.0f;
    }

    // [EAR], MEASURED. See tools/filter_probe.cpp check 5, which prints the
    // numbers these came from and fails if any shape drifts more than 1.5 dB
    // from the lowpass it is matched to.
    static constexpr float kBodyBp4 = 1.60f, kBodyHp4 = 1.90f, kBodyNo4 = 0.83f;
    static constexpr float kBodyBp2 = 1.45f, kBodyHp2 = 1.78f, kBodyNo2 = 1.00f;
    /** [EAR], MEASURED ON THE ONE-POLE ITSELF, on check 6's own meter: a
        bandlimited C3 saw at a 1 kHz cutoff, resonance 0, width 0.5, DRIVE 0.
        White noise would ask a different question and answer it wrongly, which is
        check 6's own header.

        ONE NUMBER AND NOT THREE, because this cell has two shapes and not four -
        see `kOnePoleOneEdged`. Its lowpass is the row's 1.000 by construction and
        its highpass is this; the bandpass and the notch never read a body here
        because the lower leg of SLOPE does not travel at either of them.

        Measured on the C3 saw at a 1 kHz cutoff, resonance 0, width 0.5, DRIVE 0,
        the four shapes' rms at slope 6 - and remember that BP and NO here ARE the
        12 dB anchor's, because the lower leg does not travel at either:

            kBodyHp1   LP      BP      HP      NO      spread   9e's 6400
              1.00   0.5347  0.4039  0.1848  0.4839   9.23 dB   0 over
              2.00   0.5347  0.4039  0.3127  0.4839   4.66      0
              2.89   0.5347  0.4039  0.3752  0.4839   3.08      0
              3.40   0.5347  0.4039  0.4014  0.4839   2.49      0
              4.20   0.5347  0.4039  0.4362  0.4839   2.44      0
              5.00   0.5347  0.4039  0.4663  0.4839   2.44      8 over
            (slope 12  0.5168  0.4039  0.2121  0.4839   7.74 dB)
            (slope 24  0.4922  0.3424  0.1753  0.4925   8.97 dB)

        3.40 IS WHERE THE HIGHPASS STOPS BEING THE SHAPE THAT IS THIN, and past it
        nothing improves: from 3.50 up the spread is pinned at 2.44 dB by the 12 dB
        anchor's own BP-against-NO pair, which this number cannot reach. So more
        gain buys no match at all and buys broadband level at a low corner, which
        is what the 5.00 row costs: its 8 settings are all `cut 20, shape 2,
        slope 6` - a highpass at a 20 Hz corner is not filtering anything, so its
        shape body is simply a gain, and 1.068 rms comes out of a 0.231 rms input.
        The same failure the four-pole row solved silently, one order down: its
        1.90 is not the 2.81 a full match asks for either.

        AND A FULL MATCH IS UNREACHABLE HERE RATHER THAN DECLINED, which is new.
        At 1.000 the highpass wants 2.89 to reach the lowpass; at 2.89 it wants
        1.43 MORE, because the output rail compresses a first-order highpass of a
        saw - the waveform is spiky and its peaks find the knee. The fixed point is
        past 5.00, which is past what the sweep allows. 2.49 dB of spread is the
        tightest of the three anchors and is where this lands. */
    static constexpr float kBodyHp1 = 3.40f;   // [EAR] see above

    /** THE KERNEL LENGTH IN BASE SAMPLES, WHICH IS ALSO THE LATENCY. See
        `latencySamples()` for the measured table that settled it. */
    static constexpr int kOsTapsPerPhase = 16;
    // FULLY QUALIFIED FROM THE GLOBAL NAMESPACE, and it has to be. This class lives in
    // `dsp::filter`, so an unqualified `dsp::` here is looked up from inside `dsp` - and
    // in any translation unit that also has JUCE in scope there is a `juce::dsp`, so the
    // name is AMBIGUOUS. dspblocks' own tests do not include JUCE and compiled it
    // happily; Solera's first build against it did not. A block written for consumers
    // has to qualify.
    using Oversampler = ::dsp::Oversampler<kOsTapsPerPhase, 4, 1>;
    Oversampler ovs;
    /** TWO RATES AND THEY ARE NOT THE SAME NUMBER. `hostSr` is what the host
        asked for; `sr` is what `set` and `core` compute at, which is `hostSr`
        times the oversampling factor. At the default factor of 1 they are equal
        and every coefficient in this file is the one it always was. */
    double hostSr = 48000.0;
    int    os = 1;
    double sr = 48000.0;
    /** WHICH TOPOLOGY, and 0 is the fused one. Every other member below this
        line belongs to exactly one of the three and is untouched by the other
        two, which is what lets all three share one `reset`. */
    int   type = kTypeSuper;
    // type 1's state: four two-pole sections, two per SLOPE anchor.
    SvfStage svf[4] {};
    float gSvfHi = 0.1f, gSvfLo = 0.1f;
    float d24 = 1.0f, d12 = 1.0f, gIn24 = 1.0f, gIn12 = 1.0f;
    float sat24 = 0.0f, sat12 = 0.0f;
    float tap24[4] { 0.0f, 1.0f, 0.0f, 1.0f };
    // type 2's state: its own poles and its own loop, so switching type does not
    // pour one filter's state into another's.
    float zc4[4] {}, zc2[2] {};
    float fbc4 = 0.0f, fbc2 = 0.0f, kc4 = 0.0f, kc2 = 0.0f;
    float z4[4] {}, z2[2] {};
    float fb4 = 0.0f, fb2 = 0.0f, out4 = 0.0f, out2 = 0.0f;
    float gHi4 = 0.1f, gLo4 = 0.1f, gHi2 = 0.1f, gLo2 = 0.1f;
    float k4 = 0.0f, k2 = 0.0f, body4 = 1.0f, body2 = 1.0f;
    // the third anchor's own everything, and it is SEVEN floats where the cell it
    // replaced was twenty. It shares nothing with the other two but the input,
    // the SHAPE index and the output rail - not even the peak trim, because it
    // has no loop for one to correct.
    float z1 = 0.0f, out1 = 0.0f, g1 = 0.1f, body1 = 1.0f;
    float w1[2] { 0.0f, 1.0f };
    float morph1 = 0.0f;
    float morph = 1.0f, res = 0.0f, shape = 0.0f, drive = 0.0f;
    float spr = 1.0f, rSq = 1.0f, dGain = 1.0f, dInv = 1.0f, shapeFrac = 0.0f;
    float loopTrim = 1.0f;
    int   shapeIndex = 0;
    float w4[5] {}, w2[3] {};
};

} } // namespace dsp::filter
