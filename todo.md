# Piano: what is open

## KNOWN RED, deliberately

**`piano_hammer_probe`: "a harder felt is brighter than a soft one" is inverted.**
Measured soft 1689 Hz against hard 444 Hz; it should be the other way round, and
it WAS at milestone 2 (soft 222, hard 444). It regressed during milestone 5's
stability work - the impedance scaling on the hammer's injection and the
compression limit - and I did not re-run this gate after those changes, which is
the process fault worth naming: a stability fix in one place is a change to the
physics everywhere, and the earlier gates are what say so.

Contact time is still correct (4.92 ms soft, 2.06 ms hard) and velocity
brightness is still correct (244 to 628 Hz, 2.57x), so the blow still behaves.
It is specifically the HARDNESS axis that inverted. Not yet diagnosed; the
chatter hypothesis (soft contact 4.9 ms against a 4.5 ms period, so the returning
wave re-bounces the hammer) was tested and is wrong - forbidding re-contact
changed nothing.

**`piano_hammer_probe`: the strike-position notch.** Currently passing, and it
should not be trusted: it reads partial 8 at -53.9 dB against -42.9 for its
neighbours, but both are near the floor, so it is measuring nothing. The notch is
genuinely absent. See below.

## THE NEXT REAL PIECE: TWO RAILS

The author, listening to milestone 6: *"it sounds like you are picking the strings, not
hammering them."*

He is right and the cause is identified. A hammer at 1/n of the string kills the
nth partial, because the waves it sends each way reflect off the two ends and
cancel when they recombine. That missing partial is a large part of what makes a
struck string sound struck. Measured at strike position 1/8 on a 220 Hz string:
plucked, partial 8 sits 20 dB below its neighbours; struck, it came out at
-0.8 dB, LOUDER than partials 6 and 7.

One circulating delay line cannot express it. Two attempts to fake it are
recorded in the file: a second opposite tap at `nInt - hSite` spaces the pair by
N(1-2p) and notches the wrong frequency entirely, and spacing them by p*N does
produce the notch and costs the hammer its velocity response, because an opposite
impulse a few samples away is a dipole and dipoles are bright. Three other rows
went red. A notch bought by destroying the velocity response is not the piano's
notch.

The fix is structural: an upper and a lower rail, the hammer driving both at its
position with the same sign, and separate reflections at the bridge and the
agraffe. The cancellation then falls out rather than being drawn on.

## Also open

- **Sympathetic resonance.** Every note alive on one bridge, which is what makes
  the sustain pedal a pedal rather than a reverb send. It breaks the per-voice
  assumption every synth in this workspace is built on.
- **A player.** No keyboard, no pedals, no plugin. Everything so far is renders.

## OPEN: ReverbTank delivers about half the decay time it is asked for

Found 2026-10-04 while building the Roland SRV-330, whose REVERB TIME is a
printed range (0.01 to 100.0 s) rather than an arbitrary knob, so the number
had to mean seconds.

Measured: fitted T30 against the requested time, `topologies::hall()`, damping
out of circuit, 20 ms windows, least-squares slope over -5 to -35 dB
extrapolated to -60.

    asked 0.5 s -> 0.30     asked 1 s -> 0.54     asked 2 s -> 1.05
    asked 4 s   -> 1.96     asked 8 s -> 3.89

A clean constant over a 16:1 range, converging on 0.49.

RULED OUT, each by measurement:
- damping. `setDamping(20000)` measures the same 1.06 s as 8000.
- diffusion. Short at density 0, 40, 70 and 100 (1.45 / 1.30 / 1.25 / 1.30).
- the envelope. `enabled` is false by default and nothing turns it on.
- the meter. The first version took -60 dB from the absolute peak, which reads
  SHORT because the tank's first output is a splat far louder than the tail.
  Replacing it with a proper T30 fit made the discrepancy BIGGER, not smaller,
  which is the evidence that it is real.

SUSPECTED CAUSE, not yet confirmed: `rebuild()` computes

    sum = (loopS[0] + loopS[1]) * size + allpass stretches

but `process()` reads ONE line per round trip of the figure-of-eight
(`loopLine[1]` at dA, `loopLine[0]` at dB). If a trip traverses one delay
rather than both, `loopTime` is twice the true loop period, `need = 60 *
loopTime / seconds` is twice too large, and every trip loses twice the dB it
should - which is exactly a halved tail.

NOT CHANGED HERE, deliberately. This header says four shipping units are tuned
against the current behaviour and that swapping the decay form "would change
the tail of four shipping units for no measured gain". The same applies to
this. The SRV-330 therefore calibrates at its own call site, with the table
above inline, and this entry is the record so the next reader does not
re-derive it.

What would settle it: drive the tank with a known topology, measure the actual
period between successive impulse responses in the loop, and compare against
`loopSeconds()`. If they differ by 2x, the fix is in rebuild() and every unit
needs re-measuring together.

### 2026-10-05, the Korg DRV-1000: it is a LINE, and it is topology dependent

Six more topologies, measured with `dsp::measureT60` (the new `MeasureDecay.h`)
on a 60 ms noise burst, spin and wander at zero, over each pattern's own
published decay range. The DRV-1000 needed this because Korg published its
complete 8 x 8 decay table, so every number below has an external check.

Delivered against asked, least squares weighted by 1/y^2 so the fit minimises
RELATIVE error:

    topology (as the DRV-1000 scales it)   delivered = a * asked + b   worst
    hall,      scale 0.78                  0.556 * asked + 0.057       1.3 %
    cathedral, scale 1.45                  0.464 * asked + 0.097       2.5 %
    room,      scale 0.55, 5 tank allpass  0.771 * asked + 0.007       2.4 %
    chamber,   scale 0.72                  0.494 * asked + 0.096       1.2 %
    plate,     scale 1.70                  0.506 * asked + 0.110       4.1 %
    plate,     scale 1.32                  0.472 * asked + 0.103       2.9 %

TWO THINGS THAT ARE NEW HERE.

**`a` runs 0.46 to 0.77 across tap sets.** The SRV-330's 0.49 is not a constant
of the block, it is a constant of `hall()` at that unit's settings, and a second
unit reusing it would be 60 % out on a room. Any fix in `rebuild()` has to be
re-measured per topology, which is the cost of changing it.

**It is NOT a scale factor, it is an affine line with a positive intercept.** A
single ratio lands the middle of a pattern's range and misses both ends:
cathedral at scale 1.45 runs 1.2 s to 10.0 s and a pure ratio was 17 % out at the
short end. A fixed loss per trip costs proportionally more the longer the tail is
meant to be, and the in-loop `damp` one-pole is the obvious candidate - with
damping OUT of circuit the SRV-330 measured a clean constant 0.49 over a 16:1
range, and with it in, every one of the six above has an intercept of 0.06 to
0.11 seconds. That is a testable difference between the two readings and it costs
one more run to settle.

**Also found, and it is not the decay law:** the tank's default 12 ms wander at
0.9 Hz is enough to stop a short plate's tail being a single exponential at all -
the T30 fit's r2 falls to 0.96, and a tail that is not exponential has no RT60.
Any unit modelling a machine with no modulation in it wants `setSpin(0)` and
`setWander(0)`, and `ReverbTank`'s defaults do not say so.

NOTHING WAS CHANGED IN THIS REPO FOR THE DRV-1000. It calibrates at its own call
site with the line above, the same way the SRV-330 does with its scalar, and both
entries are here so the next reader does not re-derive either.

## 2026-10-05 - EnvelopeFollower gained an RMS mode (TC Triple-C)

`dsp::EnvelopeFollower` now has `setDetector(Detector::Peak | Detector::Rms)`,
and `Compressor` forwards it. **PEAK IS STILL THE DEFAULT**, so nothing written
before this changes behaviour; the four shipping units that use these blocks were
rebuilt and are unaffected.

Why: the TC Triple-C's manual says of itself "The TRIPLE-C is basically an RMS
based Compressor, meaning that it responds to average level of the incoming
source material. This key turns the TRIPLE-C into a Peak based Compressor." A
machine that states its own detector wins over the library's default.

One thing inside it is worth knowing before using it. **In the RMS branch the
attack and release coefficients are computed for HALF the requested time.** The
MEAN SQUARE is what is smoothed and the square root halves the exponent, so
smoothing x^2 with a time constant of T/2 gives an ENVELOPE with a time constant
of T - which is what the Attack and Release a panel prints have to mean, since
one pair of knobs serves both detector modes. Without the halving the Triple-C's
printed 300 ms release measured 600 ms in the audio. With it, its sync probe
reads 500.09 ms against an expected 500.00.

### 2026-10-05, the Sony DRE-2000: third time, so here is the RECIPE

Three units have now calibrated around the ReverbTank decay shortfall - the
SRV-330 with a scalar, the DRV-1000 with a line, the DRE-2000 with four lines,
one per program - and each of them re-derived the method. The rule is that the
second time something is done it becomes a tool or a written procedure, so:

1. **Build a calibration binary against the unit's own engine**, not against the
   tank directly. The shortfall depends on size, diffusion and the tap set, and
   the unit's `makeCoeffs` is what sets those. A number measured on a bare tank
   does not transfer.
2. **Measure in the band the machine's own figure is referenced to.** The
   DRE-2000 publishes its reverb time at 500 Hz - 1 kHz, so the tail is
   band-passed there before `dsp::measureT60` sees it. Skip this on a machine
   with band multipliers and the reading is dragged by whichever band is longest:
   REVERB A's x2.0 low multiplier reads LONG and REVERB C's x0.5 high multiplier
   reads SHORT, and both look like engine bugs.
3. **Set every band multiplier to x1.0 while measuring**, so only the mid-band
   law is in play.
4. **Eight asked times across the machine's whole published range**, least
   squares weighted by `1/y^2` so the fit minimises RELATIVE error.
5. **Invert the line at the call site** and convert each band SEPARATELY, then
   express the result back as a multiplier: the line has an intercept, so
   multiplying a corrected mid time by the published multiplier carries that
   intercept into the band by the wrong amount. The DRE-2000's
   `tankAskSeconds()` is the worked example.
6. **Put a gate on it.** The calibration is stale the moment a tap set, a size
   or a diffusion changes, and nothing else will say so.

The DRE-2000's four lines, for the record:

    REVERB A   hall  @1.25   delivered = 0.508297 * asked + 0.059157   worst 8.6 %
    REVERB B   hall  @1.00   delivered = 0.518616 * asked + 0.024871   worst 3.9 %
    REVERB C   plate @1.00   delivered = 0.339832 * asked + 0.006115   worst 1.2 %
    REVERB D   plate @1.35   delivered = 0.393272 * asked + 0.013253   worst 1.6 %

`a` now runs 0.34 to 0.77 across six topologies and three units. It is not a
constant of the block and it never was.
