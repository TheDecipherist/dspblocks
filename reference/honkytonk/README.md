# Piano FB - the honky-tonk

**FreePats**, recorded by Piotr Barcz from a **Francis Bacon player piano** with a
Zoom H1n, sent to FreePats in February 2020. Published under
**Creative Commons CC0 1.0** - public domain, no credit required.

<https://freepats.zenvoid.org/Piano/honky-tonk-piano.html#PianoFB>
Source repository: <https://github.com/freepats/old-piano-FB>

## What is here

Converted from the published SFZ + FLAC release on 2026-10-07 into
`Piano.<dyn>.<note>.wav`, as the upright was, so it gets the same onset trim,
level match and length cap.

78 notes, and **ONE recorded velocity layer**, which the conversion writes to all
three dynamics. So this instrument's tone does not change with how hard it is
played - only its level does, through the voice. A real honky-tonk is a worn
piano whose unisons have drifted, and that character is in the recording itself
rather than in anything we add.

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

**ONE LAYER, STORED ONCE.** Writing a single recording into pp, mf and ff held it three times on
disk and three times in RAM for no information at all. It is stored once now and the loader's
empty-dynamic rule serves the other two from it, preferring the softer side.
