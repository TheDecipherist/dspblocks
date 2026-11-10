// Do a machine's factory programs actually sound different from each other?
//
// This is the check that found the two worst bugs of a week's work, and neither
// was audible as "a bug" or visible to a green test suite:
//
//   * the Eventide H3500 - 625 presets measuring as 146 effective distinct
//     sounds. Three causes, all upstream of the DSP: an invented global `mix`
//     the hardware does not have, defaulting to 0.5 so every preset played half
//     wet (fixing it moved the median preset-to-preset difference from 0.014 to
//     0.164, ELEVEN TIMES); 272 presets silently loading algorithm defaults
//     instead of their stored values; and one algorithm with min == max == 0 on
//     every parameter, rendering 66 presets silent.
//   * the Yamaha PSR-730 - 707 voices measuring as 38 effective distinct sounds,
//     now 534. The wave id never reached the engine, so 707 voices resolved to
//     83 coefficient sets.
//
// Both presented as "the presets all sound the same". That sentence is a real
// report and a testable one, so it belongs in code rather than in an opinion.
//
// WHY THIS IS SHARED. The number is only worth having if 0.164 means the same
// thing on every machine, and it stops meaning the same thing the moment two
// units each grow their own probe with their own excitations, their own window
// and their own idea of "identical". A unit supplies its bank, its records and
// one render function; everything from the note patterns to the report format
// is here, once.
//
// WHAT A UNIT HAS TO PROVIDE  (see `Unit` below):
//   - the stored records, as bytes, and their names
//   - which byte offsets are NAME rather than parameter
//   - render(record, source, seconds) -> mono samples
//   - optionally, the engine's own copy of a record after it has loaded it
//
// WHAT THIS PRINTS, in order:
//   0. DETERMINISM - two renders of one program, unchanged, must be
//      bit-identical. FIRST, because a number out of a non-deterministic engine
//      is worse than no number: it carries the same authority. jupiter8's
//      prepareToPlay left the oscillator phases, the LFO phase, the delay ramp
//      and the noise seed wherever the previous run stopped, and the shared
//      reverb tank's RNG seed had the same problem. Neither announced itself.
//      (Playbook 14b.)
//   1. COVERAGE - every byte of every stored program against the engine's own
//      copy after it loads it. Assert the OPPOSITE of what you believe: this is
//      the assertion that found 272 silently-defaulted H3500 presets and 383
//      clamped values on the PCM 91.
//   2. REACH - the strong form of the same question. Coverage only proves the
//      loader copied the record; REACH proves the DSP reads it. Every byte that
//      VARIES across the bank is set to the bank's own low, middle and high
//      value on several base programs at two pitches, and the render must move.
//      A byte that changes nothing is a byte that never reached the engine -
//      the PSR-730's wave id, exactly.
//   3. SETTINGS - how many distinct records the bank actually holds. The ceiling
//      on the spread is set by the DATA; a bank that resolves to thirteen
//      settings cannot be fixed in the DSP, and the two need different fixes.
//   4. SPREAD - median / min / mean / max pairwise difference per excitation.
//   5. TWINS - programs that are another program on EVERY excitation, and the
//      EFFECTIVE DISTINCT SOUND COUNT out of the bank size. That count is the
//      headline.
//
// TWO CALIBRATIONS, BOTH PAID FOR ON THE PSR-730, are built into the note
// patterns. A figure must FILL the render: a note that stops early leaves a
// nine-second window mostly silent, and comparing silence to silence says
// nothing - shortening one machine's release, correctly, moved 25,907 pairs
// into "identical" overnight. But filling it with a busy loop is the wrong
// instinct too, because dsp/Measure.h frames are log-spaced in TIME, so a
// repeating line makes every frame an average of the same notes: a 14-note
// figure dropped one bank's LEAD median from 0.24 to 0.026 with 43 % of all
// pairs reading as identical. One long held note, a sparse line and one held
// chord are what discriminate.
//
// NEVER MEASURE AT MIDDLE C ALONE (playbook 14a). Filter key follow, sample
// zones, per-zone trims and envelope key scaling are all exactly zero at C4,
// which is where everybody auditions. The struck note is taken at C2, C4 and C6.
//
// AND SAY WHAT THE METRIC CANNOT SEE. dsp::measure::difference is
// 0.45 cosine + 0.15 decay + 0.2 echo structure + 0.2 comb spacing. On an
// INSTRUMENT every voice playing the same note has the same pitch period, so the
// comb term is zero for every pair, and none of the renders have discrete
// echoes: two of the four terms are inert, and a synth's numbers are compressed
// against an effects unit's on the same scale. Quote them that way; do not "fix"
// the shared metric to flatter one machine.
#pragma once
#include "Measure.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dsp { namespace spread {

/** One note in an excitation pattern: (start beat, length in beats, key, velocity). */
struct Note { double at, len; int key, vel; };

/** The excitations, named from `shell::AuditionSource`'s own musical modes so the
    vocabulary is the shared one. An instrument is not an effect: AuditionSource
    generates AUDIO to feed a box, and a synth makes its own, so what carries
    across is the four musical SHAPES as note patterns. */
enum class Source { NoteLow = 0, NoteMid, NoteHigh, Drums, Lead, Pad, Strings,
                    // Not measured. A SHORT note at two pitches, used only by the
                    // reach section: the seven measured sources all hold their
                    // notes for the whole window, so a RELEASE TIME can never
                    // move them and every release byte in a record reads as
                    // deaf. Found on the Nord Lead 2, where offsets 11 and 15
                    // (filter and amp envelope release) sat in the deaf list for
                    // exactly that reason. The blip's note is over inside half a
                    // second, so attack, decay, sustain and release are all
                    // inside a 2.5 s render.
                    BlipLow, BlipHigh };

/** The sources that are MEASURED. The blips are excitations, not measurements. */
inline constexpr int kNumSources = 7;

inline const char* sourceName (Source s)
{
    switch (s)
    {
        case Source::NoteLow:  return "NOTE-C2";
        case Source::NoteMid:  return "NOTE-C4";
        case Source::NoteHigh: return "NOTE-C6";
        case Source::Drums:    return "DRUMS";
        case Source::Lead:     return "LEAD";
        case Source::Pad:      return "PAD";
        default:               return "STRINGS";
    }
}

/** Playbook 14a: the error lives in `(note - 60)`, which is exactly zero where
    everybody auditions. Two octaves either side of C4 is enough to expose key
    follow, key-scaled envelope times and a sample zone boundary. */
inline int sourceTranspose (Source s)
{
    switch (s)
    {
        case Source::NoteLow:  return -24;
        case Source::NoteHigh: return +24;
        case Source::BlipLow:  return -24;
        case Source::BlipHigh: return +24;
        default:               return 0;
    }
}

inline std::vector<Note> pattern (Source s)
{
    switch (s)
    {
        case Source::NoteLow:
        case Source::NoteMid:
        case Source::NoteHigh:
            // ONE STRUCK NOTE, held long enough to show attack, body and release.
            return { { 0.0, 14.0, 60, 100 } };
        case Source::BlipLow:
        case Source::BlipHigh:
            // Reach only: a note that ENDS, so a release time can move the audio.
            return { { 0.0, 0.8, 60, 100 } };
        case Source::Drums:
        {
            // Transients and gaps: what shows an envelope's attack and a gate.
            std::vector<Note> v;
            for (int bar = 0; bar < 4; ++bar)
            {
                const double b = 4.0 * bar;
                v.push_back ({ b + 0.0,  0.30, 36, 120 });
                v.push_back ({ b + 1.0,  0.30, 48, 110 });
                v.push_back ({ b + 1.75, 0.20, 55,  85 });
                v.push_back ({ b + 2.0,  0.30, 36, 120 });
                v.push_back ({ b + 3.0,  0.30, 48, 110 });
                v.push_back ({ b + 3.5,  0.25, 60,  95 });
            }
            return v;
        }
        case Source::Lead:
            // SPARSE on purpose. See the header note on log-spaced frames.
            return { { 0.0, 3.0, 64, 105 }, { 3.0, 2.0, 67, 105 }, { 5.0, 4.0, 71, 110 },
                     { 9.0, 2.0, 69, 100 }, { 11.0, 5.0, 62, 108 } };
        case Source::Pad:
            // ONE HELD CHORD, filling the window.
            return { { 0.0, 15.0, 48, 90 }, { 0.0, 15.0, 55, 88 },
                     { 0.0, 15.0, 60, 86 }, { 0.0, 15.0, 64, 84 } };
        default:
            // A slow swell of moving voices.
            return { { 0.0, 8.0, 52, 80 }, { 1.0, 8.0, 59, 85 }, { 2.0, 8.0, 64, 90 },
                     { 9.0, 7.0, 55, 95 }, { 9.0, 7.0, 62, 95 }, { 10.0, 6.0, 67, 95 } };
    }
}

/** A program as this probe needs to see it: a flat record of bytes and a name.
    A machine whose engine settings are a struct rather than a byte record
    serialises the struct into `bytes`; the point is only that two programs can
    be compared field by field and one field can be perturbed. */
struct Program
{
    std::string name;
    std::vector<unsigned char> bytes;
};

/** FLOORS, NOT TARGETS. Set each one from the first honest measurement of the
    real bank and leave it well under what the bank measures, so it catches a
    REGRESSION - a law that flattens, a loader that stops loading, an engine that
    stops responding - rather than the normal wobble of a metric. RAISE them when
    the bank improves: a floor that never moves is a floor nobody is using.

    And a floor that cannot fail is not a floor. `--selftest` collapses every
    program onto one record and requires the probe to notice; wire it in as its
    own ctest so the gate is proved live rather than trusted (playbook 14c). */
struct Floors
{
    double noteMedian    = 0.10;   ///< median of the three single-note medians
    double musicalMedian = 0.10;   ///< median of DRUMS / LEAD / PAD / STRINGS
    int    soundsPct     = 90;     ///< effective distinct sounds, % of the bank
    /** Distinct RECORDS as a % of the bank. 100 is right for a single factory
        bank, where two programs holding the same bytes is a loader fault. Lower
        it only for a COLLECTION - a factory bank plus sound cards, say - where
        the same patch genuinely appears twice, and say in the unit's docs which
        it is. It is not a knob for making a red gate green. */
    int    distinctPct   = 100;
    int    maxSilentPct  = 0;      ///< programs silent on EVERY source, % of bank
};

/** What a unit supplies. Everything else is here. */
struct Unit
{
    const char* title = "";                    ///< "D-50", "Access Virus C"
    std::vector<Program> programs;

    /** Render one record. `seconds` is at least dsp::measure::kMinSeconds. The
        engine must be constructed fresh, or reset completely: section 0 asserts
        that two calls with the same arguments return identical buffers. */
    std::function<std::vector<float> (const std::vector<unsigned char>&, Source, double)> render;

    /** True for a byte that is part of a NAME rather than a parameter. Name bytes
        are excluded from the record comparison, because two programs that differ
        only in their name are one sound wearing two labels. */
    std::function<bool (int)> isNameByte = [] (int) { return false; };

    /** The engine's own copy of a record after it has loaded it, for section 1.
        Return an empty vector to skip - but skipping is a decision to make
        explicitly, because this is the assertion that found 272 silently
        defaulted presets. A unit whose loader is a straight `memcpy` should say
        so in `coverageNote` rather than leaving the reader to assume it. */
    std::function<std::vector<unsigned char> (const std::vector<unsigned char>&)> engineCopy;

    /** One line printed under section 1 saying what that section can and cannot
        see on THIS machine. A gate that lists its own blind spots stops a green
        line being read as more than it is (playbook 14c). */
    const char* coverageNote = "";

    /** True for a program whose SOURCE SOUND is blank, so rendering silence is
        fidelity rather than the loader dropping it.

        A percentage ceiling cannot tell those two apart, and they are opposite
        faults: the H3500's 66 silent presets were an engine loading defaults,
        while the Fairlight's `Groucho:JSGTR` is 16,384 bytes of 0x80 on the
        disk - a blank slot Jeff never recorded into. Tolerating the second with
        a percentage silently widens the ceiling for the first, and on a SAMPLED
        bank the ceiling scales with the sweep: 1 of 55 is 2 %, 2 of 2,198 is
        0.09 %, and the same two voices are behind both.

        So a unit that can PROVE a program is blank at source says which ones,
        and the ceiling then applies only to silence nobody explained. Derive it
        from the data every time - never from a name list, which goes stale the
        moment the library is rescanned. Default: nothing is expected silent, so
        every existing unit keeps the behaviour it had. */
    std::function<bool (const std::vector<unsigned char>&)> expectedSilent;

    Floors floors {};
    double seconds = 16.0;                     ///< render length; >= kMinSeconds
    int    reachBases = 4;                     ///< base programs for section 2
    bool   runReach = true;
};

namespace detail {

inline bool identical (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size()
        && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
}

} // namespace detail

/** Run the whole probe. Returns the number of findings; 0 means the bank still
    spreads. `collapse` is the deliberate injection for `--selftest`. */
inline int run (const Unit& u, int step, bool collapse)
{
    const auto& all = u.programs;
    std::vector<int> idx;
    for (size_t i = 0; i < all.size(); i += (size_t) (step < 1 ? 1 : step)) idx.push_back ((int) i);
    const int n = (int) idx.size();
    if (n < 2) { std::printf ("SKIP  fewer than two programs to compare\n"); return 0; }

    const int recBytes = (int) all[0].bytes.size();
    int bad = 0;

    // THE INJECTION: every program resolving to the same record. That is what a
    // voice model reading only the category field does, and what a trust flag
    // substituting defaults does. The bank keeps all of its names.
    const auto rec = [&] (int i) -> const std::vector<unsigned char>&
    { return collapse ? all[0].bytes : all[(size_t) i].bytes; };

    std::printf ("%s program spread - %d of %d factory programs, %.0f s each,\n"
                 "metric dsp::measure (0 = identical, 1 = nothing in common)\n\n",
                 u.title, n, (int) all.size(), u.seconds);

    // ---- 0. DETERMINISM ----------------------------------------------------
    {
        const int stride = std::max (1, n / 8);
        int sampled = 0, differed = 0;
        for (int k = 0; k < n; k += stride)
        {
            ++sampled;
            if (! detail::identical (u.render (rec (idx[(size_t) k]), Source::NoteMid, 2.0),
                                     u.render (rec (idx[(size_t) k]), Source::NoteMid, 2.0)))
                ++differed;
        }
        std::printf ("0. DETERMINISM - the same program rendered twice, unchanged\n");
        std::printf ("   programs sampled                    %d\n", sampled);
        std::printf ("   renders that DIFFERED               %d\n\n", differed);
        if (differed > 0)
        {
            std::printf ("FAIL  the engine is not deterministic; every number below would be\n"
                         "      worthless, so nothing below is measured. Reset ALL running state\n"
                         "      in prepare(): every phase, every ramp, every envelope, every RNG\n"
                         "      seed - not just the buffers (playbook 14b).\n\n");
            return 1;
        }
    }

    // ---- 1. COVERAGE -------------------------------------------------------
    if (u.engineCopy)
    {
        int mismatched = 0; long bytesOff = 0;
        for (int k = 0; k < n; ++k)
        {
            const auto& want = all[(size_t) idx[(size_t) k]].bytes;
            const auto got = u.engineCopy (rec (idx[(size_t) k]));
            int d = 0;
            for (size_t i = 0; i < want.size(); ++i)
                if (i >= got.size() || got[i] != want[i]) ++d;
            if (d) { ++mismatched; bytesOff += d; }
        }
        std::printf ("1. COVERAGE - every field of every stored program against the engine's copy\n");
        std::printf ("   programs                            %d\n", n);
        std::printf ("   programs whose stored fields DIFFER %d\n", mismatched);
        std::printf ("   stored values not reaching engine   %ld of %d\n",
                     bytesOff, n * recBytes);
        if (u.coverageNote[0] != '\0') std::printf ("   %s\n", u.coverageNote);
        std::printf ("\n");
        if (mismatched > 0)
        { std::printf ("FAIL  %d programs' stored fields never reach the engine\n\n", mismatched);
          ++bad; }
    }
    else
    {
        std::printf ("1. COVERAGE - NOT RUN: this unit supplies no engineCopy(). Section 2 is the\n"
                     "   only evidence here that a stored value arrives.\n\n");
    }

    // ---- 2. REACH ----------------------------------------------------------
    // REACH is a DIAGNOSTIC, not a regression gate: it perturbs every varying byte
    // of the record and is by far the most expensive section on a machine with a
    // long record. It runs on the full sweep and is skipped when the bank is
    // being sampled, which is what ctest does - so the gate stays a tripwire
    // rather than a coffee break and the diagnostic stays thorough.
    if (u.runReach && ! collapse && step <= 1)
    {
        // Two pitches AND a note that ends: a held note cannot show a release
        // time, so a probe that only holds reports every release byte as deaf.
        const Source es[2] = { Source::BlipLow, Source::BlipHigh };
        std::vector<int> bases;
        const int nb = std::max (1, u.reachBases);
        for (int k = 1; k <= nb; ++k) bases.push_back ((int) ((size_t) k * all.size() / (size_t) (nb + 1)));
        std::vector<std::vector<float>> refs;
        for (int b : bases)
            for (int xi = 0; xi < 2; ++xi)
                refs.push_back (u.render (all[(size_t) b].bytes, es[xi], 2.5));

        int varying = 0, deaf = 0;
        std::vector<int> deafOffsets;
        for (int off = 0; off < recBytes; ++off)
        {
            if (u.isNameByte (off)) continue;
            std::set<int> seen;
            for (const auto& p : all)
                if (off < (int) p.bytes.size()) seen.insert ((int) p.bytes[(size_t) off]);
            if (seen.size() < 2) continue;      // constant across the bank: nothing to test
            ++varying;
            std::vector<int> cand { *seen.begin(), *seen.rbegin() };
            { auto it = seen.begin(); std::advance (it, seen.size() / 2); cand.push_back (*it); }
            bool moved = false;
            for (size_t bi = 0; bi < bases.size() && ! moved; ++bi)
                for (int v : cand)
                {
                    auto mod = all[(size_t) bases[bi]].bytes;
                    if ((int) mod[(size_t) off] == v) continue;
                    mod[(size_t) off] = (unsigned char) v;
                    for (int xi = 0; xi < 2; ++xi)
                        if (! detail::identical (u.render (mod, es[xi], 2.5), refs[bi * 2 + (size_t) xi]))
                        { moved = true; break; }
                    if (moved) break;
                }
            if (! moved) { ++deaf; deafOffsets.push_back (off); }
        }
        std::printf ("2. REACH - a byte that varies across the bank, perturbed: does audio move?\n");
        std::printf ("   base programs x pitches             %d x 2\n", (int) bases.size());
        std::printf ("   bytes varying across the bank       %d of %d\n", varying, recBytes);
        std::printf ("   bytes the engine is DEAF to         %d\n", deaf);
        if (deaf)
        {
            std::printf ("   offsets:");
            for (int o : deafOffsets) std::printf (" %d", o);
            std::printf ("\n");
        }
        std::printf ("   (a deaf byte is a stored value that never reaches the DSP. It is a lead\n"
                     "    to follow, not a verdict: a byte only read in a mode none of the base\n"
                     "    programs is in reads as deaf here too.)\n\n");
    }

    else if (u.runReach && ! collapse)
        std::printf ("2. REACH - not run: it perturbs every varying byte of every record and is\n"
                     "   the expensive section, so it runs on the full sweep only. Run this probe\n"
                     "   with no --step for it.\n\n");

    // ---- 3. DISTINCT SETTINGS ---------------------------------------------
    int distinct = 0;
    {
        std::set<std::vector<unsigned char>> settings;
        for (int k = 0; k < n; ++k)
        {
            const auto& b = rec (idx[(size_t) k]);
            std::vector<unsigned char> key;
            key.reserve (b.size());
            for (size_t i = 0; i < b.size(); ++i) if (! u.isNameByte ((int) i)) key.push_back (b[i]);
            settings.insert (key);
        }
        distinct = (int) settings.size();
        std::printf ("3. DISTINCT ENGINE SETTINGS (the record, name bytes excluded)\n");
        std::printf ("   programs                            %d\n", n);
        std::printf ("   distinct records                    %d\n", distinct);
        std::printf ("   programs sharing one with another   %d\n", n - distinct);
        std::printf ("   -> no rendering can separate these; the ceiling on the spread is set by\n"
                     "      the DATA reaching the engine, not by the DSP.\n\n");
        // Same arithmetic problem the silent ceiling has: one duplicate out of 32
        // sampled is 3 %, and out of 1 024 it is 0.1 %, and it is the same pair.
        // A unit that has declared any tolerance at all tolerates at least one.
        const int sharedAllowed = u.floors.distinctPct < 100
                                ? std::max (1, (100 - u.floors.distinctPct) * n / 100)
                                : 0;
        if (n - distinct > sharedAllowed)
        { std::printf ("FAIL  %d programs resolve to another program's record; the ceiling is %d\n\n",
                       n - distinct, sharedAllowed); ++bad; }
        else if (distinct < n)
            std::printf ("note  %d programs share a record with another. That is expected in a\n"
                         "      COLLECTION (a factory bank plus sound cards) and a fault in a single\n"
                         "      bank; this unit has declared which it is.\n\n", n - distinct);
    }

    // ---- 4. RENDER AND MEASURE --------------------------------------------
    std::vector<std::vector<dsp::measure::Features>> feats ((size_t) kNumSources);
    for (auto& f : feats) f.resize ((size_t) n);
    for (int x = 0; x < kNumSources; ++x)
        for (int k = 0; k < n; ++k)
            feats[(size_t) x][(size_t) k] =
                dsp::measure::analyse (u.render (rec (idx[(size_t) k]), (Source) x, u.seconds), 48000.0);

    std::printf ("4. SPREAD ACROSS THE BANK\n");
    std::printf ("   %-9s %8s %8s %8s %8s %12s %8s\n",
                 "source", "median", "min", "mean", "max", "identical", "silent");
    for (int x = 0; x < kNumSources; ++x)
    {
        const auto r = dsp::measure::compare (feats[(size_t) x]);
        std::printf ("   %-9s %8.4f %8.4f %8.4f %8.4f %6ld/%-5ld %8d\n",
                     sourceName ((Source) x), r.median, r.worst, r.mean, r.best,
                     r.identical, r.pairs, r.silent);
    }
    std::printf ("   (`identical` counts pairs under %.2f, the same-sound threshold in\n"
                 "    dsp/Measure.h. TWO OF THAT METRIC'S FOUR TERMS ARE INERT ON AN INSTRUMENT:\n"
                 "    every voice playing the same note has the same pitch period, so the comb\n"
                 "    term is zero for every pair, and none of these renders have discrete\n"
                 "    echoes. A synth's numbers are compressed against an effects unit's on the\n"
                 "    same scale - say so when quoting them.)\n\n",
                 dsp::measure::Report::kIdentical);

    // A factory bank does not ship silent programs. Name them: on the H3500 one
    // algorithm with min == max == 0 on every parameter rendered 66 silent, and
    // a count without names is not a lead.
    int silent = 0;
    {
        std::vector<int> dead, blank;
        for (int k = 0; k < n; ++k)
        {
            bool all7 = true;
            for (int x = 0; x < kNumSources && all7; ++x)
                if (! feats[(size_t) x][(size_t) k].silent) all7 = false;
            if (! all7) continue;
            // Blank AT SOURCE is fidelity; blank anywhere else is the fault this
            // section exists to find. Counting them together hides one in the
            // other, so they are counted apart and only the second is a verdict.
            if (u.expectedSilent && u.expectedSilent (rec (idx[(size_t) k])))
                blank.push_back (k);
            else
                dead.push_back (k);
        }
        silent = (int) dead.size();
        std::printf ("   programs silent on EVERY source     %d of %d\n", silent, n);
        for (int k : dead)
            std::printf ("     %-5d %s\n", idx[(size_t) k], all[(size_t) idx[(size_t) k]].name.c_str());
        if (! blank.empty())
        {
            std::printf ("   blank at SOURCE (silence is correct)  %d of %d\n",
                         (int) blank.size(), n);
            for (int k : blank)
                std::printf ("     %-5d %s\n", idx[(size_t) k],
                             all[(size_t) idx[(size_t) k]].name.c_str());
        }
        std::printf ("\n");
    }

    // ---- 5. TWINS ---------------------------------------------------------
    // A program is a twin only when it is below the threshold on EVERY
    // excitation: a held note cannot show a fast envelope and a fast line cannot
    // show a long release, so a pair has to fail all seven before it is called
    // one sound.
    int sounds = n;
    {
        std::vector<int> parent ((size_t) n);
        for (int i = 0; i < n; ++i) parent[(size_t) i] = i;
        std::function<int (int)> find = [&] (int a)
        { while (parent[(size_t) a] != a)
          { parent[(size_t) a] = parent[(size_t) parent[(size_t) a]]; a = parent[(size_t) a]; }
          return a; };
        long pairs = 0;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
            {
                bool twin = true;
                for (int x = 0; x < kNumSources && twin; ++x)
                    if (dsp::measure::difference (feats[(size_t) x][(size_t) i],
                                                  feats[(size_t) x][(size_t) j])
                        >= dsp::measure::Report::kIdentical) twin = false;
                if (twin) { const int a = find (i), b = find (j);
                            if (a != b) parent[(size_t) b] = a; ++pairs; }
            }
        std::map<int, std::vector<int>> groups;
        for (int i = 0; i < n; ++i) groups[find (i)].push_back (i);
        int inTwins = 0;
        for (auto& kv : groups) if (kv.second.size() > 1) inTwins += (int) kv.second.size();
        sounds = (int) groups.size();
        std::printf ("5. TWINS - programs that are another program on EVERY one of the seven sources\n");
        std::printf ("   twin pairs                          %ld\n", pairs);
        std::printf ("   programs involved                   %d of %d (%.1f %%)\n",
                     inTwins, n, 100.0 * (double) inTwins / (double) std::max (1, n));
        std::printf ("   EFFECTIVE DISTINCT SOUNDS           %d of %d\n\n", sounds, n);
        int shown = 0;
        for (auto& kv : groups)
        {
            if (kv.second.size() < 2) continue;
            if (++shown > 60) { std::printf ("   ... and more\n"); break; }
            std::printf ("   [%d] %s", (int) kv.second.size(),
                         all[(size_t) idx[(size_t) kv.second[0]]].name.c_str());
            for (size_t m = 1; m < kv.second.size() && m < 8; ++m)
                std::printf (" = %s", all[(size_t) idx[(size_t) kv.second[m]]].name.c_str());
            if (kv.second.size() > 8) std::printf (" = ...");
            std::printf ("\n");
        }
        if (shown) std::printf ("\n");
    }

    // ---- the floors --------------------------------------------------------
    std::vector<double> noteMedians;
    for (int x = 0; x < 3; ++x)
        noteMedians.push_back (dsp::measure::compare (feats[(size_t) x]).median);
    std::sort (noteMedians.begin(), noteMedians.end());
    const double noteMedian = noteMedians[1];
    std::vector<double> musical;
    for (int x = 3; x < kNumSources; ++x)
        musical.push_back (dsp::measure::compare (feats[(size_t) x]).median);
    std::sort (musical.begin(), musical.end());
    const double musicalMedian = musical[musical.size() / 2];

    std::printf ("HEADLINE  %d effective distinct sounds of %d, single-note median %.4f,\n"
                 "          musical median %.4f\n\n", sounds, n, noteMedian, musicalMedian);

    // A percentage ceiling cannot express "one known silent program" once the
    // bank is SAMPLED: one of forty is 2.5 %, one of 478 is 0.2 %, and the same
    // program is behind both. So a unit that has declared any tolerance at all
    // tolerates at least one, and the ceiling does the rest.
    //
    // Prefer `Unit::expectedSilent` to this where the blankness can be PROVED
    // from the source sound: it names the programs instead of budgeting for
    // them, so the ceiling stays at zero for silence nobody has explained.
    const int silentAllowed = u.floors.maxSilentPct > 0
                            ? std::max (1, u.floors.maxSilentPct * n / 100)
                            : 0;
    if (silent > silentAllowed)
    { std::printf ("FAIL  %d of %d programs are silent on every source; the ceiling is %d\n",
                   silent, n, silentAllowed); ++bad; }
    else if (silent > 0)
        std::printf ("note  %d of %d programs are silent on every source, inside the ceiling of\n"
                     "      %d that this unit has explained in its bugs.md\n",
                     silent, n, silentAllowed);
    if (noteMedian < u.floors.noteMedian)
    { std::printf ("FAIL  single-note median %.4f is under the floor of %.2f\n",
                   noteMedian, u.floors.noteMedian); ++bad; }
    if (musicalMedian < u.floors.musicalMedian)
    { std::printf ("FAIL  musical median %.4f is under the floor of %.2f\n",
                   musicalMedian, u.floors.musicalMedian); ++bad; }
    if (sounds * 100 < u.floors.soundsPct * n)
    { std::printf ("FAIL  %d effective sounds for %d programs is under the floor of %d %%\n",
                   sounds, n, u.floors.soundsPct); ++bad; }
    return bad;
}

/** The whole `main`, so a unit's probe is its `Unit` and nothing else.

      <unit>_spread              measure the whole bank
      <unit>_spread --step N     every Nth program (for ctest)
      <unit>_spread --selftest   collapse the bank on purpose and require this
                                 probe to notice */
inline int main (int argc, char** argv, const Unit& u)
{
    int step = 1; bool self = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--selftest") == 0) self = true;
        else if (std::strcmp (argv[i], "--step") == 0 && i + 1 < argc) step = std::atoi (argv[++i]);
    }
    if (step < 1) step = 1;

    if (u.programs.size() < 2)
    {
        std::printf ("SKIP  %s: fewer than two factory programs available (%d).\n"
                     "      Nothing measured, nothing claimed.\n",
                     u.title, (int) u.programs.size());
        return 0;
    }

    if (! self)
    {
        const int bad = run (u, step, false);
        std::printf ("%s\n", bad == 0 ? "PASS - the bank still spreads"
                                      : "FAIL - the bank has collapsed, see above");
        return bad == 0 ? 0 : 1;
    }

    std::printf ("SELFTEST - collapse every program onto one record and require the probe to\n"
                 "notice. That is what a voice model reading only the category field does, and it\n"
                 "is how 707 PSR-730 voices came out as 38 effective sounds with every other test\n"
                 "in the suite green.\n\n");
    const int bad = run (u, step, true);
    std::printf ("\nthe collapsed bank produced %d findings\n", bad);
    std::printf ("%s\n", bad > 0 ? "PASS - the probe catches a collapsed bank"
                                 : "FAIL - the probe passed a bank of one sound");
    return bad > 0 ? 0 : 1;
}

}} // namespace dsp::spread
