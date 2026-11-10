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

// THE TWIN FILTER: the Alesis Andromeda A6's voice filter, which is TWO
// COMPLETE FILTERS PER VOICE RUNNING AT THE SAME TIME - and the first of them
// has no mode switch at all.
//
// The author asked for this machine by name, having owned one: "I am not saying to
// implement it. our filter already sounds amazing. I am just saying what I
// remember was that the cutoff knob on an andromeda had a huge effect on the
// sound", with the qualification he attached himself - "Again its years ago I
// had my andromeda." So that is a description of a machine and not a
// specification for ours, and nothing in this file is tuned toward it. What IS
// built here is what the paper prints, and the paper happens to agree about the
// cutoff: ten octaves and eleven out of one 16-bit word, both exact powers of
// two, reached by the whole factory corpus.
//
// This repo already has ONE filter behind a type selector (SuperFilter,
// `docs/super-filter-types.md`), and that is the opposite arrangement: one
// filter, three topologies, pick one. The A6 has two whole filters in two
// sections of one ASIC, both live, joined by a four-way input selector, and the
// first one's "mode" is a MIX of three simultaneous taps rather than a choice
// between them. Neither is reachable by adding a type to the filter that is
// already here, which is why this is its own block.
//
// ~/projects/synthcatalog/filters/alesis/andromeda.md (filter 1) and
// andromeda-filter2.md (filter 2) are the evidence; both are grade B+. Their
// two headers are the reference implementation and this file is a PORT of
// filter 1's arithmetic rather than a rewrite of it - the catalog tree is not on
// this target's include path (BankFx.h's RmsFollower says the same about the
// same tree), so the copy is field for field and the move back is a delete.
//
// ===========================================================================
// THE HEADLINE: TEN OCTAVES AND ELEVEN, OUT OF THE SAME NUMBER.
//
// `[MAN]` a6_manual p.128 prints both endpoints of both filters, and both
// ratios come out exact powers of two, which `[COR]` all 1,024 factory and
// designer programs reach:
//
//     filter 1   21.824 Hz .. 22.3482 kHz   22348.2 / 21.824 = 1024 = 2^10
//     filter 2   21.824 Hz .. 44.6960 kHz   44696.0 / 21.824 = 2048 = 2^11
//
//     f = 21.824 * 2^(word / 65535 * octaves)
//
// and `octaves` IS THE ONLY THING THAT DIFFERS BETWEEN THE TWO. Same 16-bit
// word, same bottom, and filter 2 an octave further at the top: that is a
// capacitor value on the ASF's integrators, not a firmware scale. THE TWO
// FILTERS DIVERGE AS YOU OPEN THEM - on the same frequency at word 0 and an
// octave apart at full. A model that gives both filters one span is right at the
// bottom and an octave wrong at the top, silently, which is why the law lives in
// this block and why the block takes a WORD rather than a frequency.
//
// THE WORD IS AN EXPONENTIAL INDEX AND THE SUM THAT FEEDS IT IS IN SEMITONES.
// `[FW]` 0x00247650 is twenty instructions and it is not specific to the cutoff:
//
//     asicWord(voice, control) = clamp16( base[control] + requested
//                                       + voiceTrim[voice][control] )
//
// carries oscillator pitch, pulse width, resonance and all ten VCAs. Every term
// is in semitones above the 21.824 Hz anchor, so the whole sum is ONE
// EXPONENTIAL away from a frequency. That shape is the machine's own and the
// API here is built in it: one anchor, one word, one exponential, and a span
// that differs by a number. `setCutoffSemitones1/2` is the sum's own domain and
// `setFrequencyWord1/2` is the word the clamp produces; nothing in this block
// takes Hz in.
//
// WHAT AN EXPONENTIAL WORD BUYS, in one comparison. Ten octaves spread evenly
// over the travel puts half travel at 21.824 * 2^5 = 698 Hz on filter 1 and
// 21.824 * 2^5.5 = 987 Hz on filter 2 - both in the middle of where a filter is
// played. A control of the same range that is LINEAR IN HERTZ puts half travel
// at about 11 kHz and crams eight of its octaves into the bottom six per cent
// of the knob. That is nearly four octaves of error at the middle of the travel,
// and it is the difference between a cutoff that feels enormous and one that
// feels like an on/off switch with a dead zone.
//
// AND THE TOP GOES PAST THE AUDIO BAND ON PURPOSE. 44.696 kHz is above Nyquist
// at every rate this plugin runs at. It is not an artefact: an analogue 4-pole
// whose corner is past the audio band is a wire with a resonant peak nobody can
// hear, and being able to put the filter fully out of the way is part of why
// that machine's cutoff feels limitless. SO THE WORD DOMAIN IS NEVER CLAMPED -
// every word 0..65535 is legal, reaches the block and is stable. What is clamped
// is the COEFFICIENT, at 0.45 of the sample rate, where the arithmetic needs it.
// `highestUnclampedWord1/2` says exactly where that starts biting at a given
// rate and `tools/twin_probe.cpp` check 12 measures that every word above it is
// still stable.
//
// ===========================================================================
// THE CONTROL PATH IS A 500 Hz STAIRCASE AND THE INTERPOLATION IS A PROGRAM
// PARAMETER. This is in the block because it is a fact about the machine being
// ported, not because of anything anyone remembers about how it sounded.
//
// `[SCH]` sheet 40: the ASF takes five address lines, one digital data line and
// ONE ANALOGUE data line, and `[FW]` 0x002770EC DMAs a 2736-byte shadow image -
// 684 longs of [16 switch bits][16-bit DAC word] - into the chips EVERY 2 ms.
// Every control on this voice is a voltage held on a capacitor and refreshed 500
// times a second, and there is no sample-rate-smooth parameter anywhere in it.
// That is why the program record carries `Oscillator Smoothing Type` and `Filter
// Smoothing Type` (bytes 1907:0-3 and 1907:4-7) at all, and why OS 1.40.12's
// release note for its new ULTRA fast setting warns of "clicks and other
// artifacts" `[MAN]` a6notes.pdf.
//
// The catalog's sentence is the one this block is built against: "Smooth the
// control path and the A6 stops sounding like an A6 in a way no frequency
// response will show." So the staircase is ON by default here, it is not an
// implementation detail to optimise away, and the interpolation across the gap
// is a PARAMETER with the machine's own fastest setting as one of its positions.
// `setControlPath (kPerSample)` is the other side of the A/B and is labelled as
// ours, because no A6 can do it.
//
// AND THE TWO FILTERS SHARE ONE SET OF TIMING CAPACITORS AND ONE TEMPERATURE.
// `[SCH]` sheet 40 gives the ASF four external caps per voice (C1A1-C4A1,
// C1A2-C4A2) serving BOTH filter sections, which is why `[FW]` 0x00245FCA's
// autotune corrects PER VOICE and not per filter section. Any per-voice
// variation a caller models has to move BOTH filters together; this block holds
// one word per filter and no per-voice term, and that zero is a CLAIM.
//
// ===========================================================================
// THE THREE THINGS A HYBRID BREAKS HERE BY ACCIDENT.
//
// 1. THERE IS NO MODE SWITCH ON FILTER 1. `[MAN]` p.117, "designed to emulate
//    the Oberheim SEM": a 2-pole state variable whose LOW, BAND and HIGH outputs
//    are ALL LIVE AT ONCE, each with its own level knob in POST FILTER MIX, and
//    a notch is LP + HP with BP down. Alesis printed the word NOTCH with an
//    arrow between the FILTER 1 LOW PASS and HIGH PASS knobs on the faceplate
//    `[PUB]`, which is the machine stating its own topology in metal. THE MODE
//    IS THE MIX. There is no selector anywhere in this block and there must
//    never be one: modelling it as three filters gives the wrong slopes, because
//    the slopes only come out right from ONE SHARED PAIR OF POLES, and modelling
//    it as one filter with a mode selector loses what the machine is for.
//
// 2. KEY FOLLOW IS IN SEMITONES, SO A SEMITONE IS NOT THE SAME NUMBER OF COUNTS
//    ON THE TWO FILTERS: 546.1 on filter 1 and 496.5 on filter 2, because the
//    spans differ. Convert once and reuse the number on both and filter 2 tracks
//    the keyboard 10 per cent sharp, which reads as the two filters drifting
//    apart up the keyboard and nowhere else. `wordPerSemitone1/2` exist so that
//    conversion cannot be done once; check 11 measures it.
//
// 3. FILTER 2 IS CALLED, NOT REIMPLEMENTED. `[MAN]` p.117 says it is a 4-pole
//    "designed to emulate the Moog modular", and `megasynth::SuperFilter`'s
//    24 dB anchor is already a four-pole ladder with negative feedback from the
//    last pole, the Virus limiter in the loop and two calibrated anchors. This
//    block SETS it and READS it, exactly as `BankFx`'s SUPER core does, and
//    modifies nothing in it. Which of SuperFilter's three types is not a
//    preference: `[MAN]` p.128 states that filter 2 "can generate tones on its
//    own when in full resonance", and of the three only type 0's 24 dB anchor
//    self-oscillates (`docs/super-filter-types.md`'s ring table: 3.000 s against
//    SVF's 0.090 and CASCADE's 0.095). The one anchored resonance fact on this
//    machine picks the core. Check 5 re-measures it here.
//
// AND FILTER 1 IS NOT GIVEN SELF-OSCILLATION ON THE STRENGTH OF FILTER 2 HAVING
// IT. The manual states it for filter 2 and is silent about filter 1; the
// catalog refuses to assume it from the neighbouring section on the same die and
// so does this. Check 5 measures the asymmetry rather than asserting it.
//
// ===========================================================================
// WHAT THIS BLOCK IS NOT, and each absence is a decision rather than a gap.
//
// NO DRIVE, NO WIDTH, NO SHAPE ON FILTER 2. Nothing in `[SCH]`, `[MAN]` or
// `[FW]` reaches inside the ASF to say what its stages do when driven, and the
// catalog declines to claim a Moog saturation shape for exactly that reason.
// SuperFilter's DRIVE is passed 0 (its limiter is still BOUNDED there, which is
// what keeps full resonance finite), WIDTH 0 and SHAPE lowpass, because filter 2
// has ONE tap: `FILT2LPLEVEL` is the only filter-2 entry on POST FILTER MIX
// `[MAN]` chart page 20, against filter 1's three.
//
// NO CUTOFF SUM. `[FW]` 0x00247650 is the PROCESSOR's arithmetic here as it was
// the ColdFire's on the A6 - megasynth's own split, "Engine never sees a
// Program" - so this block takes the word the sum produced, in the sum's own
// semitone domain. Two of that sum's three terms have nothing behind them in the
// patch (`base[]` at 0x2D1F30 and `voiceTrim[]` at 0x2D6800) and are unmodelled
// here as they are in the catalog; the catalog's `CutoffTerms` enumerates them.
//
// NO 16-BIT QUANTISATION OF THE WORD. One LSB is 0.026 cents on filter 1, which
// is below anything; the STAIRCASE is the audible half of that mechanism and it
// is built. `[REF]`, and what would retire it is a measurement showing the
// quantisation itself is audible under a slow sweep.
#pragma once

#include "SuperFilter.h"   // filter 2 IS megasynth::SuperFilter - called, never copied

#include <algorithm>
#include <cmath>

namespace dsp { namespace filter
{

/** What the twin filter declares about its surroundings. Field for field the
    vocabulary of synthcatalog/module.h's `ModuleCard` + `Assumptions`, spelled
    out here because that tree is not on this target's include path yet; when it
    is, this collapses into the catalog's own card. Same shape and same reason as
    `BankCard` in BankFx.h. An UNKNOWN assumption is a routing hazard and is
    flagged as loudly as an `[EAR]` law, which is the catalog's rule and the
    reason this is a struct rather than a comment. */
struct TwinCard
{
    double      hardwareRateHz;      ///< 0 = analogue, rate-independent
    double      controlRateHz;       ///< NOT the audio rate: the S&H refresh
    const char* rateNote;
    double      inputNominalPeak;
    const char* inputNote;
    bool        partnerCoDesigned;   ///< true = do not lift one half out alone
    const char* partnerNote;
    const char* calibrationNote;     ///< the terms with no patch byte behind them
    const char* signature;
};

inline constexpr TwinCard kTwinCard {
    0.0,
    500.0,
    "analogue, so the audio path is rate-independent - but THE CONTROL PATH IS A "
    "500 Hz SAMPLE-AND-HOLD BANK, [SCH] sheet 40 + [FW] 0x002770EC, and the "
    "machine ships a CHOICE of interpolation across the 2 ms gap (Filter "
    "Smoothing Type, record bytes 1907:4-7). Modelled here, on by default, and "
    "the thing to leave alone.",
    1.0,
    "the PRE FILTER MIX: two oscillators with four simultaneous waveform LEVELS "
    "each, two subs, a ring modulator and three colours of noise, summed into an "
    "op-amp that runs out of rail smoothly [SCH] sheet 40",
    true,
    "CO-DESIGNED AND PHYSICALLY JOINED. Four external capacitors per voice serve "
    "BOTH filter sections [SCH] sheet 40, so they share one temperature and drift "
    "together, and the autotune corrects per VOICE rather than per section [FW] "
    "0x00245FCA. Any per-voice variation moves BOTH filters at once. Patching "
    "either half alone loses the selector and the shared drift.",
    "THE PATCH DOES NOT CONTAIN THE CUTOFF. [FW] 0x00247650 writes "
    "clamp16(base[control] + requested + voiceTrim[voice][control]); base[] at "
    "0x2D1F30 is a per-control global filled at init and voiceTrim[] at 0x2D6800 "
    "is the autotune's per-voice correction, and NEITHER has a patch byte behind "
    "it. Sixteen analogue voices agree because of them. Both are zero here, and a "
    "zero in an unmodelled row is a CLAIM.",
    "Ten octaves and eleven out of the same 16-bit word, so the two filters agree "
    "at the bottom and diverge by a whole octave at the top - and the mode is the "
    "MIX, three live taps with three level knobs rather than a selector, on a "
    "control path that is a 500 Hz staircase by design."
};

// ============================================================ the block =====
/** The A6 voice filter: filter 1 and filter 2, both running, joined by filter
    2's four-way input selector.

    ONE CHANNEL. The state is one voice's, the way `SuperFilter` is one voice's
    and the way the hardware is (one ASF section pair per voice). A caller that
    wants stereo holds two, which is what `BankFx` does with its own SuperFilter
    members. No patch, no parameters, no branch on a program: the processor
    computes every coefficient and this holds blocks.

    THE SIGNAL, in one place, because every comment below is a claim about it:

        in --+--> FILTER 1 (2 poles, ONE pair) --+--> lp * LP LEVEL ---+
             |                                   +--> bp * BP LEVEL ---+--> out
             |                                   +--> hp * HP LEVEL ---+
             |                                                         |
             +--> [ FILTER 2 INPUT SELECTOR ] --> FILTER 2 (4 poles) --+
                    off      nothing                   * FILT 2 LEVEL
                    mix      the pre-filter mix        (parallel)
                    notch    lp*LP + hp*HP, POST-fader (series)
                    bp       the band-pass tap, PRE-fader (series)

    IN NOTCH THE LP AND HP FADERS STOP FEEDING THE VCA and become filter 2's
    input mix; in BP the band-pass tap reaches filter 2 whatever the BP fader
    says. That asymmetry is `[MAN]` p.128 and only the manual states it. Get it
    backwards and NOTCH patches go silent when the player turns the LP and HP
    knobs down, while BP patches sound dead when the BP knob is down. Check 6
    measures both halves with every filter-1 fader at zero. */
class TwinFilter
{
public:
    /** `[MAN]` p.128 + the a6syx chart's `FILT_2 INPUT_POT` (page 19, child 4,
        range 0-3). FOUR POSITIONS, AND IT IS A SELECTOR RATHER THAN A CROSSFADE:
        the machine's own control is a 0-3 pot and nothing in `[MAN]`, `[SCH]` or
        the chart offers a blend between two of them. Series and parallel are
        BOTH here because the machine has both; a crossfade between them would be
        ours, and there is no evidence for it, so it is not offered. */
    enum Input : int
    {
        /** Filter 2 gets nothing - AND THIS IS NOT BYPASS. `[MAN]` p.128: "it
            can generate tones on its own when in full resonance." A filter with
            no input and full resonance is a sine generator on the voice's output
            bus, and that is a documented use of the machine. Its level knob
            still reaches the VCA in this position. */
        kInputOff      = 0,
        /** The PRE FILTER MIX, the same signal filter 1 sees: PARALLEL. */
        kInputMix      = 1,
        /** Filter 1's LP + HP sum, POST-FADER: SERIES, and the two faders stop
            feeding the VCA. */
        kInputNotch    = 2,
        /** Filter 1's band-pass tap, PRE-FADER: SERIES, and the BP fader does
            not have to be up for anything to arrive. */
        kInputBandpass = 3,
        kNumInputs     = 4
    };

    // ------------------------------------------------------------- the law ---

    /** `[MAN]` p.128, printed: the bottom of BOTH filters. THE ANCHOR AND THE
        SPAN ARE TWO SEPARATE CLAIMS out of one sentence and they fail
        differently - a span falls out of a ratio, an anchor needs one absolute
        measurement - so they are two constants here and not one. `[COR]` all
        1,024 factory and designer programs reach both endpoints on both
        channels. */
    static constexpr double kAnchorHz = 21.824;
    /** `[MAN]`+`[DER]` 22348.2 / 21.824 = 1024 = 2^10 exactly. */
    static constexpr double kOctaves1 = 10.0;
    /** `[MAN]`+`[DER]` 44696.0 / 21.824 = 2048 = 2^11 exactly. ONE OCTAVE MORE
        OUT OF THE SAME WORD, which is a capacitor on the integrators. */
    static constexpr double kOctaves2 = 11.0;
    /** The record's own control word, `[MAN]` a6syx p.7: 0..65535, written
        straight to the chip as an "Actual ASIC Control Value". */
    static constexpr double kWordMax = 65535.0;

    /** A 0..1 control word into Hz, once, per filter. `[MAN]` p.128.

        The catalog prints 6553.6 and 5957.8 counts per octave, which is
        65536 / octaves; dividing the record's own 0..65535 by 65535 gives 6553.5
        and 5957.7 and lands the top of the travel on 22347.8 Hz and 44695.6 Hz,
        which is exactly what `[COR]` reports the corpus reaching. The two
        readings differ by one count in 65536, which is 0.026 cents. This block
        uses the record's range because the word is an integer the machine wrote.

        HALF TRAVEL IS 698.4 Hz AND 987.9 Hz. That is the number to hold on to
        when somebody proposes a cutoff control that is linear in hertz. */
    [[nodiscard]] static double hz1 (double word01) noexcept
    { return kAnchorHz * std::exp2 (std::clamp (word01, 0.0, 1.0) * kOctaves1); }
    [[nodiscard]] static double hz2 (double word01) noexcept
    { return kAnchorHz * std::exp2 (std::clamp (word01, 0.0, 1.0) * kOctaves2); }

    /** COUNTS PER OCTAVE, the machine's own numbers: 6553.5 and 5957.7 over the
        record's 0..65535 (the catalog prints 6553.6 and 5957.8 over 65536). */
    [[nodiscard]] static constexpr double countsPerOctave1() noexcept
    { return kWordMax / kOctaves1; }
    [[nodiscard]] static constexpr double countsPerOctave2() noexcept
    { return kWordMax / kOctaves2; }

    /** WORD COUNTS PER SEMITONE, AND THEY ARE NOT THE SAME NUMBER. `[MAN]`
        p.128 gives key follow in semitones and gives it an absolute anchor with
        a worked example: "At a setting of 50.00, the selected filter will be
        tracked by the keyboard at 1 volt-per-octave which is the same amount as
        the VCOs", stored 0..32768, so 16384 is ONE SEMITONE PER KEY and full
        scale is two. An absolute anchor for a filter's key follow, in the
        manual, in the machine's own units, with a worked example beside it.
        Nothing here is fitted. */
    [[nodiscard]] static constexpr double wordPerSemitone1() noexcept
    { return countsPerOctave1() / 12.0; }
    [[nodiscard]] static constexpr double wordPerSemitone2() noexcept
    { return countsPerOctave2() / 12.0; }

    /** Semitones into a NORMALISED word delta, per filter. `filter` is 1 or 2;
        anything else is filter 1, because a documented default beats a silent
        wrong answer. */
    [[nodiscard]] static double semitonesToWord01 (int filter, double semitones) noexcept
    {
        const double perSemi = filter == 2 ? wordPerSemitone2() : wordPerSemitone1();
        return semitones * perSemi / kWordMax;
    }

    /** `[MAN]` p.128 again: KEYTRK is stored 0..32768 and 16384 is one semitone
        per key. The caller divides its record value by this and multiplies by
        two to get semitones per key. */
    static constexpr double kKeyTrackFullScale = 32768.0;
    static constexpr double kKeyTrackSemisAtHalf = 1.0;

    // ----------------------------------------------- the coefficient ceiling -

    /** `[REF]` THE SAME CEILING `SuperFilter` ALREADY APPLIES INTERNALLY, stated
        here so the clamp is visible where the law is rather than hidden one call
        down. 0.45 of the sample rate, and both filters get it.

        IT IS ON THE COEFFICIENT AND NEVER ON THE WORD. Filter 2's printed top is
        44.696 kHz, above Nyquist at every rate we run, and that is a property of
        the machine: an analogue 4-pole whose corner is past the audio band is a
        wire with a resonant peak nobody can hear, and a filter you can put fully
        out of the way is part of why this machine's cutoff feels limitless.
        Clamping the SPAN to Nyquist and calling it the same filter would take
        that away. So every word is legal, reaches the block and is stable, and
        only the tangent that builds the pole is held. Where that starts, by rate
        (`highestUnclampedWord1/2` computes it):

          44.1 kHz  ceiling 19845 Hz  filter 1 from word 0.9830, filter 2 0.8936
          48 kHz    ceiling 21600 Hz  filter 1 from word 0.9952, filter 2 0.9047
          96 kHz    ceiling 43200 Hz  filter 1 never, filter 2 from word 0.9956
          192 kHz   ceiling 86400 Hz  NEITHER: the whole printed span is reachable

        Note what the 96 kHz row says: filter 2's 44.696 kHz top is UNDER that
        rate's Nyquist (48 kHz) and still over this ceiling, because a TPT
        one-pole a hair under Nyquist is a coefficient heading for infinity
        rather than a filter. The ceiling is the useful limit, not Nyquist. */
    static constexpr double kNyquistFrac = 0.45;

    /** The word at which each filter's coefficient starts being held, at a given
        rate. 1.0 means nothing is held anywhere on the travel. */
    [[nodiscard]] static double highestUnclampedWord1 (double sr) noexcept
    { return unclampedWord (sr, kOctaves1); }
    [[nodiscard]] static double highestUnclampedWord2 (double sr) noexcept
    { return unclampedWord (sr, kOctaves2); }

    /** `[EAR]` a FLOOR rather than a clamp at zero, and the same 8 Hz
        `SuperFilter::kFloorHz` uses for the same reason: a one-pole at 0 Hz is a
        latch, so an envelope that closes the filter all the way would freeze the
        state rather than silence it. The A6's own bottom is 21.824 Hz, well above
        it, so this only binds if a caller asks for something the machine cannot
        ask for. Retired by nothing; it is below the law's own range. */
    static constexpr double kFloorHz = 8.0;

    /** `[EAR]` the catalog's own state rail, carried over verbatim from
        `andromeda.h`: an integrator state outside +/-16 is not a signal, it is a
        number that got away, and zeroing it is what keeps a resonant SVF finite
        without putting a nonlinearity in the audio path. Retired by a hardware
        measurement of where the ASF's own integrators rail. */
    static constexpr float kStateRail = 16.0f;

    /** Filter 2's fixed arguments into `SuperFilter::set`, and each is an
        absence rather than a setting. See "WHAT THIS BLOCK IS NOT". */
    static constexpr float kF2SlopeDb = 24.0f;   ///< [MAN] p.117, four poles
    static constexpr float kF2Shape   = 0.0f;    ///< kShapeLp: ONE tap [MAN] chart p.20
    static constexpr float kF2Width   = 0.0f;    ///< no such control on the machine
    static constexpr float kF2Drive   = 0.0f;    ///< no such control on the machine
    static constexpr int   kF2Type    = kTypeSuper;  ///< the only core that self-oscillates

    // ------------------------------------------------- the control staircase -

    /** `[SCH]` sheet 40 + `[FW]` 0x002770EC: 500 Hz, one refresh every 2 ms, and
        the shadow image is 684 longs of [16 switch bits][16-bit DAC word]. The
        SWITCH BITS RIDE IN THE SAME LONGS, which is why the input selector and
        the band-pass invert latch on the same 2 ms boundary in here and do not
        interpolate: a bit cannot be half set. */
    static constexpr double kControlHz   = 500.0;
    static constexpr double kTickSeconds = 0.002;

    /** `Filter Smoothing Type`, record bytes 1907:4-7 `[MAN]` a6syx p.13. A FOUR-
        BIT FIELD, so the machine has up to sixteen positions and we have the name
        of exactly one of them: OS 1.40.12 added `ULTRA fast` and Alesis's own
        release note warns it produces "clicks and other artifacts" `[MAN]`
        a6notes.pdf.

        SO THE EXISTENCE OF THE CHOICE IS `[MAN]` AND THE POSITIONS ARE OURS.
        Position 0 is the machine's named one and is what its warning describes:
        the whole remaining distance taken in one refresh. The other three are
        `[REF]` a shift-based one-pole evaluated at the refresh, which is the
        shape these control loops have - the catalog has a measured instance on a
        neighbouring machine, `[FW]` 0x10147a, the Nord Lead 2's
        `s = s - (s >> 2) + target` at 256 Hz, which is exactly alpha = 1/4 at the
        control rate. Ours are 1, 1/2, 1/4 and 1/8 for the same reason a 90s
        fixed-point loop uses powers of two.

        WHAT WOULD RETIRE THE THREE FITTED POSITIONS: the a6syx chart's own
        enumeration of the 1907:4-7 field, which names them, or a step response
        captured off a real A6 at each setting. Until then they are four rates of
        one honest mechanism rather than four names copied off a machine.

        THE DEFAULT IS POSITION 1, AND THAT IS OURS TOO: position 0 is the
        setting the manufacturer shipped a warning with, which is a poor default
        even when it is authentic. */
    enum Smoothing : int
    {
        kUltraFast = 0,   ///< [MAN] named, and [MAN] warned about
        kFast      = 1,   ///< [REF]
        kMedium    = 2,   ///< [REF], and the Nord Lead 2's own measured alpha
        kSlow      = 3,   ///< [REF]
        kNumSmoothing = 4
    };
    static constexpr float kSmoothAlpha[kNumSmoothing] { 1.0f, 0.5f, 0.25f, 0.125f };

    /** WHICH CONTROL PATH, and only one of the two is a machine.

        `kStaircase` is the A6: a target latched and held at 500 Hz, with the
        smoothing type choosing how much of the remaining distance each refresh
        takes. It is the default and it is the thing not to optimise away.

        `kPerSample` is OURS and is what a modern plugin does - the same
        one-pole with the same time constant, evaluated every sample, so the
        staircase disappears and nothing else changes. It exists so the question
        "does the staircase matter" can be settled by listening instead of
        argued: `twin_probe --render` writes the same sweep both ways. */
    enum Path : int { kStaircase = 0, kPerSample = 1, kNumPaths = 2 };

    // --------------------------------------------------------------- setup ---

    /** Allocates nothing. Every state is a member and the only per-rate work is
        coefficient arithmetic, so this is safe to call again at any time. The
        signature matches every other block's so a rack's apply() needs no
        special case. */
    void prepare (double sampleRate, int maxBlock = 0) noexcept
    {
        (void) maxBlock;
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        tickSamples = std::max (1, (int) (kTickSeconds * sr + 0.5));
        f2.prepare (sr);
        f2.setType (kF2Type);
        reset();
    }

    /** Everything with memory back to where it started, and THE CONTROL PATH
        RE-ARMED TO SNAP: the first control write after a reset lands whole,
        because a voice that has just been allocated is not mid-glide. Same rule
        and same wording as `BankFx::reset` re-arming its smoothed gains. Two
        renders after a reset must be bit-identical; check 9 asserts it. */
    void reset() noexcept
    {
        f1.reset();
        f2.reset();
        tapLp = tapBp = tapHp = lastIn = 0.0f;
        tickCountdown = 0;
        snapPending = true;
        recut();
    }

    // ----------------------------------------------- the control surface -----

    /** FILTER 1 FREQUENCY, as the machine's own control word normalised to
        0..1. A WORD AND NOT A FREQUENCY, because the word is the domain the two
        filters share and the Hz is the thing they differ in. Hand this block a
        frequency for each filter and the octave is somebody else's problem,
        which is how it goes missing. */
    void setFrequencyWord1 (double word01) noexcept { setTarget (cWord1, word01); }

    /** FILTER 2 FREQUENCY. Its own channel in the record - `[COR]` ASIC control
        word 2 is filter 1's and word 3 is filter 2's, and both span their full
        16-bit range across the factory corpus - so the two are independently
        settable. What they share is the LAW, and the same number in the two
        channels is two different frequencies. */
    void setFrequencyWord2 (double word01) noexcept { setTarget (cWord2, word01); }

    /** BOTH CHANNELS AT ONE NUMBER, which is the arrangement that makes the
        finding audible: one knob, two filters, on the same frequency at the
        bottom and an octave apart at the top. */
    void setFrequencyWord (double word01) noexcept
    { setTarget (cWord1, word01); setTarget (cWord2, word01); }

    /** THE SUM'S OWN DOMAIN: semitones above the 21.824 Hz anchor, converted
        into that filter's word with that filter's own counts per octave. `[FW]`
        0x00247650 sums in this domain and the word is one exponential away from
        a frequency, so this is the entry point a cutoff sum should use - and it
        is the entry point that cannot make mistake 2 in the header, because the
        conversion is per filter by construction. */
    void setCutoffSemitones1 (double semitonesAboveAnchor) noexcept
    { setTarget (cWord1, semitonesAboveAnchor / (kOctaves1 * 12.0)); }
    void setCutoffSemitones2 (double semitonesAboveAnchor) noexcept
    { setTarget (cWord2, semitonesAboveAnchor / (kOctaves2 * 12.0)); }

    /** FILTER 1 RESONANCE, 0..1. The record is 0..32768 displayed 0..100.00
        `[MAN]` chart page 18; the caller divides. It is on the same 500 Hz
        staircase as everything else, `[FW]` 0x00247650 being the same twenty
        instructions for resonance as for the cutoff. */
    void setResonance1 (float r01) noexcept { setTarget (cRes1, r01); }

    /** FILTER 2 RESONANCE, 0..1, straight into `SuperFilter`'s own law.

        THE CATALOG'S `andromeda_filter2::resonanceToQ` IS DELIBERATELY NOT USED:
        it is an `[EAR]` ladder feedback amount written for the catalog's own
        four-stage cascade, and this block does not have that cascade, it has
        SuperFilter. Replacing SuperFilter's resonance law with a fitted one from
        a different ladder would be two `[EAR]` guesses stacked. The one ANCHORED
        fact - `[MAN]` p.128, full resonance oscillates - is preserved and
        measured instead. */
    void setResonance2 (float r01) noexcept { setTarget (cRes2, r01); }

    /** POST FILTER MIX. `FILT1LPLEVEL`, `FILT1BPLEVEL`, `FILT1HPLEVEL` and
        `FILT2LPLEVEL`, `[MAN]` chart page 20, each 0..1 here and 0..32768 in the
        record. THESE ARE THE MODE. Three of them are filter 1's and there is no
        fourth control that says which one you get.

        They are levels and nothing normalises them, which is megasynth's own
        non-negotiable 2 (nothing replaces anything, everything is an amount)
        agreeing with the machine for once rather than being imposed on it. They
        are on the staircase with the rest: `[FW]` 0x00247650 carries ALL TEN
        VCAs through the same twenty instructions as the cutoff. */
    void setLpLevel (float v) noexcept { setTarget (cLp, v); }
    void setBpLevel (float v) noexcept { setTarget (cBp, v); }
    void setHpLevel (float v) noexcept { setTarget (cHp, v); }
    void setFilter2Level (float v) noexcept { setTarget (cF2, v); }

    /** `F1_BPINV_POT`, `[MAN]` chart page 18: the SIGN of the band-pass TAP.

        It changes what the notch sums to as well as what reaches the VCA, which
        is why it is on the tap and not on the fader. A SWITCH BIT, so it latches
        on the 2 ms boundary and does not interpolate.

        WHERE THIS IS A CLAIM: the band-pass send into filter 2 is `[MAN]`
        PRE-FADER, and pre-fader is a statement about the LEVEL, so the inverted
        tap is what leaves here in the `bandpass` position too. Nothing in
        `[MAN]`, `[SCH]` or the chart settles whether the invert sits before or
        after that send. Flagged rather than hidden; a 400 dpi render of sheet
        40's ASF block around OS1-OS10 would close it, which is the same lead the
        catalog lists for whether the three taps are summed inside the chip. */
    void setBpInvert (bool inv) noexcept { invTarget = inv; if (snapPending) invHeld = inv; }

    /** THE FOUR-WAY SELECTOR, also a switch bit and also latched at the refresh.
        Out of range clamps rather than asserts, because a stored program from a
        future bank must not be able to crash this one - the same rule
        `SuperFilter::setType` states. */
    void setInput (int which) noexcept
    {
        inputTarget = std::clamp (which, 0, kNumInputs - 1);
        if (snapPending) inputHeld = inputTarget;
    }

    /** THE INTERPOLATION ACROSS THE 2 ms GAP, which on this machine is a program
        parameter. See `Smoothing`. */
    void setSmoothing (int type) noexcept
    { smoothing = std::clamp (type, 0, kNumSmoothing - 1); }
    [[nodiscard]] int smoothingType() const noexcept { return smoothing; }

    /** THE STAIRCASE, OR OURS. `kStaircase` is the machine. See `Path`. */
    void setControlPath (int p) noexcept { path = std::clamp (p, 0, kNumPaths - 1); }
    [[nodiscard]] int controlPath() const noexcept { return path; }

    /** THE MODULE'S BYPASS, and it is OURS: the A6 has no filter bypass, its
        filter 2 has no bypass at all, and `off` on the selector is a filter with
        no input rather than a filter out of the path. This is the house module
        bypass (`data/modules.json` gives every module one) and it is the block's
        DOCUMENTED NEUTRAL: bit-identical, sample for sample, which check 10
        asserts.

        The other neutral a caller might expect does not exist, and it is worth
        saying so: with every POST FILTER MIX level at zero this block is SILENT
        rather than transparent, because on this machine those four levels are the
        only route to the VCA. */
    void setBypass (bool b) noexcept { bypassed = b; }

    // -------------------------------------------------------------- render ---

    /** One sample in, one out. Both filters run every sample, whatever the
        selector says and whatever the levels say - which is what "two complete
        filters per voice, both running" means and is the whole reason a filter
        with its input OFF can still be singing. */
    [[nodiscard]] float process (float x) noexcept
    {
        if (bypassed) return x;

        // ---- the control path, and it is 500 Hz unless somebody asked for ours
        if (snapPending) snapControls();
        else if (path == kPerSample)
        {
            if (advance (perSampleAlpha())) recut();
        }
        else if (--tickCountdown <= 0)
        {
            tickCountdown = tickSamples;
            if (advance (kSmoothAlpha[smoothing])) recut();
        }

        // ---- FILTER 1: ONE pair of poles, three taps out of it ---------------
        f1.step (x);
        lastIn = x;
        tapLp  = f1.lp;
        tapBp  = invHeld ? -f1.bp : f1.bp;
        tapHp  = f1.hp;

        // ---- the selector, and the pre/post-fader asymmetry ------------------
        const float lpL = (float) held[cLp], bpL = (float) held[cBp];
        const float hpL = (float) held[cHp], f2L = (float) held[cF2];
        float in2 = 0.0f;
        switch (inputHeld)
        {
            case kInputMix:      in2 = x; break;
            case kInputNotch:    in2 = tapLp * lpL + tapHp * hpL; break;   // POST-fader
            case kInputBandpass: in2 = tapBp; break;                       // PRE-fader
            default:             break;   // kInputOff: nothing arrives, and it still rings
        }

        // ---- FILTER 2: SuperFilter, called ----------------------------------
        const float out2 = f2.process (in2);

        // ---- POST FILTER MIX. THE MODE IS THIS SUM. --------------------------
        // In NOTCH the LP and HP faders ARE filter 2's input mix and stop feeding
        // the VCA, [MAN] p.128. The BP fader feeds the VCA in every position,
        // because its send is taken pre-fader.
        float y = tapBp * bpL + out2 * f2L;
        if (inputHeld != kInputNotch)
            y += tapLp * lpL + tapHp * hpL;
        return y;
    }

    /** A buffer of one channel, in place. */
    void process (float* buf, int n) noexcept
    {
        if (buf == nullptr) return;
        for (int i = 0; i < n; ++i) buf[i] = process (buf[i]);
    }

    // ----------------------------------------------------------- the gates ---
    /** What the last `process` put on the three taps, and what went in. NOTHING
        IN THE AUDIO PATH READS THESE - they exist for the check that the three
        taps are one pair of poles, the same way `SuperFilter::fourPoleWeights`
        exists for the check that its weights reduce to the Synthex's modes.

        The state-variable identity is `in = hp + damp * bp + lp`, exactly, at
        every sample and every coefficient, and it holds ONLY because the three
        taps come out of one solve over two shared integrators. Three textbook
        filters with their own Q each reproduce the three shapes and break this
        immediately, which is the catalog's warning turned into a number. */
    [[nodiscard]] float lastInput() const noexcept { return lastIn; }
    [[nodiscard]] float lastLp() const noexcept { return tapLp; }
    /** The tap AFTER `F1_BPINV_POT`, which is what the mix and the send see. */
    [[nodiscard]] float lastBp() const noexcept { return tapBp; }
    [[nodiscard]] float lastHp() const noexcept { return tapHp; }
    /** The damping term the identity needs, `1/Q`. */
    [[nodiscard]] float damping1() const noexcept { return f1.damp; }
    /** The two corners the last refresh resolved to, in Hz, AFTER the ceiling.
        For the check that measures the octave, and for a panel that wants to
        print a frequency beside a word. */
    [[nodiscard]] double corner1Hz() const noexcept { return fc1; }
    [[nodiscard]] double corner2Hz() const noexcept { return fc2; }
    /** The word each filter is actually holding, which lags the target by the
        smoothing while the staircase is walking. */
    [[nodiscard]] double heldWord1() const noexcept { return held[cWord1]; }
    [[nodiscard]] double heldWord2() const noexcept { return held[cWord2]; }
    [[nodiscard]] int    inputSelector() const noexcept { return inputHeld; }
    /** How many samples there are between control refreshes at this rate. */
    [[nodiscard]] int    controlTickSamples() const noexcept { return tickSamples; }

    /** `[EAR]` FILTER 1's RESONANCE LAW, the catalog's own, verbatim.

        It CANNOT BE CLOSED FROM PAPER: `[MAN]` gives 0..100.00 and the record
        0..32768, and the resonance cell is inside a custom Alesis part
        (`2-27-0010`) with no datasheet in existence. What is anchored is that
        FILTER 2 oscillates at full resonance; the manual says nothing of the kind
        about filter 1, and this block does not assume it from the neighbouring
        section on the same die - Q tops out at 18.6, which rings hard and does
        not sing.

        WHAT WOULD RETIRE IT: a measurement on a real A6, or the
        self-oscillation threshold read off the calibration bus (`OS10
        FILT_ASIC_VCA_CAL_BUS`, `[SCH]` sheet 40) which the autotune at `[FW]`
        0x00245FCA already drives. Failing both, it is a law somebody has to
        CHOOSE, which is what `twin_probe --render` writes the resonance pair
        for. It is the catalog entry's largest single gap and it is on its debt
        ledger under its own name. */
    [[nodiscard]] static double resonance1ToQ (double r01) noexcept
    { return 0.6 + std::clamp (r01, 0.0, 1.0) * 18.0; }

private:
    /** THE 2-POLE STATE VARIABLE, ported from
        `~/projects/synthcatalog/filters/alesis/andromeda.h` field for field.

        The solve computes HP and reads BP and LP off the same two integrator
        states, which is why the identity above is exact and why the slopes come
        out right: one pair of poles, three taps. `dspblocks`' own SVF and
        `BankFx::step` both compute the high-pass FROM the identity
        (`hp = x - damp*bp - lp`), which is the same filter and makes the identity
        a tautology; this form solves for it, so the check measures something.
        That is the only reason the catalog's arithmetic is kept rather than the
        package's. */
    struct Svf
    {
        void reset() noexcept { s1 = s2 = 0.0f; lp = bp = hp = 0.0f; }

        void step (float in) noexcept
        {
            const float d = 1.0f / (1.0f + damp * g + g * g);
            hp = (in - (damp + g) * s1 - s2) * d;
            bp = g * hp + s1;  s1 = g * hp + bp;
            lp = g * bp + s2;  s2 = g * bp + lp;
            // The rail, [EAR] and the catalog's: a state outside +/-16 is a
            // number that got away rather than a signal.
            if (! (s1 > -kStateRail && s1 < kStateRail)) s1 = 0.0f;
            if (! (s2 > -kStateRail && s2 < kStateRail)) s2 = 0.0f;
        }

        float g = 0.1f, damp = 1.0f;
        float s1 = 0.0f, s2 = 0.0f;
        float lp = 0.0f, bp = 0.0f, hp = 0.0f;
    };

    /** The eight controls that ride the 500 Hz analogue bus. The two switch bits
        are separate because they latch rather than interpolate. */
    enum Ctl : int { cWord1 = 0, cWord2, cRes1, cRes2, cLp, cBp, cHp, cF2, cNumCtl };

    [[nodiscard]] static double unclampedWord (double sr, double octaves) noexcept
    {
        const double ceilHz = sr * kNyquistFrac;
        if (ceilHz <= kAnchorHz) return 0.0;
        return std::min (1.0, std::log2 (ceilHz / kAnchorHz) / octaves);
    }

    void setTarget (int c, double v) noexcept
    {
        tgt[c] = std::clamp (v, 0.0, 1.0);
        // RE-ARMED TO SNAP: before the first sample after a reset, a control
        // write lands whole, so `corner1Hz()` tells the truth before any audio
        // has run and a freshly allocated voice does not glide in from wherever
        // the last one left the bus.
        if (snapPending) { held[c] = tgt[c]; recut(); }
    }

    void snapControls() noexcept
    {
        for (int c = 0; c < cNumCtl; ++c) held[c] = tgt[c];
        inputHeld = inputTarget;
        invHeld   = invTarget;
        snapPending = false;
        tickCountdown = tickSamples;
        recut();
    }

    /** ONE REFRESH. Returns true when something moved, so the coefficients are
        rebuilt only when there is a reason to - which is also what keeps two
        renders bit-identical across a reset. */
    bool advance (float alpha) noexcept
    {
        bool moved = false;
        for (int c = 0; c < cNumCtl; ++c)
        {
            const double d = tgt[c] - held[c];
            if (d == 0.0) continue;
            held[c] += (double) alpha * d;
            // Land exactly rather than approaching forever: a control that never
            // arrives is a coefficient that is rebuilt every refresh for the life
            // of the note and a value that is never quite what the patch says.
            if (std::fabs (tgt[c] - held[c]) < 1.0e-9) held[c] = tgt[c];
            moved = true;
        }
        // The switch bits, [FW] riding in the same longs as the DAC words: they
        // change on the refresh boundary and cannot be half set.
        if (inputHeld != inputTarget) { inputHeld = inputTarget; moved = true; }
        if (invHeld   != invTarget)   { invHeld   = invTarget;   moved = true; }
        return moved;
    }

    /** The per-sample path's own coefficient: the SAME time constant the
        staircase has, so the A/B differs by the staircase and by nothing else.
        alpha = 1 stays alpha = 1, which is "no smoothing at all" on both paths
        and is the cleanest comparison of the two. */
    [[nodiscard]] float perSampleAlpha() const noexcept
    {
        const float a = kSmoothAlpha[smoothing];
        if (a >= 1.0f) return 1.0f;
        return 1.0f - std::pow (1.0f - a, 1.0f / (float) tickSamples);
    }

    /** ONE CONVERSION PER FILTER, at the end, which is the machine's own shape:
        the 16-bit word IS an exponential index and everything upstream of here
        sums in that domain. */
    void recut() noexcept
    {
        const double ceilHz = sr * kNyquistFrac;

        // THE CLAMP IS ON THE COEFFICIENT AND NOT ON THE WORD. See kNyquistFrac.
        fc1 = std::clamp (hz1 (held[cWord1]), kFloorHz, ceilHz);
        f1.g    = (float) std::tan (3.14159265358979 * fc1 / sr);
        // The catalog's own `max (0.5, Q)` is kept although the law's floor is
        // 0.6 and it can never bind: it is the catalog's line, and a guard that
        // never fires is cheaper than a divergence between the two files.
        f1.damp = (float) (1.0 / std::max (0.5, resonance1ToQ (held[cRes1])));

        fc2 = std::clamp (hz2 (held[cWord2]), kFloorHz, ceilHz);
        // SuperFilter clamps to its own 0.45 of the rate internally as well. The
        // clamp above is not redundant - it is what makes `corner2Hz()` tell the
        // truth, and the ceiling is stated where the law is.
        f2.set ((float) fc2, (float) held[cRes2], kF2Shape, kF2Width, kF2Drive,
                kF2SlopeDb, kF2Type);
    }

    double sr = 48000.0;
    int    tickSamples = 96;        ///< 2 ms at 48 kHz
    int    tickCountdown = 0;
    bool   snapPending = true;

    // the eight bus controls: target and held. Words are normalised 0..1.
    double tgt  [cNumCtl] { 0.5, 0.5, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0 };
    double held [cNumCtl] { 0.5, 0.5, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0 };

    double fc1 = 698.0, fc2 = 987.0;

    int  inputTarget = kInputMix, inputHeld = kInputMix;
    bool invTarget = false, invHeld = false;
    int  smoothing = kFast;
    int  path = kStaircase;
    bool bypassed = false;

    Svf         f1;
    SuperFilter f2;

    float tapLp = 0.0f, tapBp = 0.0f, tapHp = 0.0f, lastIn = 0.0f;
};

/** THE ANDROMEDA PAIR, DRIVEN FROM THIS MACHINE'S OWN FIVE FILTER CONTROLS.

    Wired 2026-09-17 at the author's ask ("lets add that new twinfilter to the filters
    section"), after he remembered the machine itself: "the cutoff on the
    andromeda was so amazingly big... I remember the filter felt very big and
    warm." The block was built on 2026-09-15 from the catalog and had no
    parameters until now; this function is the whole of the wiring, kept here
    rather than in SuperFilter because a TwinFilter CONTAINS a SuperFilter and
    the two cannot nest the other way round.

    Every mapping is a claim about what the A6's control IS, and each one is
    written down so it can be argued with:

      CUTOFF     both filters, in semitones above the pair's own anchor
                 (21.824 Hz, [MAN] p.128 printed). One knob moves the pair.
      WIDTH      the SPLIT between them - half up, half down, so the corner does
                 not move. The A6 gives its two filters separate frequency
                 knobs, and the sound people remember is what happens when they
                 are NOT together, so this is the control that matters most.
      SHAPE      filter 1's three output FADERS, which is the morph itself: LP
                 at 0, BP at 1, HP at 2, and the notch at 3 as LP + HP with the
                 band inverted, which is how the machine makes one. This is the
                 one topology whose shape control is the hardware's own rather
                 than an approximation of it.
      RESONANCE  both filters', for the same reason cutoff moves both.
      SLOPE      how much of FILTER 2 reaches the output. Filter 2 is the second
                 pole pair, so more of it IS steeper. The only control here with
                 no direct A6 equivalent, and it is marked as ours.
      DRIVE      the input gain, applied around the block, so DRIVE means the
                 same thing on all four topologies.

    THE INPUT SELECTOR IS FIXED AT THE PRE FILTER MIX (parallel), which is what
    the machine ships in and where the remembered sound comes from. Its other
    three positions - off, the notch sum, the bandpass tap - are a control this
    panel does not have; that is in todo.md rather than guessed at here. */
inline void configureTwin (TwinFilter& twin, double sr,
                           float cutoffHz, float resonance01, float shape03,
                           float width01, float drive01, float slopeDb,
                           float& driveGain, float& driveInv) noexcept
{
    const float nyq = (float) (sr * 0.45);
    const float halfSemis = std::clamp (width01, 0.0f, 1.0f) * 24.0f * 0.5f;
    const float spread = std::exp2 (halfSemis / 12.0f);
    const float fHi = std::clamp (cutoffHz * spread, 20.0f, nyq);
    const float fLo = std::clamp (cutoffHz / spread, 20.0f, nyq);
    const auto semis = [] (float hz)
    { return 12.0 * std::log2 ((double) hz / TwinFilter::kAnchorHz); };
    twin.setCutoffSemitones1 (semis (fLo));
    twin.setCutoffSemitones2 (semis (fHi));
    const float r = std::clamp (resonance01, 0.0f, 1.0f);
    twin.setResonance1 (r);
    twin.setResonance2 (r);
    // THE MORPH AS THREE FADERS. Between two named points BOTH are up, which is
    // what a fader mix does and what a crossfade between taps cannot.
    const float sh = std::clamp (shape03, 0.0f, 3.0f);
    const int i = std::min ((int) sh, 2);
    const float f = sh - (float) i;
    float lp = 0.0f, bp = 0.0f, hp = 0.0f;
    bool invert = false;
    if (i == 0)      { lp = 1.0f - f; bp = f; }
    else if (i == 1) { bp = 1.0f - f; hp = f; }
    else             { hp = 1.0f - f * 0.5f; lp = f * 0.5f; bp = f; invert = true; }
    twin.setLpLevel (lp); twin.setBpLevel (bp); twin.setHpLevel (hp);
    twin.setBpInvert (invert);
    twin.setFilter2Level (std::clamp ((slopeDb - 12.0f) / 12.0f, 0.0f, 1.0f));
    twin.setInput (TwinFilter::kInputMix);
    driveGain = 1.0f + 7.0f * std::clamp (drive01, 0.0f, 1.0f);
    driveInv  = 1.0f / driveGain;
}

} } // namespace dsp::filter
