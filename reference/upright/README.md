# Upright Piano KW

**FreePats**, recorded by Gonzalo and Roberto in January 2017 from a Kawai upright
in a living room, with a Zoom H1 portable recorder. Published under
**Creative Commons CC0 1.0** - public domain, no credit required.

<https://freepats.zenvoid.org/Piano/acoustic-grand-piano.html#UprightKW>
Source repository: <https://github.com/freepats/upright-piano-KW>

## What is here

Converted from the published SFZ + FLAC release on 2026-10-07 into the naming
Solera's loader already reads, `Piano.<dyn>.<note>.wav`, so the same loader gives
it the onset trim, the per-note level match and the common length cap that the
Steinway gets. The conversion is `ffmpeg -ar 48000`; nothing else was done to the
audio here.

| dynamic | notes |
|---|---|
| pp | 30 |
| mf | 30 |
| ff | 36 |

**TWO RECORDED VELOCITY LAYERS, NOT THREE.** The set is `vL` and `vH`, split at
velocity 80 in its own SFZ. The soft layer fills both pp and mf, so on this
instrument those two differ in LEVEL but not in TONE. The Steinway has three real
layers and does differ. That is a property of the recording, not of the engine.

**AND IT IS SPARSE**: 33 root notes over 88 keys, mapped by the SFZ as key ranges.
The loader's `pick` already searches outward for the nearest recorded note and
transposes, which is what covers the gaps - the same path the Steinway's three
missing files take.

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

**TWO LAYERS, STORED AS TWO.** The set is `vL` and `vH`; they are kept as pp and ff, and mf is
empty, which the loader serves from the softer of the two.
