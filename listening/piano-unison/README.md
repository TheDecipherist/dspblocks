# Milestone 3: the coupled unison, dry and in a hall

The author: *"still sounds more like a guitar than a piano"*, and then *"remember a
piano needs reverb. I am looking at like a grand piano."* Both are here.

Four notes, each with the number of strings a real piano puts under that pitch,
and each rendered twice.

| note | strings | why |
|---|---|---|
| A1, 55 Hz | 1 | the low bass really is one thick wound string |
| A2, 110 Hz | 2 | the tenor, where a piano runs pairs |
| A3, 220 Hz | 3 | and most of the keyboard runs threes |
| A4, 440 Hz | 3 | |

## What changed, and what to listen for

**The decay is two-stage now, and that is the guitar leaving.** The strings of a
unison are tuned a couple of cents apart. They start in phase, push the bridge
together and lose energy into it fast; then they drift out of phase, their forces
at the bridge cancel, the note stops paying, and what is left rings on quietly
for a long time. Measured on A3: the first half second falls at a rate worth a
0.92 s T60, the tail at 6.43 s - **seven times shallower**. A single string can
only give one exponential, which is what the last two sets did.

Nobody wrote two envelopes. It falls out of three detuned strings sharing one
bridge, and the gate proves that by turning the DETUNE off: identical strings
cannot drift, and the two-stage behaviour vanishes completely (0.00x against
6.98x). If it had survived that control, the explanation above would be wrong.

**The hall is the second half of your note.** `--dry` is the strings alone and
is what the model actually produces; `--with-hall` puts it through `ReverbTank`
on the hall topology, 2.2 s, size 1.5, damped at 6.5 kHz. **Which one reads as a
grand?** My guess is neither yet, and that the dry one will sound like strings
in a vacuum while the wet one sounds like strings in a room rather than a piano
in a room. That difference is the point of the next milestone.

## What is still missing

**The soundboard, which is not the same thing as reverb and cannot be faked with
it.** A hall is the space AROUND the instrument. A soundboard is part of the
instrument: a large stiff plate with its own resonances that every string is
bolted to, and it colours the note before any room hears it. A grand's tone is
mostly that plate. Reverb on top of an uncoloured string is a string in a nice
room.

**Sympathetic resonance**, which needs all 88 notes alive on one bridge, and is
what makes the sustain pedal sound like a pedal instead of a reverb send.

**The noises** - hammer thump, key, damper on release.

Levels are unnormalised. The hall renders peak lower because the dry signal is
scaled back to make room for the tail.
