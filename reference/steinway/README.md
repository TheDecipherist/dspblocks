# The reference: a real Steinway

Recordings from the **University of Iowa Electronic Music Studios**, made by
Lawrence Fritts and freely available since 1997 with no restrictions. 16-bit,
44.1 kHz, stereo, three dynamics (pp, mf, ff), note by note. Steinway & Sons
model B, played by Evan Mazunik, Voxman Music Building, 5 and 27 November 2001,
a Neumann KM 84 pair eight inches over the strings - a stereo, non-anechoic
recording, so the room is in it.
<https://theremin.music.uiowa.edu/MISpiano.html>

## What is here, as of 2026-10-01

**261 of a possible 264 notes**, fetched one at a time with a pause between, since
the page offers no archive and this is somebody else's server giving away somebody
else's work.

| dynamic | notes |
|---|---|
| pp | 87 |
| mf | 86 |
| ff | 88 |

The three that are not in the set, and the download is also the survey because the
coverage is published nowhere: **A0 at pp and mf, and Bb0 at mf.** The bottom of
the keyboard, which is also where a sampled piano is least often played and where
a model has the least to learn.

The `.aiff` originals and the mono 48 kHz `.wav` conversions are gitignored - they
are somebody else's recordings and they are a download away. 1.4 GB. What is kept
is `probes/reference_steinway.py`, which turns them into the numbers, and the
numbers themselves, below.

The studio asks that users consider a donation or an email about what was made
with the recordings.

## Why this exists

Six milestones of piano were built with no reference. Every judgement about
whether it sounded right was opinion against opinion, and the two-rail branch was
blocked on a question a single real note answers: how deep is the strike notch on
an actual stiff string?

## What it says

| note | f0 | B measured | B our keySpec | |
|---|---|---|---|---|
| A1 mf | 54.81 | 0.000133 | 0.000502 | **3.8x too high** |
| A3 pp | 221.55 | 0.000052 | 0.000085 | 1.6x |
| A3 mf | 220.34 | 0.000226 | 0.000085 | 0.4x |
| A3 ff | 222.26 | 0.000038 | 0.000085 | 2.2x |
| A5 mf | 887.63 | 0.000021 | 0.000283 | **13x too high** |

**The strike notch is real and deep.** The strongest dip against its neighbours:

- A1 mf: **n = 8, 26.0 dB**
- A3 mf: n = 5 at 21.1 dB, **n = 8 at 10.3 dB**
- A3 ff: n = 14 at 15.6, n = 5 at 15.5, **n = 8 at 7.3 dB**

Partial 8 is dipped in all three, which is a hammer at one eighth of the string
doing exactly what the physics says. Our two-rail model produces 12 to 17.6 dB at
B = 0, squarely in that range - so the topology is right and finishing it is
worth doing.

**And our inharmonicity is too high, badly so in the treble.** The U-curve in
`keySpec` puts its minimum around E3 and climbs after it. The real instrument is
still FALLING at A5, where B is 2.1e-5 against our 2.8e-4. The minimum belongs
much further up - around C5 to C6 - with the rise saved for the last octave.

The three A3 readings differ by a factor of six between dynamics, which B cannot
do: B is a property of the wire. That spread is the measurement's own noise and a
reminder not to trust any single row - the A1 and A5 readings agree with each
other and with scaling practice, and that is what the conclusion rests on.

## The .wav set is trimmed; the .aiff originals are not (2026-10-02)

The author, after a key took three seconds to speak: *"we need to fix the samples at their source
I think. There should be absolutely no silence before the wave it self. it should play when
a play"*.

**The recordings open with the room.** Measured across all 261 before the trim: the median
note carried **250 ms** of silence before the strike, and the bottom three at ff carried
over **four seconds** — `Piano.ff.Bb0` 4.262 s, `Piano.ff.B0` 4.154 s, `Piano.ff.A0`
4.010 s. Played from sample zero, a key press played the studio first and the lowest keys
were unusable.

So every `.wav` was trimmed in place: **255 files, 85 seconds of silence removed.** Each
now begins exactly **5 ms** before its onset.

- The threshold is **2 per cent of that file's own peak**, which clears a noise floor
  sitting about fifteen decibels down on the quietest `pp` note.
- The **5 ms run-up is deliberate**: the detector fires at 2 per cent and a piano's hammer
  noise starts below that, so cutting exactly at the onset would clip the front of the
  transient — which is most of what distinguishes one dynamic from another.
- The `.aiff` files are the studio's and are **untouched**. The `.wav` set is our own
  48 kHz mono conversion, so correcting it is fixing derived data, not editing a master.
  Re-run the conversion from the AIFFs if you ever need the silence back.

**It also fixed a measurement, which is the part worth remembering.** A brightness check
on the velocity layers had given three different answers and I concluded the metric was
wrong. It was not: that silent head is broadband hiss, it sat inside the measurement
window, and normalising to peak amplified it — so the "brightest" layer was whichever had
the most silence in frame. Trimmed, the figures lined up on the first run (pp 0.0024,
mf 0.0393, ff 0.0744, monotonic). Suspect the data before the instrument.

## Stored trimmed, as FLAC (2026-10-07)

Every file is cut from its own ONSET to the eight seconds the loader keeps, and encoded as
FLAC. That is not a quality decision: the loader already found the onset and capped at eight
seconds on every load, so what was removed is what it read and threw away. The Steinway's
recordings were 35 seconds of which 27 were discarded each time, and the tail of a piano note
is near-silence, which FLAC crushes - measured on three of our own files, 8568 KB to 1304.

The arithmetic is `tools/trimflac.py`'s and it duplicates the loader's own threshold (2 per
cent) and run-up (5 ms) deliberately. If that law changes in `Source/Steinway.cpp`, this script
changes with it, or the two drift and the trim starts cutting attacks.

The originals are not kept here. They are re-fetchable from the source named above, and the
loader reads WAV as well as FLAC, so a re-fetched set drops straight in.
