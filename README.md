# dspblocks

The effect blocks the rebuilt units are made of: delay, pitch shift, phaser,
flanger, chorus, tremolo, filters, drive and two reverb tanks. Header-only
C++17, no dependencies, one file per family.

## Why this exists

Six effects units were rebuilt from firmware — Lexicon PCM 91 and PCM 80,
TC Reverb 4000 and TC 2290, Eventide H3000 and H3500 — and every one of them
reproduced its machine's PARAMETER layer faithfully while running a single
generic engine underneath. The H3000 documents 23 algorithms and had three
family branches with seven algorithms matching none of them. The PCM 91 has no
delay path at all: its "Tape Echoes" and "Slap + Verb" programs are the reverb
tank with the echo taps turned up, which is why they sound like reverbs.

That is the fault this library fixes. A machine's algorithm list is only real if
a Digiplex sounds like a delay, a Band Delay sounds like a filtered delay and a
Swept Comb sounds like a comb.

## The honest position

**These blocks are ours.** The original DSP lives inside a custom chip in every
one of these machines — the Lexichip's control store is internal to the part,
the H3000's PELs run microcode that was never dumped — so nothing here is a
byte-level reproduction and nothing here should ever be tagged `[FW]`.

What this library aims at is the RIGHT KIND of effect with the right controls
and the right ranges: a pitch shifter that shifts by the interval the machine
prints, a phaser with the number of stages the manual states, a delay whose
maximum is the machine's own delay memory. Where a firmware-derived constant
exists, the unit passes it in. Where one does not, the block's default is
ordinary DSP practice and is marked `[EAR]` in the unit that uses it.

That is a real and useful instrument. It is not a clone, and the documents say
so.

## Layout

    include/dsp/Delay.h        delay line, fractional read, feedback, multi-tap
    include/dsp/DelayDesigner.h  two lanes of hand-placed taps, each with its own
                               time, level, feedback, decay and pan
    include/dsp/Pitch.h        pitch shifter, pitch detector, diatonic shifter
    include/dsp/Modulation.h   LFO, phaser, flanger, chorus, tremolo, rotary, ring mod
    include/dsp/Filters.h      one-pole, state-variable, biquad, comb, allpass, filter bank
    include/dsp/Drive.h        soft clip, asymmetric drive, bit crush
    include/dsp/Reverb.h       tap sets as data, Schroeder/Gardner allpass tank,
                               gate/inverse envelope, early-reflection player
    include/dsp/ReverbFigure8.h  the SECOND reverb topology: a Dattorro-style
                               figure-of-eight tank, modulated by default
    include/dsp/EarlyPatterns.h  the early-reflection pattern as data, feedable
                               to either tank
    include/dsp/Dynamics.h     envelope follower, compressor, expander, ducker
    include/dsp/Sampler.h      record/replay, stutter, continuous reverse
    include/dsp/Blocks.h       includes all of the above

Three of these are worth calling out because a unit that skips them ends up with
the fault this library exists to fix.

`Reverb.h` takes its tap set as a **`ReverbTopology` struct**, not as a mode
enum, so a plate and a hall are different delay sets rather than the same tank
with a different decay number. `topologies::hall/plate/room/chamber/ambience/
cathedral` are supplied; a unit with better evidence passes its own.

### Two reverb topologies, not one

`Reverb.h` is the Schroeder/Gardner chain: input diffusers into two
cross-coupled loops of allpasses, a short circuit (50 to 270 ms), dense almost
immediately. Four units run it.

`ReverbFigure8.h` is a second STRUCTURE, not a replacement: Dattorro's
figure-of-eight (JAES 45(10), 1997) - bandwidth filter, four input diffusers,
then a loop that runs through two halves, each a modulated allpass, a delay, a
damping filter, a second allpass and a second delay, with the stereo field read
from seven signed taps INSIDE the tank. Its circuit is ten times longer, it
fills in more slowly, and its two tank input allpasses modulate continuously
whether or not anyone asked. `figure8::plate()` is the paper's plate verbatim;
`hall/room/chamber` are ours.

**Nothing that includes `Reverb.h` changed when this was added.** Which of the
two sounds better is not a thing a test can assert, so the way to find out is

    cmake --build build --target dsp_reverb_ab
    ./build/dsp_reverb_ab <output directory>

which renders the same impulse, noise burst and tone burst through both at
matched settings, writes eight WAVs, and prints RT60, echo density at four
instants and how far a steady tone's level wanders in each. Adopting one in a
unit is a separate, measured change - an A/B and a listen, never a silent swap.

Both tanks are plugin-ready: they take a sample rate and plain setters, hold no
patch state and no host types, so a standalone reverb plugin is a thin wrapper
over one of them rather than a copy of it.

`EarlyPatterns.h` keeps the early pattern away from both. A pattern is DATA -
times in milliseconds, signed gains - and either tank can be fed the same one.
The library's own patterns are `[EAR]`; a unit that has RECOVERED its machine's
pattern table passes it in, and that header says where the worked example lives
and records one table that looks recovered and is not.

`Dynamics.h` exists because these machines make real use of it — the TC 2290's
dynamic delay ducks its repeats under the playing, the PCM 91's Concert Hall has
Compress and Expand rows in its own edit matrix — and modelling the parameter
without the processing is how a preset ends up sounding like every other preset.

## Using it

Header-only, so a unit adds the include directory and nothing else:

```cmake
target_include_directories(MyUnit PRIVATE ${CMAKE_SOURCE_DIR}/../dspblocks/include)
```

Every block takes its sample rate in `prepare()` and is otherwise driven by
plain setters, so a unit's own coefficient builder stays where it belongs: in
the unit, next to the firmware evidence that produced it.

## Known, measured, and not fixed

`Figure8Tank` does not deliver the decay time it is asked for, and the gap is
big enough to hear. Plate topology, 48 kHz, damping and high cut opened right
up, impulse in, RT60 fitted between -20 and -50 dB, averaged over asks of 2, 4
and 8 seconds:

| what is running | measured / asked |
|---|---|
| whole-sample delays, no modulation | 0.93 to 0.94 |
| the loop's fractional reads | 0.89 to 0.90 |
| plus Dattorro's own +-0.27 ms of modulation | 0.82 to 0.84 |

The law itself is 6 to 7 per cent optimistic, which is inside the band its own
header claims. The loop's fractional delay reads cost about 3 more. The
modulation costs another 8, and that is the largest single term: nothing in
`setDecaySeconds` knows the tank is moving while it decays. The ratio is flat
across Size and flat at 96 kHz, so one factor covers the whole control.

The same interpolation darkens the tail. Reading the two static loop delays and
the output taps at whole samples, as `~/projects/supersynth/Source/Reverb.h`
does, moves the tail's spectral centroid from 5744 Hz to 6628 Hz against that
reverb's 7243 Hz, so it is more than half the brightness difference between the
two implementations.

Fixing either changes the tail of every machine running this tank: the H3000,
the H3500, the PCM 80, the PCM 91, the Reverb 4000, the D-50 and the shell's own
integrated reverb. That is a deliberate decision about seven shipping
instruments, not a side effect of a bug fix, so it is the author's call and nothing
here has been changed. `synthshell`'s `FxReverbBlock` corrects for the decay
shortfall locally and its `test_fx_reverb.cpp` fails the day this is put right,
which is how the correction gets deleted instead of doubling up.

Measured 2026-09-12, against supersynth's reverb at its shipping defaults.

### `topologies::room()`'s 0.8 cross-coupling puts a hole in the low end

Measured 2026-10-05, while building the Lexicon PCM 60, which has a published
reverberant response of 20 Hz to 10 kHz +/-1 dB to check against.

`ReverbTank` at `crossCoupling` below 1 gives each half of the figure-of-eight
TWO return paths of different lengths - a short self-loop at gain `(1-xc)` and
the cross loop at `xc^2` - and they interfere. `topologies::room()` ships with
0.8, and the interference is not subtle. Octave-band T60 of one tail, asked for
a broadband 2.70 s and delivering exactly that:

| band centre | 62 | 125 | 250 | 500 | 1k | 2k | 4k |
|---|---|---|---|---|---|---|---|
| `crossCoupling` 0.8 | 1.58 | **0.64** | **1.08** | 2.24 | 2.50 | 2.77 | 2.70 |
| `crossCoupling` 1.0 | 3.25 | 3.14 | 3.95 | 4.08 | 3.85 | 4.24 | 2.92 |

`r2` is 1.000 in every one of those bands, so it is a clean exponential at the
wrong rate rather than a measurement artefact, and a broadband T60 cannot see it
at all. The same change moved what the tank DELIVERS against what it is asked
for from 0.26-0.50 up to 0.55-0.60, which is where the other topologies sit.

`dsp::measureBandT60` in `MeasureDecay.h` is what found it, and is the reason it
was added: nothing in this library could measure a per-band decay before, so
every two-band decay law in every unit was asserted by reading the code that
implemented it.

**Nothing has been changed.** `room()` still ships 0.8 and the PCM 60 overrides
it in its own `topologyFor()`, because `ambience()` is at 0.5 and some units may
be relying on the colour. Whether the default moves is the author's call.

## Tests

    cmake -B build && cmake --build build && ctest --test-dir build

The tests are behavioural, not golden-file: a delay is checked by finding its
echo at the requested time, a pitch shifter by measuring the frequency it
returns for a known input, a phaser by counting notches in its response. They
are what stops a block silently becoming a different effect.

## ReverbTank's stock topologies are too sparse to be a reverb (2026-10-05)

MEASURED, not an opinion. The author judged every finished effects unit by ear in one
session and his verdicts sort the archive perfectly by which tank it runs:

| tank | units | verdict |
|---|---|---|
| `Figure8Tank` | pcm80, reverb4000, pcm91, h3000, h3500 | *"the best reverbs and programs you have made so far"* |
| `ReverbTank` + `topologies::{room,plate,hall}()` | pcm60, mone, srv330, m300, rev5, rev7, rmx16, spx90, dep5, drv1000, pcm70, dre2000 | *"metal playing in a bathroom"*, *"a can through a comb"*, *"absolutely terrible"*, *"bizarre"* |

Twelve units on one side, five on the other, and not one exception.

The numbers, from `pcm60/tools/tail_quality_probe.cpp` on the wet impulse
response, 60 ms to 460 ms:

| program | size | echo density | comb peak | at lag |
|---|---|---|---|---|
| Plate | 0 | **0.202** | **0.340** | 5.01 ms |
| Plate | 3 | 0.333 | 0.231 | 2.24 ms |
| Room | 0 | 0.281 | **0.310** | 1.61 ms |
| Room | 2 | 0.264 | 0.264 | 2.29 ms |

**Echo density is the share of the sign-crossing rate that Gaussian noise of the
same length would give.** A tail that has diffused into a wash approaches 1.0.
These sit at 0.20 to 0.33 - three to five times too sparse - so the ear hears
individual repeats rather than a wash. The comb peak is the strongest
autocorrelation after 1 ms, and a lag of 5 ms is a 200 Hz ring: that is the
"metal" and the "comb" in the author's words, measured.

**The cause is the tap set, not the decay law.** `ReverbTank` delivers its T60
accurately once calibrated - the PCM 60 measures all 32 published cells at
r2 >= 0.998 - so every decay gate those units carry passes while the thing is
unlistenable. A reverb can be exactly the right LENGTH and the wrong SOUND, and
nothing in this library measured the second until now.

`Figure8Tank` is Dattorro's figure-of-eight: four input diffusers into two
modulated half-loops, read through six points inside it, with the TAP SET
SUPPLIED AS THE ALGORITHM'S OWN DATA rather than taken from a stock table. That
is what the five good units have and the twelve do not.

**So: do not reach for `topologies::room()`, `plate()` or `hall()` for a machine
whose reverb has to be judged by ear.** They are a scaffold for getting a unit
building, and they were never replaced on twelve machines. Use `Figure8Tank`
with the machine's own tap set, and measure the tail's density, not only its
length.
