// ONE PIANO STRING, as a dispersive digital waveguide.
//
// WHY NOT A WAVEFORM, WHICH IS THE FIRST THING EVERYONE REACHES FOR. A piano note
// has no waveform, for two independent reasons and either one is fatal:
//
//   1. ITS PARTIALS ARE NOT AT HARMONIC RATIOS. A piano string is stiff, so partial
//      n sits at  f_n = n * f0 * sqrt(1 + B*n^2),  sharp of harmonic and more so the
//      higher you go. The 16th partial of a low note is most of a semitone sharp.
//      A stretched series never repeats, so there is no cycle to store.
//   2. THE SPECTRUM CHANGES THROUGHOUT THE NOTE. The top partials are gone in under
//      a second while the fundamental rings for twenty. A waveform has a fixed
//      spectrum by definition, and a wavetable needs one table per velocity per note
//      per moment.
//
// That stretch IS the piano. It is why piano tuning is stretched, and it is the one
// thing a subtractive oscillator can never produce.
//
// THE STRUCTURE. A delay line long enough to hold one period, with three things in
// the loop, and each earns its place:
//
//        in --->(+)---> [ delay N + frac ] ---> [ allpass x M ] ---> [ loss ] ---+
//                ^                               DISPERSION          DECAY      |
//                |________________________________________________________ out  |
//
//   * THE LOSS FILTER is a one-pole lowpass. It sets how long the note rings AND
//     makes the highs die before the lows, which is most of what piano decay sounds
//     like. One number would give an organ.
//   * THE DISPERSION CHAIN is M cascaded first-order allpasses with a NEGATIVE
//     coefficient, so group delay falls with frequency: the highs travel faster,
//     come round sooner, and land sharp. That is the inharmonicity, made rather than
//     faked. Nothing else in this codebase does it.
//   * THE FRACTIONAL DELAY is a first-order allpass, so the pitch is exact instead
//     of quantised to whole samples.
//
// WHAT THIS IS NOT, YET. No hammer (velocity here is a scalar on the excitation, not
// a felt collision, so it changes level and not spectrum), no coupled unison strings
// and therefore no two-stage decay, no soundboard, no sympathetic resonance. Those
// are milestones 2 to 4 and each has its own gate. This file is milestone 1 and the
// only claim it makes is the one its probe measures: the partials come out where the
// stiff-string law says they should.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace dsp { namespace piano {

/** A first-order allpass:  A(z) = (a + z^-1) / (1 + a*z^-1).
    Phase delay at DC is (1-a)/(1+a), which is how the tuning below accounts for it. */
struct Allpass1
{
    float a = 0.0f, x1 = 0.0f, y1 = 0.0f;
    inline float process (float x) noexcept
    {
        const float y = a * x + x1 - a * y1;
        x1 = x; y1 = y;
        return y;
    }
    void reset() noexcept { x1 = y1 = 0.0f; }
    /** Phase delay at DC, in samples. */
    [[nodiscard]] float dcDelay() const noexcept { return (1.0f - a) / (1.0f + a); }

    /** PHASE DELAY AT A FREQUENCY, and the design lives or dies on using this rather
        than the DC figure. The delay is nearly FLAT from DC until well up the band and
        only falls near Nyquist, so a coefficient chosen from `dcDelay` alone dispersed
        nothing where the partials actually are: at a = -0.3 the delay across a 110 Hz
        string's first twenty partials moves 1.857 to 1.827 samples, which is 0.8 cents
        at partial 20. Measured, and it is why the first attempt read B = 0.000003.
        @param w  radians per sample */
    [[nodiscard]] static float phaseDelay (float a, float w) noexcept
    {
        if (w < 1.0e-7f) return (1.0f - a) / (1.0f + a);
        const float c = std::cos (w), s = std::sin (w);
        const float arg = std::atan2 (-s, a + c) - std::atan2 (-a * s, 1.0f + a * c);
        float ph = -arg;
        while (ph < 0.0f) ph += 6.28318530718f;
        return ph / w;
    }
};

/** Phase delay of the one-pole loss filter  y = (1-d)x + d*y1, in samples. */
[[nodiscard]] inline float onePoleDelay (float d, float w) noexcept
{
    if (w < 1.0e-7f) return d / (1.0f - d);
    return std::atan2 (d * std::sin (w), 1.0f - d * std::cos (w)) / w;
}

class String
{
public:
    static constexpr int kMaxDispersion = 8;

    void prepare (double sampleRate, float lowestHz = 20.0f)
    {
        sr = (float) sampleRate;
        // TWO RAILS. A real string carries a wave toward the bridge and a wave away
        // from it; a hammer at a point drives BOTH, and their recombination after
        // reflecting off the bridge and the agraffe is what kills the nth partial for a
        // hammer at 1/n. A single circulating loop has only one direction to inject
        // into, so every partial comes out equally excited and no notch exists. The
        // reference proved that is not a subtlety: on a real C2, partial 8 sits 14 dB
        // under its own neighbours while ours was flat from partial 1 to 9.
        const int cap = (int) std::ceil (sampleRate / lowestHz / 2.0) + 4;
        upper.assign ((size_t) cap, 0.0f);
        lower.assign ((size_t) cap, 0.0f);
        len = cap;
        reset();
    }

    void reset() noexcept
    {
        std::fill (upper.begin(), upper.end(), 0.0f);
        std::fill (lower.begin(), lower.end(), 0.0f);
        ui = li = 0; lossZ = 0.0f; tune.reset(); tuneL.reset();
        for (auto& ap : disp)  ap.reset();
        for (auto& ap : dispL) ap.reset();
        hOn = false;
    }

    /** THE STRING ITSELF, asked for in the terms physics uses.

        @param f0        the pitch in hertz
        @param B         the inharmonicity coefficient. A piano runs about 0.00005 in
                         the treble to 0.0006 and beyond in the bass; 0 gives a
                         perfectly harmonic string, which is a guitar rather than a
                         piano. THE BLOCK SOLVES THE CHAIN FROM THIS, because the
                         coefficient that produces a given B depends on the pitch and
                         is nothing a caller should have to know.
        @param decayT60  seconds for the fundamental to fall 60 dB
        @param damping   0 keeps the highs with the lows, 1 kills them fast */
    void setString (float f0In, float B, float decayT60, float damping) noexcept
    {
        const float f0 = std::clamp (f0In, 16.0f, sr * 0.45f);
        d = std::clamp (damping, 0.0f, 0.95f);

        // THE RESONANCES OF A WAVEGUIDE sit where the loop phase is a whole number of
        // turns: partial n needs a loop delay of n*sr/f_n. Put the stiff-string law in
        // for f_n and the whole design falls out as ONE requirement --
        //
        //     D(f_n) = sr / (f0 * sqrt(1 + B*n^2))
        //
        // so the loop must be sr/f0 samples long at the fundamental and SHORTER, by
        // exactly that factor, up at partial n. Dispersion is not decoration here: it
        // IS the inharmonicity, and the amount of delay the chain has to shed between
        // the fundamental and the top partial is the specification.
        const float w0   = 6.28318530718f * f0 / sr;
        // FIT AT THE HIGHEST PARTIAL THAT ACTUALLY EXISTS, not always the twentieth.
        // Above about midi 87 a string has only a handful of partials below Nyquist,
        // and asking for the stretch at a twentieth that is past it makes `need`
        // enormous, drives the coefficient to the rail, and hands the allpass chain
        // more delay than the whole loop has. The line then clamped to its 2-sample
        // floor, the tuning went with it, and the loop gain went over one: measured
        // peaks of 38, 262 and 195 at midi 87, 93 and 105 against 0.35 in the middle
        // of the keyboard. A note that explodes is not a loud note.
        // FIT AT A PARTIAL THAT MEANS SOMETHING FOR THIS REGISTER, not always the
        // twentieth. A bass string has forty or more audible partials and a treble one
        // has six, so a fixed twentieth asks a low note for its whole stretch across a
        // tiny span of frequency - at C1 partial 20 is only 654 Hz - and the solver
        // answers by running the coefficient to the rail. Measured: C1 came out at
        // -0.9625 where C4 sits at -0.6906, and an allpass that close to -1 has fifty
        // samples of group delay at DC falling steeply, which smears the low partials
        // so hard they read as absent. The author heard exactly that register dependence
        // before it was measured: "the C4 is alot closer than the lower ones."
        //
        // Fitting near a fixed FREQUENCY instead keeps the coefficient in its
        // comfortable range everywhere.
        int fitN = std::clamp ((int) (1200.0f / f0), 8, 40);
        while (fitN > 4 && (float) fitN * f0 * std::sqrt (1.0f + B * (float) fitN * (float) fitN) > sr * 0.45f)
            --fitN;
        const float stretchN = std::sqrt (1.0f + B * (float) fitN * (float) fitN);
        const float fTop = (float) fitN * f0 * stretchN;
        const float wTop = 6.28318530718f * std::min (fTop, sr * 0.45f) / sr;
        float need = (sr / f0) * (1.0f - 1.0f / stretchN);
        // AND THE CHAIN MAY NEVER EAT THE LINE. Whatever the dispersion wants, the
        // delay line has to keep enough samples to be a delay line at all.
        need = std::min (need, std::max (0.0f, sr / f0 - 8.0f));
        // HALF PER RAIL, because dispersion on a real string is distributed along it
        // rather than concentrated at one end. Lumped at the bridge, the hammer's two
        // waves pass through different amounts of it and their delay difference becomes
        // frequency dependent, which slides the comb null off the partial it should
        // kill.
        need *= 0.5f;

        // SOLVE FOR THE CHAIN. The drop a stage can provide grows monotonically as the
        // coefficient approaches -1, so a bisection settles it in a few steps of
        // arithmetic - no rendering, no table. Stages are added only when one cannot do
        // the job without running to the rail, because a coefficient past about -0.97
        // puts tens of samples of delay at the fundamental and makes tuning fragile.
        //
        // THIS MATCHES THE TWO ENDS OF THE SERIES, partial 1 and partial `kFitPartial`,
        // and that choice is a known approximation with a measured cost. A cascade of
        // IDENTICAL first-order allpasses has its own dispersion shape and it is not
        // the stiff-string curve, so the middle of the series sits off the law by up to
        // about 30 cents at large B (2.1 cents at B = 5e-5, 30.2 at 6e-4). Fitting one
        // coefficient across all twenty partials instead was tried and is WORSE - it
        // trades the ends for the middle and mistuned the fundamental - because one
        // degree of freedom cannot follow a curve of a different family. The real answer
        // is a cascade with PER-STAGE coefficients designed against the target curve
        // (Rauhala and Valimaki's tunable allpass design); that is its own piece of work
        // and it is what would take the worst partial under 10 cents.
        nStages = 0; float a = 0.0f;
        if (B > 1.0e-7f && need > 1.0e-3f)
        {
            for (int m : { 1, 2, 4, 8 })
            {
                float lo = -0.9995f, hi = -0.0005f;      // lo drops most
                for (int it = 0; it < 48; ++it)
                {
                    const float mid = 0.5f * (lo + hi);
                    const float drop = (float) m * (Allpass1::phaseDelay (mid, w0)
                                                  - Allpass1::phaseDelay (mid, wTop));
                    if (drop > need) lo = mid; else hi = mid;
                }
                a = 0.5f * (lo + hi);
                nStages = m;
                // GENTLER STAGES, MORE OF THEM. This accepted anything short of -0.97,
                // and an allpass that close to the rail has fifty samples of group
                // delay at DC falling steeply - it does not disperse the bass, it
                // smears it, and the low partials come out reading as absent. Spread
                // the same delay drop over more stages and each one stays mild.
                if (a > -0.80f) break;
            }
        }
        for (int i = 0; i < nStages; ++i) { disp[(size_t) i].a = a; dispL[(size_t) i].a = a; }

        // THE LOSS FILTER's gain is solved from the wanted T60 so tone and length do
        // not fight: changing `damping` must not change how long the note rings.
        const float trips = std::max (1.0f, decayT60 * f0);      // round trips in T60
        g = std::pow (10.0f, -3.0f / trips);
        // HOW MUCH MORE THE BOTTOM PAYS. Fitted to the reference: the fundamental has
        // to fall about four times as fast as the top of the note.
        lowLoss = kLowLoss;

        // TUNING AT THE FUNDAMENTAL, not at DC. Everything else in the loop has
        // already taken some delay; the line gets the remainder.
        // both rails carry the same chain, so a round trip pays for it twice
        const float apDly   = 2.0f * (float) nStages * (nStages ? Allpass1::phaseDelay (a, w0) : 0.0f);
        // THE SHAPER'S DELAY IS A FRACTION OF THE FILTER'S, because only a fraction of
        // the filter is used. When the loss became SUBTRACTIVE - `v - lowLoss * lossZ`
        // instead of the lowpass output itself - the signal stopped travelling through
        // the one-pole and started merely having a sliver of it removed, so its phase
        // delay collapsed from onePoleDelay(d) to roughly lowLoss times that. The
        // tuning kept subtracting the whole figure.
        //
        // At damping 0.899 that is nearly nine samples taken out of a line that never
        // gets them back, and the error is proportional to pitch: C4 rendered at
        // 277.28 Hz against 261.63, a WHOLE SEMITONE sharp, and C1 at 33.26 against
        // 32.70. The instrument had been sharp since the decay was inverted, and the
        // string gate never caught it because that gate calls setString directly with
        // its own damping rather than going through keySpec.
        const float lossDly = kLowLoss * onePoleDelay (d, w0);
        // THE RAILS MUST BE THE SAME LENGTH: the upper is the one-way path to the
        // bridge and the lower is the return, so a point at fraction p is index p*len
        // in BOTH. Absorbing a fractional sample into an uneven split destroys the very
        // thing this topology exists to provide, so each rail carries its own tuning
        // allpass and between them they cover the two samples 2*len cannot resolve.
        // NO ALLOWANCE FOR THE TUNING ALLPASSES HERE: their delay is already carried by
        // `frac` below, which is what they are for. Subtracting 2.0 for them as well
        // double-counted it and left the loop exactly two samples short at every pitch
        // - which is a constant error in SAMPLES and therefore a growing one in CENTS:
        // +2.8 at C1, +9.7 at C3, +19.3 at C4, +79.8 at C6. The instrument was sharp
        // everywhere and worse the higher you played.
        float perRail = (sr / f0 - apDly - lossDly) * 0.5f;
        perRail = std::max (3.0f, perRail);
        // SPLIT SO THE FRACTION LANDS IN [0.5, 1.5), NEVER NEAR ZERO. A first-order
        // allpass fractional delay has coefficient (1-frac)/(1+frac), which goes to 1
        // as frac goes to 0 - a pole ON the unit circle, and a loop containing one does
        // not decay, it grows. Taking floor(rest) put frac wherever it happened to fall,
        // so whether a note was stable depended on its pitch: measured peaks of 78 at
        // midi 93 and 6.6e9 at midi 99 while the middle of the keyboard sat at 0.35.
        // Biasing the split keeps the coefficient inside (-0.2, 0.34] for every note.
        len = (int) std::floor (perRail - 0.5f);
        const float frac = perRail - (float) len;        // in [0.5, 1.5)
        tune.a = tuneL.a = (1.0f - frac) / (1.0f + frac);
        len = std::clamp (len, 4, (int) upper.size());
        nInt = 2 * len;                                  // the round trip, for the outside
        siteScale = (float) nInt / 200.0f;               // see the hammer's injection
    }

    /** The partial the dispersion is fitted at. 20 is far enough up that the stretch
        is unmistakable and low enough to still carry energy on a real note. */
    static constexpr int kFitPartial = 20;

    /** A PLUCK: release the string from a displaced position. Kept because it is the
        control the hammer is judged against, and because it is what a harpsichord
        does. The author, on milestone 1: "that sounds like an acoustic guitar mixed with a
        harpsichord" - which was exactly right, and this is the half he was hearing. */
    void pluck (float amplitude, float strikePos = 0.125f) noexcept
    {
        // A PLUCK IS AN INITIAL DISPLACEMENT with its corner at the pluck point, and
        // on a two-rail string the displacement at a point is the SUM of the two waves
        // there - so each rail gets half the shape, and as they run apart the corner
        // splits into the two travelling halves a released string really has.
        const float pp = std::clamp (strikePos, 0.02f, 0.5f);
        for (int i = 0; i < len; ++i)
        {
            const float x = ((float) i + 0.5f) / (float) len;   // distance from the bridge
            const float shape = x < pp ? x / pp : (1.0f - x) / (1.0f - pp);
            const float half = 0.5f * amplitude * shape;
            upper[(size_t) ((ui + i) % len)] += half;
            lower[(size_t) ((li + (len - 1 - i)) % len)] += half;
        }
    }

    /** THE HAMMER, WHICH IS WHAT MAKES IT A PIANO RATHER THAN A PLUCKED STRING.
        Call it once at note-on; `process` then drives the collision until the string
        pushes the hammer off, which takes a millisecond or two of real time.

        A PLUCK AND A STRIKE ARE NOT THE SAME GESTURE. A pluck releases the string from
        a corner, so every partial is excited at once and velocity only scales the lot.
        A hammer arrives with mass and momentum, compresses against the string through a
        felt that gets STIFFER the harder it is squeezed, and leaves when the string
        throws it off. Two consequences, and both are the sound:

          * HOW LONG IT TOUCHES sets which partials get any energy. Contact lasting T
            cannot excite much above about 1/T, so a soft blow is dark because it is
            SLOW, not because something filtered it.
          * HIT HARDER AND THE CONTACT SHORTENS, because the felt stiffens under load.
            So the spectrum opens up with velocity. That is the thing an envelope on a
            VCA can never do and the reason a loud piano note is a different sound
            rather than the same sound louder.

        The felt is the standard power law, force = K * compression^p with p around 2.5
        to 3 for a real hammer [Hall, Askenfelt]. Below about 2 the blow is too soft to
        brighten with velocity; that is measurable and the gate checks it. */
    void strike (float velocity, float strikePos = 0.125f) noexcept
    {
        hVel  = std::clamp (velocity, 0.01f, 1.0f) * kHammerSpeed;
        hPos  = 0.0f;                       // at the string's surface, moving into it
        hOn   = true;
        // A FRACTIONAL POSITION, and it is the difference between a notch and a
        // shoulder. The null sits where the two waves are 2*(len - x) apart, so it
        // lands exactly on partial n only when x is exactly len/n - and len/8 is rarely
        // a whole number. Rounding it put the null at partial 7.96 and left partial 8
        // sitting beside its neighbours instead of under them.
        hPosX = std::clamp (std::clamp (strikePos, 0.02f, 0.5f) * (float) len,
                            1.0f, (float) len - 2.0f);
        hSite = (int) hPosX;
    }

    /** Felt stiffness. Higher is a harder hammer: brighter everywhere, and it reaches
        its brightest at a lower velocity. */
    void setHammer (float hardness, float massGrams = 9.0f) noexcept
    {
        // THE STIFFNESS IS NOT A FREE CONSTANT, it is set by the contact time the
        // instrument has to produce. Impulse over the blow is 2*m*v, so the mean force
        // is 2*m*v/T and the felt compresses by about v*T/pi; put a real hammer in
        // (9 g, 1 to 4 m/s, 1 to 4 ms of contact) and K comes out between 1e7 and 1e9.
        // The first version used 2.0e5, which is a factor of ~500 short, so the string
        // could never throw the hammer off: contact ran 14 to 31 ms and the note was
        // inaudible. The gate passed anyway, because it only asked whether contact time
        // FELL rather than whether it landed where a piano's does. It asks both now.
        // THE STIFFNESS IS SET BY THE SPECTRUM, NOT BY A CONTACT-TIME ESTIMATE, and
        // that correction came from a real Steinway. The first value, 1.1e8, was derived
        // from impulse = 2*m*v over a 1-4 ms contact and it gave contacts in that range -
        // but a 2 ms force pulse on a 4.5 ms period is a lowpass at about partial 2, so
        // the note came out as a fundamental and a third and nothing else. Measured
        // against Piano.mf.A3, our partials 4 to 13 sat 35 to 55 dB BELOW the real
        // instrument's. That is what six milestones of "not quite a piano" was.
        //
        // Sweeping the collision against the Steinway's own partial levels, the mean
        // error over partials 1-13 falls from 22.1 dB at a 2.0 ms contact to 10.1 dB at
        // 1.1 ms, where partial 4 reads -4 dB against the real -6. So the felt is much
        // stiffer than the contact-time argument alone suggested, and the spectrum is
        // the better constraint: it is what a listener hears.
        hK    = 1.8e9f * std::pow (10.0f, 1.2f * std::clamp (hardness, 0.0f, 1.0f) - 0.6f);
        hMass = std::max (1.0f, massGrams) * 0.001f;
    }

    inline float process (float bridgeIn = 0.0f) noexcept
    {
        computeLoop();
        return commitLoop (bridgeIn);
    }

    /** THE WAVE ARRIVING AT THE BRIDGE this sample, before anything is done with it.
        Split out so several strings can be scattered against each other: a junction
        needs every string's incoming wave before it can decide any of their outgoing
        ones, which one call per string cannot give it. */
    inline float computeLoop() noexcept
    {
        // ---- THE COLLISION, resolved before the string moves on --------------------
        //
        // The hammer and the string push on each other, so this is an interaction and
        // not an injection: the felt force goes INTO the string, and the string's own
        // motion at the contact point is what decelerates the hammer and eventually
        // throws it off. Modelling it as "a filtered impulse whose brightness scales
        // with velocity" would reproduce the headline and none of the behaviour - the
        // contact time has to FALL OUT of the stiffening felt, or velocity and timbre
        // are two knobs instead of one thing.
        if (hOn)
        {
            // THE SAME POINT IN BOTH RAILS, interpolated. Upper counts from the
            // bridge, lower runs the other way, so the lower index is mirrored: get
            // that wrong and the hammer drives two different places and no notch can
            // form anywhere. Displacement at a point is the SUM of the two waves there.
            const int   i0 = (int) hPosX;
            const float fx = hPosX - (float) i0;
            const int uA = (ui + i0) % len,            uB = (ui + i0 + 1) % len;
            const int lA = (li + (len - i0)) % len,    lB = (li + (len - i0 - 1)) % len;
            const float yStr = (1.0f - fx) * (upper[(size_t) uA] + lower[(size_t) lA])
                             +         fx  * (upper[(size_t) uB] + lower[(size_t) lB]);
            const float comp = hPos - yStr;                // how far the felt is squeezed
            if (comp <= 0.0f && hVel <= 0.0f) { hOn = false; }
            else
            {
                // FORCE = K * compression^p, and it is ZERO once they separate. Felt
                // pushes, it never pulls, and forgetting that glues the hammer to the
                // string and turns a piano into a bowed instrument.
                // FELT COMPRESSES BY MILLIMETRES, NOT BY METRES, and the limit has to
                // be stated or the loop runs away. On a short string the wave the
                // hammer just injected comes back under the felt several times during
                // one contact - at midi 93 the loop is 22 samples and the blow lasts
                // 96 - and a returning trough deepens `comp`, which raises a force that
                // goes as the 2.5 power, which injects more. The string alone is stable
                // at every pitch (plucked peaks 0.44 to 0.59 across the keyboard); it
                // was the collision that exploded, 1703 at midi 85 and 1336 at 93.
                // A real hammer's felt bottoms out around a millimetre or two.
                const float cc = std::min (comp, kMaxCompression);
                const float f = cc > 0.0f ? hK * std::pow (cc, kFeltExp) : 0.0f;
                hVel -= f / hMass / sr;                    // the string slows the hammer
                hPos += hVel / sr;
                // SCALED BY THE STRING'S OWN IMPEDANCE, which is what nInt stands for here. The
                // same force must not put the same displacement into a 15-sample loop as
                // into a 200-sample one: a short string is stiffer, and a hammer that
                // ignores that pumps a treble note six times per contact. Measured before
                // this line existed: peaks of 952 at midi 87 and 6.6e9 at midi 99, against
                // 0.35 across the middle of the keyboard.
                // AND IT DRIVES BOTH RAILS, WITH THE SAME SIGN. This is the line the
                // whole topology exists for: one force at one point, two waves, and the
                // notch falls out of where they meet rather than being drawn on.
                const float drive = f * coupling * siteScale / sr;
                upper[(size_t) uA] += drive * (1.0f - fx);
                upper[(size_t) uB] += drive * fx;
                lower[(size_t) lA] += drive * (1.0f - fx);
                lower[(size_t) lB] += drive * fx;
                if (comp <= 0.0f && hVel < 0.0f) hOn = false;
            }
        }

        pendingOut = upper[(size_t) ui];
        float v = pendingOut;
        for (int i = 0; i < nStages; ++i) v = disp[(size_t) i].process (v);
        v = tune.process (v);
        // THE LOSS IS SUBTRACTIVE, AND THIS ONE LINE WAS THREE SYMPTOMS.
        //
        // It read `pendingFed = g * lossZ`, putting a one-pole LOWPASS in the loop and
        // applying it on every round trip. At C2 that is 29 passes in half a second: a
        // bare plucked string lost 54.6 dB at 1 kHz with the damping as set, and 0.2 dB
        // with it at zero. That is the whole of what the author heard as "a towel laying on
        // the strings", the whole of why nothing survived above partial 8, and the
        // whole of the 96 dB mid-band loss I could not account for - which survived
        // ruling out the dispersion, the unison, the loop gain and the topology,
        // because none of them was ever the cause.
        //
        // A REAL NOTE LOSES ITS BOTTOM FIRST, not its top. Measured on the reference
        // Steinway's C2 between 0.2 s and 2.0 s: the fundamental falls 16.8 dB, the low
        // mids 9.4, the mids 9.2, the highs 4.4. The bridge couples the low end to the
        // board efficiently and that is where the energy goes. So the lowpass output is
        // SUBTRACTED rather than passed: the bottom pays `lowLoss` per trip and the top
        // keeps the full loop gain, and `damping` is now the CORNER of that shaper
        // rather than its strength. It stays passive - g at the top, g*(1 - lowLoss) at
        // DC, both under one.
        lossZ = (1.0f - d) * v + d * lossZ;
        pendingFed = g * (v - lowLoss * lossZ);
        return pendingFed;
    }

    /** Send the wave back in, less whatever the junction took. */
    inline float commitLoop (float scatter) noexcept
    {
        // THE AGRAFFE is a clamp: it inverts and keeps everything, less its share of
        // the dispersion. THE BRIDGE inverts too, having already paid the loss and the
        // dispersion on the way in and given the junction its share.
        float aw = lower[(size_t) li];
        for (int i = 0; i < nStages; ++i) aw = dispL[(size_t) i].process (aw);
        const float atAgraffe = tuneL.process (aw);
        upper[(size_t) ui] = -atAgraffe;
        lower[(size_t) li] = -(pendingFed - scatter);
        ui = (ui + 1) % len;
        li = (li + 1) % len;
        return pendingOut;
    }

    /** HOW HARD THE FELT'S FORCE DRIVES THE STRING. Separated from the collision so
        the two can be measured against each other: raise it far enough and the string's
        own motion starts terminating the contact, which is the real coupling, and the
        contact time stops being the felt's alone. */
    void setCoupling (float c) noexcept { coupling = std::max (0.0f, c); }

    /** True while the felt is still touching. The gate reads this to report contact
        time, which is the number the whole hammer model exists to produce. */
    [[nodiscard]] bool inContact() const noexcept { return hOn; }

    [[nodiscard]] int stages() const noexcept { return nStages; }
    [[nodiscard]] float coeff() const noexcept { return nStages ? disp[0].a : 0.0f; }
    [[nodiscard]] int   lineLen() const noexcept { return nInt; }

private:
    // THE FELT'S EXPONENT. 2.5 is mid-range for a real hammer; below about 2 the blow
    // stops brightening with velocity, which is the failure the gate is written to
    // catch. [Hall; Askenfelt and Jansson]
    static constexpr float kFeltExp    = 2.5f;
    static constexpr float kMaxCompression = 0.0025f;  ///< 2.5 mm, where felt bottoms out
    static constexpr float kHammerSpeed = 4.0f;   ///< metres per second at full velocity
        // FORCE TO STRING DISPLACEMENT. Fitted, and the only fitted number in the hammer:
    // everything else comes from the felt law and the hammer's mass. 400 is where the
    // note reaches a usable level (peak 0.62 at full velocity) while the contact times
    // stay where a piano's are. It is also where the string starts to answer back - the
    // soft-blow contact moves 5.19 ms to 4.81 ms between coupling 0.02 and 400 - so the
    // blow is no longer the felt spring alone.
    static constexpr float kForceScale  = 400.0f;

    std::vector<float> upper, lower;          ///< the two travelling waves
    Allpass1 disp[kMaxDispersion], dispL[kMaxDispersion];
    Allpass1 tune, tuneL;
    static constexpr float kLowLoss = 0.006f;
    float sr = 48000.0f, g = 0.999f, d = 0.3f, lossZ = 0.0f, lowLoss = kLowLoss;
    int   ui = 0, li = 0, len = 64, nInt = 100, nStages = 0;
    // the hammer
    float hPos = 0.0f, hVel = 0.0f, hK = 1.1e8f, hMass = 0.009f, coupling = kForceScale;
    float pendingOut = 0.0f, pendingFed = 0.0f, siteScale = 1.0f;
    int   hSite = 1;
    float hPosX = 1.0f;
    bool  hOn = false;
};


/** A NOTE: the two or three strings a piano actually puts under one key, coupled
    through a bridge.

    HOW MANY STRINGS IS A PROPERTY OF THE REGISTER, not of the instrument. A real
    piano runs ONE thick wound string in the low bass, TWO through the tenor, and
    THREE for most of the keyboard. The author asked whether it was two; it is 1, 2 or 3, and
    `stringsForPitch` below is where that lives.

    WHY COUPLING IS THE WHOLE POINT, AND WHY A SINGLE STRING CANNOT SOUND LIKE A PIANO.
    The strings of a unison are tuned a couple of cents APART on purpose - tune them
    identical and the note comes out short and dead. Because they are slightly apart
    they start in phase and drift. In phase, they push the bridge together and dump
    energy into it fast: that is the prompt, loud opening of the note. As they drift
    out of phase their forces at the bridge start cancelling, the note stops driving
    the bridge, and what is left rings on quietly for a very long time. Fast fall, long
    tail, from the coupling alone.

    So the famous two-stage piano decay is not two envelopes added together, and
    writing it that way is exactly what makes a synthesised piano sound synthetic. It
    is an emergent property of several detuned strings sharing one termination, and it
    is why milestone 1 and 2 still decayed like a guitar: one string can only give one
    exponential. The author, twice: "sounds like an acoustic guitar mixed with a harpsichord",
    then "still sounds more like a guitar than a piano." */
class Note
{
public:
    static constexpr int kMaxStrings = 3;

    /** What a real piano puts under this pitch. [MAN] standard scaling practice. */
    static int stringsForPitch (float f0) noexcept
    { return f0 < 60.0f ? 1 : f0 < 130.0f ? 2 : 3; }

    void prepare (double sampleRate, float lowestHz = 20.0f)
    {
        // NOTE WHAT IS *NOT* RESET HERE. `bridgeZ` is the bridge's IMPEDANCE, a
        // property of the instrument, not a filter state - it started life as a state
        // variable and kept its zeroing through the rewrite, which silently set every
        // prepared Note to a zero-impedance bridge. A zero-impedance junction is
        // lossless, so the prompt decay vanished and all four gate cases read an
        // identical 1.03x while a hand-run sweep that called setCoupling afterwards
        // read 6.82x. The junction has no state at all; there is nothing to clear.
        for (auto& s : str) s.prepare (sampleRate, lowestHz);
    }

    /** @param detuneCents  how far apart the unison is spread. Real tuners leave a
                            couple of cents; 0 collapses the note to one string's
                            decay, which the gate checks, because a unison that is
                            not detuned has nothing to beat against. */
    void setNote (float f0, float B, float decayT60, float damping,
                  float detuneCents = 2.0f, int strings = -1) noexcept
    {
        n = strings > 0 ? std::clamp (strings, 1, kMaxStrings) : stringsForPitch (f0);
        for (int i = 0; i < n; ++i)
        {
            // spread symmetrically about the nominal pitch, so the note's PITCH is
            // still f0 however many strings it has
            const float off = (n == 1) ? 0.0f
                            : detuneCents * ((float) i / (float) (n - 1) - 0.5f) * 2.0f;
            str[(size_t) i].setString (f0 * std::pow (2.0f, off / 1200.0f), B, decayT60, damping);
        }
    }

    void setHammer (float hardness, float massGrams = 9.0f) noexcept
    { for (int i = 0; i < n; ++i) str[(size_t) i].setHammer (hardness, massGrams); }

    /** ONE HAMMER ACROSS THE WHOLE UNISON, because there is one. The strings are not
        struck at exactly the same instant on a real piano and that unevenness is part
        of the sound, so each is offset by a fraction of a sample's worth of velocity. */
    void strike (float velocity, float strikePos = 0.125f) noexcept
    { for (int i = 0; i < n; ++i)
          str[(size_t) i].strike (velocity * (1.0f - 0.02f * (float) i), strikePos); }

    void pluck (float amplitude, float strikePos = 0.125f) noexcept
    { for (int i = 0; i < n; ++i) str[(size_t) i].pluck (amplitude, strikePos); }

    /** HOW STRONGLY THE STRINGS SHARE A TERMINATION. 0 is three independent strings,
        which sounds like a chorus rather than a note. */
    void setCoupling (float bridgeImpedance) noexcept
    { bridgeZ = std::clamp (bridgeImpedance, 0.5f, 5000.0f); }

    inline float process() noexcept
    {
        // THE BRIDGE IS ONE OBJECT AND EVERY STRING SEES THE SAME ONE. That is the
        // whole mechanism: what comes back to each string is the sum of ALL of them,
        // so their phases decide whether the note drives the bridge or cancels at it.
        // In phase they move it together and lose energy into it fast; drifted apart
        // their forces cancel there and the note stops paying.
        //
        // THE FEEDBACK MUST BE INSTANTANEOUS AND NEGATIVE, AND THAT IS NOT A DETAIL.
        // The first version put a one-pole "bridge mass" in the path and fed back
        // -k*bridgeZ. A pole has phase, so at some frequencies that term stopped
        // opposing the strings and started driving them: every multi-string note grew
        // instead of decaying, reaching +694 dB and then NaN, and even a single string
        // inside the Note rose from -64 dB to -1 dB over six seconds. A bridge is a
        // LOAD. It can only ever take energy out, so the term that represents it has
        // to oppose the sum at every frequency, which means no state in the path.
        //
        // The scaling is the other half: the in-phase mode sees k*n of extra loss, so
        // k*n must stay well under 1 or the mode is over-damped into a click. n*k at
        // the default is 0.09.
        // A SCATTERING JUNCTION, which is the only formulation that cannot blow up.
        //
        //     v = 2 * sum(in) / (n + Z)        the bridge's own motion
        //     out_i = in_i - v                 what each string gets back
        //
        // With every string in phase this takes energy out: n equal waves of a come
        // back as a*(Z-n)/(Z+n), smaller than a for any Z. With them out of phase the
        // sum is zero, the bridge does not move, and nothing is taken. That IS the two
        // stage decay - fast while they agree, slow once they have drifted - and it
        // falls out of one line rather than being drawn as two envelopes.
        //
        // AND |out| <= |in| ALWAYS, because |Z-n| <= Z+n for positive Z and n. That is
        // what the two earlier attempts lacked. Adding -k*sum at the head looks like a
        // load and is not one: the injection travels a whole loop before it meets the
        // wave it was meant to oppose, so wherever the loop phase inverts it drives
        // the string instead. Every three-string note grew to +694 dB and then NaN,
        // and with a one-pole "bridge mass" in the path even a single string rose from
        // -64 dB to -1 dB. A bridge is a load. The arithmetic has to make that true at
        // every frequency, not on average.
        float sum = 0.0f;
        for (int i = 0; i < n; ++i) { fedBuf[(size_t) i] = str[(size_t) i].computeLoop(); sum += fedBuf[(size_t) i]; }
        const float v = 2.0f * sum / ((float) n + bridgeZ);
        float out = 0.0f;
        for (int i = 0; i < n; ++i) out += str[(size_t) i].commitLoop (v);
        return out;
    }

    [[nodiscard]] bool inContact() const noexcept
    { for (int i = 0; i < n; ++i) if (str[(size_t) i].inContact()) return true; return false; }
    [[nodiscard]] int strings() const noexcept { return n; }

private:
    String str[kMaxStrings];
    /** THE BRIDGE'S IMPEDANCE, relative to one string's, and it has to be HIGH.
        A real bridge is far stiffer than the wire crossing it, so it takes only a
        little energy per bounce. The first default was 6, which couples the strings so
        hard that they MODE-LOCK: three strings detuned by 12 cents produced an
        envelope identical to three tuned the same, because the junction was dragging
        them into step faster than they could drift apart. Measured over 60 to 400 the
        prompt decay becomes 6 to 7 times steeper than the aftersound, which is the
        two-stage decay appearing. Below about 20 it does not exist at all. */
    // SET BY TWO MEASUREMENTS AT ONCE, which is why it is not simply "as high as
    // possible". A stiffer bridge fits the reference Steinway's decay CURVE better -
    // mean error over six points falls from 18.7 dB at 120 to 3.6 at 600 - but it also
    // flattens the two-stage structure, and the real instrument's prompt/aftersound
    // ratio is 6.02x. At 1200 the curve fitted well and the ratio collapsed to 1.86x,
    // which is a good average achieved by losing the thing that makes it a piano.
    // 500 sits where both land: the curve near its best and the ratio on the real
    // number.
    float  bridgeZ = 500.0f;
    float  fedBuf[kMaxStrings] = { 0.0f, 0.0f, 0.0f };
    int    n = 3;
};


/** THE SOUNDBOARD, and it is not a reverb.

    The author, looking for a grand: "remember a piano needs reverb." True, and a hall was put
    under the last listening set. But a hall is the space AROUND the instrument and a
    soundboard is part OF it: a big ribbed spruce plate that every string is bolted to
    through the bridge, with its own resonances, colouring the note before any room
    hears it. A grand's tone is mostly that plate. Reverb on an uncoloured string is a
    string in a nice room, which is exactly what the dry-versus-hall pair showed.

    TWO PROPERTIES SEPARATE A PLATE FROM A ROOM and both are gated below:

      * IT RINGS BRIEFLY. A soundboard's own decay is a couple of hundred
        milliseconds, not seconds. Anything longer is a reverb wearing a plate's name,
        and it smears the attack that milestone 2 spent its whole effort sharpening.
      * IT RADIATES BADLY AT THE BOTTOM. A plate is a poor radiator below its first
        modes, which is why a piano's lowest notes are carried by their upper partials
        and why a short grand has no bottom. A model without this has a bass that is
        far too easy.

    MODAL, BECAUSE A PLATE IS ITS MODES. A rectangular plate's modes go as
    (m/a)^2 + (n/b)^2, so unlike a string they are NOT evenly spaced and their density
    stays roughly constant with frequency rather than thinning. That irregularity is
    what makes the colouration sound like wood instead of like a comb filter, so the
    spacing here is deliberately uneven. */
class Soundboard
{
public:
    // ENOUGH MODES THAT NO NOTE FALLS BETWEEN THEM. At 40 the board's response has
    // gaps a semitone wide, so whether a note is loud depends on whether it happens to
    // land on a mode: the keyboard measured a 46 dB raw spread with a JAGGED shape that
    // no smooth level curve could flatten - a quadratic fit left 19.9 dB of residual.
    // A real board has hundreds of modes and a real Steinway spans 6.9 dB across the
    // whole keyboard at one dynamic, measured off the reference recordings. More modes
    // is the physical answer; a bigger correction curve is not.
    static constexpr int kModes = 140;

    void prepare (double sampleRate)
    {
        sr = (float) sampleRate;
        // MODES FROM 55 Hz TO 5 kHz, spread on a curve rather than a grid and jittered
        // by a fixed irregular sequence. Evenly spaced modes comb; a real plate does
        // not, and the ear knows the difference immediately.
        // MODES SPACED THE WAY A PLATE'S ARE, which is NOT logarithmically.
        //
        // A rectangular plate's modes go as (m/a)^2 + (n/b)^2, and counting them below
        // a frequency gives a count that rises LINEARLY - so the density is roughly
        // constant in hertz: sparse at the bottom, crowding as you climb. Log spacing,
        // which is what this had, puts the same twelve per octave everywhere. That is
        // backwards at both ends, and it is audible at both:
        //
        //   * TOO MANY LOW MODES, sitting right where the fundamentals of the notes
        //     anybody plays are, so the board combed them. The author: "muting, like if you
        //     palmed a guitar."
        //   * TOO FEW HIGH MODES, so above about 2 kHz the board has nothing to say
        //     and the sound thins out into something with no body. The author: "ours sounds
        //     way way more digital. The Steinway has a more hollow/woody sound."
        //
        // Linear spacing gives few modes under 200 Hz, where a plate really does have
        // few, and hundreds packed above 2 kHz, where the wood's texture lives.
        unsigned rng = 0x2545F491u;
        const float fLo = 70.0f, fHi = std::min (7000.0f, sr * 0.42f);
        const float step = (fHi - fLo) / (float) (kModes - 1);
        for (int i = 0; i < kModes; ++i)
        {
            rng = rng * 1664525u + 1013904223u;
            // a plate's modes are irregular, not a grid: an even comb rings like one
            const float jitter = (float) (rng >> 16) / 65536.0f - 0.5f;
            const float f = fLo + step * ((float) i + 0.8f * jitter);
            const float t = (float) i / (float) (kModes - 1);
            // Q RISES THEN FALLS: the low modes are well defined, the high ones damped
            // by the ribs and the bridge, which is what stops a plate sounding like a
            // cymbal.
            const float q = 3.0f + 9.0f * std::sin (3.14159265f * t);
            // RADIATION EFFICIENCY RISES FROM THE BOTTOM: a plate cannot push air at
            // long wavelengths, which is why a piano's bottom octave is carried by its
            // upper partials and why a short grand has no bass.
            const float radiates = std::min (1.0f, f / 180.0f);
            setMode (i, std::clamp (f, 30.0f, sr * 0.45f), q,
                     0.9f * radiates * (0.45f + 0.55f * (1.0f - t)));
        }
        reset();
    }

    void reset() noexcept
    { for (auto& m : mode) { m.z1 = m.z2 = 0.0f; } hp1 = hp2 = 0.0f; }

    /** How much plate is in the sound. 0 is the bare strings. */
    void setAmount (float a) noexcept { amount = std::clamp (a, 0.0f, 1.0f); }

    inline float process (float x) noexcept
    {
        float sum = 0.0f;
        for (auto& m : mode)
        {
            // one resonator, direct form 2 transposed
            const float y = m.b0 * x + m.z1;
            m.z1 = m.a1 * y + m.z2;
            m.z2 = m.a2 * y;
            sum += y * m.gain;
        }
        // THE RADIATION HIGH-PASS. A plate cannot push air at long wavelengths, so
        // this is the model's bass loss and it is second order because the rolloff is
        // steep. Without it the low notes come out with a fundamental no piano has.
        const float hp = sum - hp1;
        hp1 += kRadA * hp;
        const float hp2o = hp - hp2;
        hp2 += kRadA * hp2o;
        // NORMALISED BY THE MODE COUNT, or the blend is not a blend. 140 resonators
        // summing gave the board something like a hundredfold gain, so even at
        // `amount` 0.15 the modal path buried the dry signal completely and the mix
        // control did nothing measurable - sweeping it from 0.85 to 0.15 moved the
        // ripple by 0.0 dB. The board was not colouring the note, it WAS the note, and
        // its 21 dB of ripple across A3's own partials was the whole sound.
        //
        // The author heard it as the thing that is not damping: "muting, like if you palmed a
        // guitar." That is exactly what a comb with 21 dB teeth does - it does not roll
        // the top off, it gouges holes in particular partials, and partial 2 came out
        // 22 dB under the Steinway's while partial 5 came out 11 dB over.
        const float wet = hp2o * kBoardNorm;
        return x * (1.0f - amount) + wet * amount;
    }

private:
    struct Mode { float b0 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0, gain = 0; };

    void setMode (int i, float f, float q, float gain) noexcept
    {
        const float w = 6.28318530718f * f / sr;
        const float r = std::exp (-w / (2.0f * q));
        Mode& m = mode[(size_t) i];
        m.a1 = 2.0f * r * std::cos (w);
        m.a2 = -r * r;
        m.b0 = (1.0f - r);                    // unit-ish peak gain
        m.gain = gain;
    }

    static constexpr float kRadA = 0.016f;    ///< about 122 Hz, twice
    /** So the summed modes leave at about unity and `amount` means something. */
    static constexpr float kBoardNorm = 2.6f / (float) kModes;
    Mode  mode[kModes];
    float sr = 48000.0f, amount = 0.80f, hp1 = 0.0f, hp2 = 0.0f;
};


/** WHAT A GIVEN KEY IS, across all 88 of them.

    A piano is not one instrument transposed. It is 88 of them: single thick wound
    strings at the bottom, three bright plain ones at the top, and everything about
    them changes continuously in between. Until this exists the model is a physics
    demo with hand-picked numbers, which is what milestones 1 to 4 were.

    THE INHARMONICITY CURVE IS U-SHAPED, and that surprises everyone who assumes
    stiffness simply rises with pitch. B is LOWEST in the tenor, around 5e-5, and
    climbs at BOTH ends: in the bass because the strings must be wound and short for
    their pitch rather than the many metres a plain wire would need, and in the top
    octave because the strings get very short relative to their diameter. Conklin's
    measurements put it near 8e-4 at the bottom and back up around 1e-3 at the top.
    A model with B simply falling gets the treble wrong in a way that sounds dull.

    Everything here is [COR] - fitted to published scaling practice and to the shape of
    Conklin's curve, not measured off one instrument. Replacing any of it with numbers
    taken from a real piano is an improvement, and the shape is what matters. */
struct KeySpec
{
    float f0, B, decayT60, damping, detuneCents, hardness, strikePos;
    /** WHAT THE KEY IS WORTH AT THE OUTPUT. A real piano is balanced by its scaling -
        hammer mass, string mass and speaking length all change together - and this
        model is not, because its hammer is one mass and its strings one material. Left
        alone the keyboard measured a 38 dB spread, 0.50 peak at A0 against 0.006 at
        C8, which is an instrument you cannot play. Fitted to flatten that. */
    float level;
    int   strings;
};

[[nodiscard]] inline KeySpec keySpec (int midiNote) noexcept
{
    const int  n  = std::clamp (midiNote, 21, 108);          // A0 to C8
    const float f = 440.0f * std::pow (2.0f, (float) (n - 69) / 12.0f);
    const float t = (float) (n - 21) / 87.0f;                // 0 at A0, 1 at C8

    KeySpec k {};
    k.f0 = f;

    // INHARMONICITY, AND THIS CURVE IS NOW MEASURED RATHER THAN DESCRIBED.
    //
    // The first version was a U with its minimum near the tenor at 5e-5, climbing hard
    // to both ends - written from the usual description of piano scaling. A real
    // Steinway says otherwise. Measured off the University of Iowa recordings with
    // `probes/reference_steinway.py`:
    //
    //     A1  B = 0.000133      (ours had 0.000335, 2.5x too high)
    //     A2  B = 0.000127      (ours had 0.000091)
    //     A3  B = 0.000226      (ours had 0.000063, 3.6x too low)
    //     A4  B = 0.000123      (ours had 0.000217)
    //
    // Four octaves, essentially FLAT around 1.3e-4, which is not a U at all across the
    // part of the keyboard anyone plays. That makes sense of the scaling: a piano
    // builder chooses string length, diameter and tension together precisely to hold
    // inharmonicity roughly constant where it would be heard as out of tune.
    //
    // ABOVE A4 WE CANNOT MEASURE IT AND THE CURVE SAYS SO. A treble note has five to
    // seven partials below Nyquist and is gone in about a second, so the fit has almost
    // nothing to work with: A5 read 0.000021 on a 1.5 s window and 0.001803 on a 0.6 s
    // one, a factor of ninety, which is a measurement failing rather than a string
    // changing. The rise in the top octave is [MAN] from published scaling (Conklin),
    // not [COR] from our own recordings, and replacing it with real numbers from a
    // treble recording that sustains is an open improvement.
    const float bassLift = (t < 0.12f) ? (0.12f - t) / 0.12f : 0.0f;
    const float trebLift = (t > 0.74f) ? (t - 0.74f) / 0.26f : 0.0f;
    k.B = 1.30e-4f                       // the measured plateau
        + 1.1e-4f * bassLift * bassLift  // the wound bottom, where B does climb
        + 8.5e-4f * trebLift * trebLift; // [MAN] the top octave, published not measured

    // HOW LONG IT RINGS. A bass note sustains for the best part of a minute; the top
    // octave is gone in a second or two. Roughly an inverse power of frequency.
    // HOW LONG IT RINGS, AND THIS NUMBER IS A LOOP TIME RATHER THAN A T60. The loop
    // gain is solved from it as if the string's own loss were the only one, but the
    // damping filter and the bridge junction each take energy on top, so the note that
    // comes out decays far faster than the figure asks. Measured against the reference
    // Steinway's A3 mf, the note was 26.4 dB down at half a second where the real one
    // is 13.1. Sweeping the loop time and the bridge impedance together against the
    // real decay curve brings the mean error over six points from 18.7 dB to 4.1.
    //
    // Solving the gain for the WHOLE loss instead would let this be a true T60 again,
    // and is the honest version of this line. Until then the factor is what the
    // measurement asked for and the name is a lie the comment corrects.
    // AND THE x8 IS GONE. It was added to compensate for losses that the damping was
    // taking, and once the damping was cut to 0.02 nothing compensated anything: the
    // note fell 3.5 dB between 0.2 s and 2.0 s where a real one falls 16.8. A note
    // that does not decay is most of what "digital" means.
    k.decayT60 = std::clamp (3.0f * 2200.0f / std::pow (f, 0.92f), 0.9f, 400.0f);

    // and the highs die before the lows within every note, harder as the note climbs
    // AND THE HIGHS DIE BEFORE THE LOWS - BUT FAR LESS THAN THIS FIRST SAID.
    //
    // 0.12 + 0.22t put A3 at 0.21, and at 0.21 the string has nothing left above
    // partial 8 after 150 ms. A real Steinway has real content out past 13. The author heard
    // it exactly: "it sounds like a towel laying on the strings."
    //
    // I HAD RULED DAMPING OUT TWICE AND BOTH TIMES THE MEASUREMENT WAS AT FAULT. The
    // first sweep ran while the hammer was still the bottleneck, so nothing downstream
    // could show an effect. The second read the spectrum at PREDICTED partial positions
    // - and this model's real stretch exceeds the B it is asked for, so partial 7 sits
    // at n=7.05 and partial 8 at n=8.07, outside a 1.5 per cent window. It was
    // measuring the gaps BETWEEN partials and reporting them as the partials. Finding
    // the peaks instead: the highest partial carrying energy goes 8.1 at damping 0.21,
    // 13.3 at 0.10, 15.3 at 0.04 and 19.5 at 0.
    //
    // The lesson is the one that keeps costing here: measure where the energy IS, never
    // where the model says it should be. A wrong ruler exonerates the guilty parameter.
    // `damping` is now the CORNER of the loss shaper rather than a lowpass strength:
    // it sets how far up the note the extra loss reaches. High, because the extra loss
    // belongs to the fundamental and the low mids and not to the top.
    k.damping = std::clamp (0.90f - 0.02f * t, 0.50f, 0.999f);

    // ONE, TWO OR THREE STRINGS, which is a fact about the instrument rather than a
    // choice: wound singles at the bottom, pairs through the tenor, threes above.
    k.strings = f < 62.0f ? 1 : f < 140.0f ? 2 : 3;

    // Tuners leave a little more spread in the bass, where beating is slow and wanted,
    // and less in the treble where it would just sound out of tune.
    k.detuneCents = 3.4f - 2.0f * t;

    // HARDER FELT AS YOU GO UP. Hammers are voiced that way because a short bright
    // string needs less help and a long dull one needs more.
    k.hardness = std::clamp (0.28f + 0.55f * t, 0.0f, 1.0f);

    // THE STRIKE POINT MOVES toward the end of the string in the treble. It notches
    // its own harmonic, so this is audible rather than a detail.
    k.strikePos = 0.125f - 0.045f * t;

    // FITTED AGAINST THE MEASURED KEYBOARD, and re-fitted when the inharmonicity curve
    // changed, because B changes a note's brightness and therefore its peak. At 0.82,
    // which suited the old curve, the corrected one spanned 22.8 dB; 1.10 brings it to
    // 18 and holds the gate's 20 dB bound honestly rather than by moving it.
    // FITTED AGAINST THE MEASURED KEYBOARD, as a quadratic in log-frequency rather
    // than a single power law, because the raw response is not a straight line: it is
    // nearly flat to middle C and then falls away steeply.
    // FITTED AGAINST THE MEASURED KEYBOARD, as a quadratic in log-frequency rather
    // than a single power law: the raw response is nearly flat to middle C and then
    // falls away steeply, which no single exponent follows.
    //
    // IT ONLY GETS US TO ABOUT 19 dB, and that is logged rather than hidden. The raw
    // keyboard spans 45 dB and the part that is left after any smooth fit is JAGGED -
    // neighbouring notes differ - so it is not a slope anybody can correct with a
    // curve. Raising the soundboard from 40 modes to 140 barely moved it (46.4 to
    // 44.9 dB), which rules out the board's mode spacing. The remaining suspects are
    // the things that step discontinuously from note to note: the dispersion solver's
    // stage count jumping 1, 2, 4, 8, the delay-line rounding, and the string count
    // changing at 62 and 140 Hz. A real Steinway spans 6.9 dB across the whole
    // keyboard at one dynamic, measured off the reference, so there is real work here.
    // WHAT THE KEY IS WORTH AT THE OUTPUT, MEASURED PER NOTE.
    //
    // A real piano is evened out by its scaling: hammer mass, string mass and speaking
    // length all change together, and the result is a keyboard that spans only 6.9 dB
    // at one dynamic - measured off the reference Steinway. This model has one hammer
    // mass and one string material, so its raw keyboard spans 44.9 dB.
    //
    // THREE CURVES WERE TRIED AND ALL FAILED, which is why this is a table. A single
    // power law, a quadratic in log-frequency and a quartic each left about 19 dB of
    // residual, because what survives the fit is JAGGED rather than sloped: neighbours
    // differ. Raising the soundboard from 40 modes to 140 moved the raw spread by 1.5 dB
    // and ruled out mode spacing. The remaining causes all STEP from note to note - the
    // dispersion solver's stage count jumping 1, 2, 4, 8, the delay-line rounding, the
    // string count changing at 62 and 140 Hz - and nothing smooth can undo a staircase.
    //
    // So each note is measured and given its own number, which is what the instrument's
    // scaling does physically. Generated by rendering all 88 and normalising to a
    // common peak; regenerate it after any change to the hammer, the board or the
    // dispersion, because all three move it.
    // WHAT THE KEY IS WORTH AT THE OUTPUT, MEASURED PER NOTE.
    // A real Steinway spans 6.9 dB across the keyboard at one dynamic; this model's raw
    // keyboard spans 44. Three smooth fits each left ~19 dB because what survives is
    // JAGGED, not sloped, so every note is rendered and normalised instead - which is
    // what a piano's scaling does physically. REGENERATE after any change to the
    // hammer, the board, the dispersion or the decay: all four move it.
    // MEASURED PER NOTE. Regenerate after any change to the hammer, the board, the
    // dispersion, the decay or the damping - all five move it.
    // MEASURED PER NOTE. Regenerate after any change to the hammer, the board, the
    // dispersion, the decay or the damping - all five move it.
    // MEASURED PER NOTE. Regenerate after any change to the hammer, the board, the
    // dispersion, the decay or the damping.
    // MEASURED PER NOTE. Regenerate after any change to the hammer, board, dispersion,
    // decay or damping.
    // MEASURED PER NOTE. Regenerate after any change to the hammer, board, dispersion,
    // decay, damping or topology.
    // MEASURED PER NOTE. Regenerate after any change to the hammer, board, dispersion,
    // decay, damping or topology.
    // MEASURED PER NOTE. Regenerate after any change to the hammer, board, dispersion,
    // decay, damping, tuning or topology.
    static constexpr float kLevel[88] = {
           1.4926f,    1.4598f,    1.4095f,    1.4715f,    1.6632f,    1.8608f,    2.0220f,    1.8384f,
           2.0825f,    2.1896f,    2.3310f,    2.1443f,    2.1218f,    2.5936f,    2.6401f,    1.2034f,
           1.2194f,    1.3037f,    1.6300f,    1.7679f,    1.5505f,    1.5716f,    2.1049f,    1.6745f,
           1.9772f,    2.0660f,    2.1569f,    2.1535f,    2.0087f,    1.3585f,    0.6306f,    1.0067f,
           0.6694f,    1.0304f,    0.9375f,    0.9379f,    0.6993f,    0.9267f,    1.0469f,    1.0248f,
           1.1432f,    1.0333f,    1.2481f,    0.8523f,    1.2787f,    1.0421f,    1.1751f,    1.3413f,
           1.1564f,    1.3116f,    1.5939f,    1.3288f,    1.4863f,    1.6344f,    1.8785f,    1.7882f,
           1.4160f,    2.6965f,    2.1305f,    2.5053f,    2.1946f,    2.1198f,    2.1628f,    2.2395f,
           2.7391f,    2.9472f,    3.9988f,    3.4221f,    3.2955f,    4.4550f,    2.7905f,    3.7945f,
           3.9179f,    1.8257f,    1.9900f,    5.7068f,    2.1783f,    2.4494f,    3.8316f,    2.7683f,
           4.3180f,    3.1636f,    3.2155f,    3.8034f,    3.9984f,    5.1949f,    4.5593f,    5.0810f
    };
    k.level = kLevel[std::clamp (n - 21, 0, 87)];
    return k;
}


/** THE NOISES, which are most of what "real" means and none of what "correct" does.

    The author, after four milestones of physics: "Not sure if it sounds like a real piano
    yet." Everything up to here models the STRING. But a piano is a machine, and a
    listener identifies it as much by the machine as by the wire: a felt hammer thrown
    at a string by a lever arrives with a thump, the key hits its bed, and on release
    a damper lands on a ringing string and stops it with a sound of its own.

    WITHOUT THESE EVERY NOTE BEGINS AS A PURE TONE, which nothing physical does, and
    that is the tell no amount of partial accuracy removes. They are cheap, they are
    short, and they carry a disproportionate share of the realism.

    Each is a shaped noise burst rather than a sample, so it costs nothing and scales
    with the note.  */
class Mechanics
{
public:
    void prepare (double sampleRate) { sr = (float) sampleRate; reset(); }
    void reset() noexcept { n = 0; life = 0; bp1 = bp2 = lp = 0.0f; rel = false; }

    /** HAMMER AND ACTION, at note-on. Louder and brighter with velocity because the
        whole mechanism is moving faster, and it lasts a few milliseconds - long enough
        to hear, short enough not to be a click track. Centred by pitch: the bass action
        is heavier and thumps lower. */
    void strike (float velocity, float f0) noexcept
    {
        const float v = std::clamp (velocity, 0.0f, 1.0f);
        // RE-SCALED against the per-note level table. The string's output is normalised
        // now, so a fixed thump that was in proportion before came to swamp the note:
        // the gate measured an attack of 1.35 against a body of 0.09. The thump is a
        // detail of the attack, not the attack.
        amp   = 0.010f * v * v;                     // squared: quiet blows are nearly silent
        centre = std::clamp (240.0f + 0.55f * f0, 200.0f, 2600.0f);
        // A WIDE BAND, because an action thump is a thud and not a pitch. At q 0.55 the
        // resonator was narrow enough to read as a tone of its own and the attack's
        // spectral flatness only moved 0.0075 to 0.0112, which is not broadband, it is
        // a second note. The felt, the lever and the key all sound at once and none of
        // them rings.
        q      = 1.9f;
        life  = (int) (sr * (0.004f + 0.010f * (1.0f - v)));   // 4 to 14 ms
        n     = life; rel = false;
    }

    /** THE DAMPER, at note-off. A felt wedge landing on a moving string: duller and
        softer than the strike, and it is what makes a released note sound released
        rather than merely faded. */
    void release (float f0) noexcept
    {
        amp   = 0.016f;
        centre = std::clamp (120.0f + 0.22f * f0, 90.0f, 900.0f);
        q      = 0.35f;
        life  = (int) (sr * 0.030f);
        n     = life; rel = true;
    }

    inline float process() noexcept
    {
        if (n <= 0) return 0.0f;
        rng = rng * 1664525u + 1013904223u;
        const float white = (float) (int) (rng >> 8) / 8388608.0f - 1.0f;
        // ONE BANDPASS, two poles, because the sound is a thud and not a sample: the
        // action's noise is broad but it has a centre, and that centre is what tells
        // a bass thump from a treble one.
        const float w = 6.28318530718f * centre / sr;
        const float r = std::exp (-w * q);
        const float y = (1.0f - r) * white + 2.0f * r * std::cos (w) * bp1 - r * r * bp2;
        bp2 = bp1; bp1 = y;
        // a squared decay, so it thumps rather than fades
        const float t = (float) n / (float) life;
        --n;
        return y * amp * t * t * 6.0f;
    }

    [[nodiscard]] bool active() const noexcept { return n > 0; }

private:
    unsigned rng = 0x13579BDFu;
    float sr = 48000.0f, amp = 0.0f, centre = 500.0f, q = 0.5f;
    float bp1 = 0.0f, bp2 = 0.0f, lp = 0.0f;
    int   n = 0, life = 1;
    bool  rel = false;
};

}} // namespace dsp::piano
