# Milestone 4: the soundboard

The author: *"remember a piano needs reverb. I am looking at like a grand piano."*

The reverb was the right instinct and the wrong object. A hall is the space
AROUND an instrument. A soundboard is part OF it: a big ribbed spruce plate that
every string is bolted to through the bridge, with its own resonances, colouring
the note before any room hears it. A grand's tone is mostly that plate, which is
why the last set sounded like strings in a nice room rather than a piano in one.

This set is the whole stack - dispersive strings, felt hammer, coupled unison,
and now the plate - rendered `--dry` (the instrument) and `--with-hall` (the
instrument in a room). Four notes with the string count a real piano uses: one
at A1, two at A2, three at A3 and A4.

## What the plate is doing, measured

| | |
|---|---|
| rings for | 0.352 s, so it is a plate and not a reverb |
| colours by | 51.3 dB across 60 Hz to 6 kHz, 33 peaks |
| bass rolloff | midrange sits 8.3 dB over 18-42 Hz |
| strike survives | attack still brightens 1.52x with velocity |

That first row is the whole distinction. A room takes seconds; a plate takes a
few hundred milliseconds. Anything longer smears the attack the hammer worked
for, which is the failure mode of using a reverb as a soundboard.

The bass rolloff is why A1 sounds the way it does. A plate cannot push air at
long wavelengths, so a piano's lowest notes are carried by their upper partials
rather than their fundamentals - and it is why a short grand has no bottom.

## What to listen for

**Does the dry render sound like an instrument now, rather than strings?** That
is the question the plate exists to answer. If it still sounds like wire, the
board is wrong rather than merely absent.

**And does the hall now sit on something?** Reverb on a coloured source should
read as a room around a piano; reverb on an uncoloured one reads as a room
around a synth.

## Still missing

**Sympathetic resonance.** All 88 notes alive on one bridge, so undamped strings
ring along. It is what makes the sustain pedal sound like a pedal and not like a
reverb send, and it breaks the per-voice assumption every synth here is built on.

**The noises** - hammer thump, key mechanism, damper landing on release. Small,
and a disproportionate share of what reads as real.

**One note, not a keyboard.** Every parameter here is per-pitch and the 88-key
map that makes it an instrument rather than a physics demo does not exist yet.
