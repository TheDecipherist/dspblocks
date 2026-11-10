# Milestone 5: the 88-key map, and the first music

The author: *"we are getting closer for sure."*

Everything before this was one note at a time with hand-picked numbers, which is
a physics demo rather than an instrument. This set is the whole stack played
polyphonically through ONE shared soundboard, with every note's parameters taken
from where it sits on the keyboard.

- **1 - C major chord**, six notes together.
- **2 - rolled chord**, eight notes 75 ms apart. This is where unisons and decay
  show, because you hear each note arrive into the ones already ringing.
- **3 - phrase**, alternating hard and soft, to hear velocity working musically
  rather than in a test.
- **4 - across the keyboard**, five notes from E1 to E7, to hear the map do its
  job.

Each is `--dry` (the instrument) and `--with-hall`.

## Two changes since the last set, both from the author's ears

**The action now makes a noise.** Every note before this began as a pure tone,
which nothing physical does, and it is the tell that no amount of partial
accuracy removes. A felt hammer thrown by a lever arrives with a thump, and that
thump is broadband, short, and scales hard with velocity. Measured: the first
5 ms went from a spectral flatness of 0.0075 to 0.0363, five times broader, while
the sustain stayed at 0.0001 - so it is the ATTACK that changed and not the note.
A soft blow is 35 times quieter than a hard one, because the whole mechanism is
moving slower.

**The reverb has stopped detuning.** The author: "the reverb is odd sometimes." Our tank
defaults to 0.9 Hz of spin and 12 ms of wander, which is what makes a Lexicon
hall breathe under a pad. A piano is the worst possible source for it: a tuned
percussive instrument whose partials sit still for seconds. A steady 440 Hz tone
through the hall measured **4.2 cents of average wobble and 31.5 cents at worst**
- a third of a semitone. Modulation is off in these renders and diffusion is
raised a little to keep the tail from going metallic without it.

That is a finding about using this reverb on THIS source, not a fault in the
reverb, so nothing in `Reverb.h` was changed - other machines are voiced against
those defaults.

## What the key map says

| key | Hz | B | T60 | strings | hard | strike |
|---|---|---|---|---|---|---|
| A0 | 27.5 | 0.000800 | 55 s | 1 | 0.28 | 0.125 |
| C2 | 65.4 | 0.000254 | 47 s | 2 | 0.37 | 0.117 |
| C3 | 130.8 | 0.000064 | 25 s | 2 | 0.45 | 0.111 |
| C4 | 261.6 | 0.000085 | 13 s | 3 | 0.53 | 0.105 |
| C6 | 1046.5 | 0.000653 | 3.7 s | 3 | 0.68 | 0.092 |
| C8 | 4186.0 | 0.001912 | 1.0 s | 3 | 0.83 | 0.080 |

**Inharmonicity is U-shaped**, and that is the fact everyone gets wrong. B is
LOWEST in the tenor and climbs at both ends: in the bass because the strings have
to be wound and short for their pitch, and in the top octave because they get very
short relative to their diameter. A model where stiffness simply rises with pitch
has a dull treble.

## What to listen for

**Does the chord hang together as one instrument?** Six notes through one board
is the first time that question can even be asked.

**Does the rolled chord sound like a piano sustaining?** That is the unisons and
the two-stage decay doing their work in company rather than alone.

**And across the keyboard, does each register sound like the same instrument?**
If the bass sounds like a different machine from the treble, the map is wrong
rather than merely rough.

## Still missing

**Sympathetic resonance** - every note alive on one bridge, which is what makes
the sustain pedal a pedal instead of a reverb send. It breaks the per-voice
assumption every synth in this workspace is built on, so it is real work.

**The noises** - hammer thump, key mechanism, damper landing on release.

**A player.** No keyboard, no pedals, no plugin. These are renders.
