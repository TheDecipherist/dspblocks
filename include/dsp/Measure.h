// Measuring whether an effects unit's presets are actually different.
//
// "All the presets sound like they don't really change anything" is a real
// report and a testable one, so it belongs in code rather than in an opinion.
// This turns a bank of renders into one number: the median pairwise difference
// between presets, where 0 is "byte-identical" and 1 is "nothing in common".
//
// TWO THINGS ABOUT THE MEASUREMENT, both learned the hard way:
//
// 1. THE WINDOW. A first attempt measured 0.96 s of tails that run past four
//    seconds, and every reverb came out identical - because a reverb's first
//    second is its LEAST distinctive part. Everything is dense and bright at
//    the start; what separates a plate from a hall is what is left at three
//    seconds. `kMinSeconds` is the floor this enforces.
//
// 2. THE FEATURES. Comparing three band energies summed over the whole render
//    cannot tell a 400 ms echo from a 400 ms reverb: both put the same energy
//    in the same bands. The feature vector here is a small spectrogram - log
//    bands by time frames - so a repeating echo and a smooth decay are
//    different SHAPES even when their totals match.
#pragma once
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

namespace dsp { namespace measure {

/** Frames of log-spaced band energy: a coarse spectrogram, normalised so the
    comparison is about shape rather than level.

    The FRAMES are log-spaced in time as well as the bands in frequency, and
    that matters as much as the window length. With equal frames over ten
    seconds, every short effect - an ambience, a gate, a slap - has nine of its
    ten frames in silence, so thirty-six different ambiences all measure as
    "silence" and compare as identical. A reverb's structure is logarithmic in
    time: the first 100 ms carries the early pattern, the first second the
    build-up, and the rest the tail. Frames spaced 2^k give each of those its
    own row. */
struct Features
{
    static constexpr int kBands = 12;
    static constexpr int kFrames = 10;
    double e[kFrames][kBands] {};
    double rms = 0.0;
    double decayDb = 0.0;      // fall from the loudest frame to the last, in dB
    int    echoes = 0;         // discrete repeats found in the impulse response
    double echoSpacingMs = 0.0;// mean gap between them
    double crestDb = 0.0;      // peak over RMS across the whole render
    double troughDb = 0.0;     // how far the envelope falls BETWEEN repeats
    double combMs = 0.0;       // spacing of the shortest repeat, 0.1 to 60 ms
    bool   silent = true;

    /** Does this render have an audible echo STRUCTURE, as opposed to a smooth
        decay? This is the question behind "the delays don't sound like delays,
        most of them sound just like reverbs": a delay's impulse response is a
        row of separated spikes and a reverb's is a smooth wash, and no amount
        of matching the spectrum makes one into the other. */
    bool hasDiscreteEchoes() const { return echoes >= 2; }

    /** Is this a DELAY or a REVERB?

        Counting repeats is not enough. A reverb tank with its echo taps turned
        up also produces repeats - that was exactly the PCM 91's "Tape Echoes",
        which measured eleven of them and still sounded like a reverb, because
        each repeat was a burst of reverb rather than a copy of the input.

        What separates them is how far the level falls BETWEEN repeats. A delay
        leaves near-silence in the gaps; a reverb fills them with its own tail,
        so the repeats are bumps on a wash. 18 dB is the boundary: below it the
        gap is still audibly ringing, above it the repeats are separate events.
        (The whole-render crest factor does NOT work for this - it is dominated
        by the decay, and a plain hall measures 29 dB.) */
    bool readsAsDelay() const { return echoes >= 2 && troughDb > 18.0; }
};

/** The spacing of the SHORT repeat, in milliseconds, or 0 if there is none.

    The spectrogram cannot see this. Its bands are a fraction of an octave wide,
    and a comb 500 Hz apart falls inside one band, so a flanger at 2 ms and a
    chorus at 25 ms read as the same spectrum. They are not the same effect:
    2 ms is the flanging window and 25 ms is the chorus window, and the whole
    difference between those two effects is that number.

    Measured as the lag of the strongest normalised autocorrelation peak between
    0.1 ms and 60 ms, which is where combs, flangers, choruses and doublers all
    live. Above 60 ms the repeats separate and `countEchoes` is the right tool. */
inline double combSpacingMs (const std::vector<float>& x, double sr)
{
    const int lo = std::max (4, (int) (sr * 0.0001));
    const int hi = (int) (sr * 0.060);
    if ((int) x.size() < hi * 3) return 0.0;
    // The window starts just past the direct sound and is short, because that
    // is where a comb lives: a flanger at 2 ms with moderate feedback is over
    // inside 50 ms. An earlier version started at 50 ms and ran for half a
    // second, by which point the flanger had decayed to nothing and every comb
    // measured as zero.
    // The window includes the DIRECT sound. A flanger has many repeats and is
    // periodic on its own, but a doubler or a chorus has ONE delayed copy, and
    // the only way to find that copy's lag is to correlate it against the
    // direct sound it is a copy of. Starting after the direct sound found the
    // flanger and missed both of the others.
    const size_t off = 0;
    const int n = std::min ((int) (x.size() - off), (int) (sr * 0.30));
    if (n < hi * 2) return 0.0;
    const float* p = x.data() + off;
    double e0 = 0.0;
    for (int i = 0; i < n; ++i) e0 += (double) p[i] * p[i];
    if (e0 < 1e-12) return 0.0;

    double best = 0.0; int bestLag = 0;
    for (int lag = lo; lag < hi; ++lag)
    {
        double num = 0.0, den = 0.0;
        for (int i = 0; i + lag < n; ++i)
        { num += (double) p[i] * p[i + lag]; den += (double) p[i + lag] * p[i + lag]; }
        if (den < 1e-12) continue;
        const double r = num / std::sqrt (den * e0);
        if (r > best) { best = r; bestLag = lag; }
    }
    // Below about 0.35 correlation there is no repeat, only noise. A reverb
    // tank finds a weak spurious peak near the top of the range at 0.15; it
    // drops out by 0.25, and a flanger, a chorus and a doubler all survive
    // past 0.45.
    return (best > 0.35 && bestLag > 0) ? (double) bestLag * 1000.0 / sr : 0.0;
}

/** Count discrete repeats in an impulse response.

    A repeat is a peak in the smoothed envelope that stands at least
    `thresholdDb` above the envelope 60 ms either side of it, and is at least
    `minGapMs` from the previous one. Those two numbers are what separate an
    echo from the ripple on a reverb tail: below about 30 ms the ear fuses
    repeats anyway, and a bump smaller than 6 dB is not heard as a separate
    event. */
inline int countEchoes (const std::vector<float>& x, double sr,
                        double* meanGapMs = nullptr,
                        double* troughDepthDb = nullptr,
                        double thresholdDb = 6.0, double minGapMs = 30.0)
{
    if (x.size() < (size_t) (sr * 0.2)) return 0;
    // Envelope: rectify and smooth with a 5 ms one-pole, fast enough to keep a
    // repeat's shape and slow enough to ignore the waveform itself.
    const double a = 1.0 - std::exp (-1.0 / (0.005 * sr));
    std::vector<double> env (x.size(), 0.0);
    double e = 0.0;
    for (size_t i = 0; i < x.size(); ++i) { const double r = std::fabs ((double) x[i]); e += a * (r - e); env[i] = e; }

    const int look = (int) (sr * 0.06);
    const int minGap = (int) (sr * minGapMs * 0.001);
    const double ratio = std::pow (10.0, thresholdDb / 20.0);
    double peak = 0.0; for (double v : env) peak = std::max (peak, v);
    if (peak <= 0.0) return 0;
    const double floorLevel = peak * 1e-4;      // ignore what is 80 dB down

    int count = 0; long last = -minGap; long firstAt = -1, lastAt = -1;
    std::vector<long> peaks;
    for (long i = look; i + look < (long) env.size(); ++i)
    {
        if (env[(size_t) i] < floorLevel) continue;
        if (i - last < minGap) continue;
        // local maximum over the window
        bool top = true;
        for (long k = i - look; k <= i + look; ++k)
            if (env[(size_t) k] > env[(size_t) i]) { top = false; break; }
        if (! top) continue;
        const double before = env[(size_t) (i - look)], after = env[(size_t) (i + look)];
        if (env[(size_t) i] > before * ratio && env[(size_t) i] > after * ratio)
        {
            ++count; last = i; peaks.push_back (i);
            if (firstAt < 0) firstAt = i;
            lastAt = i;
        }
    }
    if (meanGapMs) *meanGapMs = (count > 1 && lastAt > firstAt)
        ? (double) (lastAt - firstAt) * 1000.0 / sr / (double) (count - 1) : 0.0;

    // How far the envelope FALLS between one repeat and the next.
    //
    // This is the measurement that separates a delay from a reverb whose echo
    // taps are turned up, and it is the one that was missing when the PCM 91's
    // "Tape Echoes" measured eleven clean repeats and still sounded like a
    // reverb. In a delay the gaps between repeats are near-silent, so the
    // trough is 30 dB or more below the peaks. In a reverb the tail fills the
    // gaps in, so the trough is a few dB down and the repeats are bumps on a
    // wash rather than separate events.
    if (troughDepthDb)
    {
        std::vector<double> depths;
        for (size_t k = 0; k + 1 < peaks.size(); ++k)
        {
            double lo2 = 1e30;
            for (long i = peaks[k]; i <= peaks[k + 1]; ++i) lo2 = std::min (lo2, env[(size_t) i]);
            const double hi2 = std::min (env[(size_t) peaks[k]], env[(size_t) peaks[k + 1]]);
            if (hi2 > 0.0 && lo2 > 0.0) depths.push_back (20.0 * std::log10 (hi2 / lo2));
            else if (hi2 > 0.0) depths.push_back (120.0);
        }
        if (depths.empty()) *troughDepthDb = 0.0;
        else { std::sort (depths.begin(), depths.end()); *troughDepthDb = depths[depths.size() / 2]; }
    }
    return count;
}

/** Normalised echo density: how DIFFUSE an impulse response has become by time
    `atSeconds`, on a 0..1 scale.

    Abel and Huang's measure (AES 121, 2006). In a window around that instant,
    count the fraction of samples whose magnitude exceeds the window's own
    standard deviation, and divide by the fraction a Gaussian would give,
    erfc(1/sqrt2) = 0.3173. A sparse response - a few discrete echoes - has most
    of its energy in a few large samples and scores well below 1. A response that
    has become Gaussian noise scores 1, and that is what "diffuse" means.

    This is the number that separates a reverb from a multi-tap delay wearing its
    name, and the number that says whether one tank builds density faster than
    another. It saturates, so compare two topologies at the SAME early instant -
    by 200 ms most tanks read 1.0 and the measurement stops discriminating. */
inline double echoDensity (const std::vector<float>& x, double sr,
                           double atSeconds, double windowMs = 20.0)
{
    const long half = (long) (windowMs * 0.001 * sr * 0.5);
    const long c = (long) (atSeconds * sr);
    const long lo = std::max (0L, c - half), hi = std::min ((long) x.size(), c + half);
    const long n = hi - lo;
    if (n < 16) return 0.0;
    double mean = 0.0;
    for (long i = lo; i < hi; ++i) mean += x[(size_t) i];
    mean /= (double) n;
    double var = 0.0;
    for (long i = lo; i < hi; ++i) { const double d = x[(size_t) i] - mean; var += d * d; }
    const double sd = std::sqrt (var / (double) n);
    if (sd <= 0.0) return 0.0;
    long above = 0;
    for (long i = lo; i < hi; ++i) if (std::fabs (x[(size_t) i] - mean) > sd) ++above;
    return ((double) above / (double) n) / 0.3173105;
}

/** How long a render must be before the numbers mean anything. */
inline constexpr double kMinSeconds = 8.0;

/** Goertzel magnitude at one bin, cheaper than a full DFT when only a dozen
    bands are wanted. */
inline double bandEnergy (const float* x, int n, double sr, double lo, double hi)
{
    // Sum a handful of Goertzel probes spread across the band, which is enough
    // to characterise it and costs a fraction of an FFT.
    const int probes = 5;
    double total = 0.0;
    for (int p = 0; p < probes; ++p)
    {
        const double f = lo * std::pow (hi / lo, (p + 0.5) / probes);
        const double w = 2.0 * M_PI * f / sr;
        const double c = 2.0 * std::cos (w);
        double s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < n; ++i) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
        total += s1 * s1 + s2 * s2 - c * s1 * s2;
    }
    return total / (double) (n ? n : 1);
}

/** Build the feature vector for one render. `mono` should be at least
    `kMinSeconds` long; shorter input is accepted but flagged by the caller's
    own check against kMinSeconds, because silently measuring the wrong window
    is exactly the failure this file exists to prevent. */
inline Features analyse (const std::vector<float>& mono, double sr)
{
    Features f;
    if (mono.empty()) return f;
    // Log-spaced frame boundaries: frame k covers [T*(2^k-1)/(2^K-1),
    // T*(2^(k+1)-1)/(2^K-1)), so the first frame is a few milliseconds and the
    // last is half the render.
    const double span = (double) ((1 << Features::kFrames) - 1);
    size_t bound[Features::kFrames + 1];
    for (int k = 0; k <= Features::kFrames; ++k)
        bound[k] = (size_t) ((double) mono.size() * (double) ((1 << k) - 1) / span);
    if (mono.size() < 4096) return f;

    double peakFrame = 0.0, lastFrame = 0.0, total = 0.0;
    for (int t = 0; t < Features::kFrames; ++t)
    {
        const float* p = mono.data() + bound[t];
        const int frameLen = (int) (bound[t + 1] - bound[t]);
        if (frameLen < 64) continue;
        double frameTotal = 0.0;
        for (int b = 0; b < Features::kBands; ++b)
        {
            const double lo = 40.0 * std::pow (16000.0 / 40.0, (double) b / Features::kBands);
            const double hi = 40.0 * std::pow (16000.0 / 40.0, (double) (b + 1) / Features::kBands);
            const double e = bandEnergy (p, frameLen, sr, lo, std::min (hi, sr * 0.45));
            f.e[t][b] = e;
            frameTotal += e;
        }
        total += frameTotal;
        peakFrame = std::max (peakFrame, frameTotal);
        lastFrame = frameTotal;
        double sum = 0.0;
        for (int i = 0; i < frameLen; ++i) sum += (double) p[i] * p[i];
        f.rms += sum;
    }
    // Frames far below the loudest are silence, and comparing silence to
    // silence says nothing; zero them so the cosine distance is taken over the
    // part of the render that has content in it.
    for (int t = 0; t < Features::kFrames; ++t)
    {
        double s2 = 0.0;
        for (int b = 0; b < Features::kBands; ++b) s2 += f.e[t][b];
        if (peakFrame > 0.0 && s2 < peakFrame * 1e-9)
            for (int b = 0; b < Features::kBands; ++b) f.e[t][b] = 0.0;
    }
    f.rms = std::sqrt (f.rms / (double) mono.size());
    f.silent = total < 1e-12;
    f.decayDb = (peakFrame > 0.0 && lastFrame > 0.0)
                  ? 10.0 * std::log10 (lastFrame / peakFrame) : -120.0;
    f.echoes = countEchoes (mono, sr, &f.echoSpacingMs, &f.troughDb);

    // Crest factor over everything past the direct sound, so the input impulse
    // itself does not set the peak.
    const size_t skip = std::min (mono.size(), (size_t) (sr * 0.02));
    double pk = 0.0, sq = 0.0; size_t n = 0;
    for (size_t i = skip; i < mono.size(); ++i)
    { const double v = std::fabs ((double) mono[i]); pk = std::max (pk, v); sq += v * v; ++n; }
    const double rmsTail = n ? std::sqrt (sq / (double) n) : 0.0;
    f.crestDb = (pk > 0.0 && rmsTail > 0.0) ? 20.0 * std::log10 (pk / rmsTail) : 0.0;
    f.combMs = combSpacingMs (mono, sr);

    // Normalise each frame to its own total, then the whole vector to its
    // energy, so a preset that is simply quieter does not read as different.
    if (! f.silent)
        for (int t = 0; t < Features::kFrames; ++t)
        {
            double s = 0.0;
            for (int b = 0; b < Features::kBands; ++b) s += f.e[t][b];
            if (s > 0.0) for (int b = 0; b < Features::kBands; ++b) f.e[t][b] /= s;
        }
    return f;
}

/** 0 = the same sound, 1 = nothing in common.

    Cosine distance over the spectrogram, plus a term for how differently the
    two decay, because two presets can have the same spectrum frame by frame
    and still be a gate and a hall. */
inline double difference (const Features& a, const Features& b)
{
    if (a.silent && b.silent) return 0.0;
    if (a.silent != b.silent) return 1.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int t = 0; t < Features::kFrames; ++t)
        for (int i = 0; i < Features::kBands; ++i)
        {
            dot += a.e[t][i] * b.e[t][i];
            na  += a.e[t][i] * a.e[t][i];
            nb  += b.e[t][i] * b.e[t][i];
        }
    const double cosDist = (na > 0 && nb > 0) ? 1.0 - dot / std::sqrt (na * nb) : 1.0;
    // 60 dB of difference in decay counts for as much as a completely different
    // spectrum; anything less scales down from there.
    const double decayDist = std::min (1.0, std::fabs (a.decayDb - b.decayDb) / 60.0);
    // A row of discrete repeats and a smooth wash are different effects however
    // similar their spectra are, so structure counts separately.
    const double structDist = (a.readsAsDelay() != b.readsAsDelay())
        ? 1.0
        : std::min (1.0, 0.5 * std::fabs ((double) a.echoes - (double) b.echoes) / 8.0
                       + 0.5 * std::fabs (a.troughDb - b.troughDb) / 40.0);
    // Comb spacing, on a log scale: a flanger at 2 ms and a chorus at 25 ms are
    // 3.6 octaves apart and are different effects; 2 ms against 2.2 ms is the
    // same effect at a different setting. Two renders with no short repeat at
    // all agree on that, and one with and one without disagree completely.
    double combDist = 0.0;
    if ((a.combMs > 0.0) != (b.combMs > 0.0)) combDist = 1.0;
    else if (a.combMs > 0.0 && b.combMs > 0.0)
        combDist = std::min (1.0, std::fabs (std::log2 (a.combMs / b.combMs)) / 4.0);

    return std::min (1.0, 0.45 * cosDist + 0.15 * decayDist + 0.2 * structDist + 0.2 * combDist);
}

struct Report
{
    int    presets = 0;
    long   pairs = 0;
    double median = 0.0, mean = 0.0, worst = 1.0, best = 0.0;
    long   identical = 0;      // pairs under kIdentical
    double levelSpreadDb = 0.0;
    int    silent = 0;
    int    withEchoes = 0;     // renders whose impulse response has discrete repeats
    int    readAsDelay = 0;    // ... and whose repeats are sharp enough to be heard as echoes
    /** Below this a pair is, for listening purposes, the same preset. */
    static constexpr double kIdentical = 0.02;
    /** A bank whose median clears this has presets that genuinely differ. */
    static constexpr double kHealthy = 0.15;
    bool healthy() const { return median >= kHealthy && identical * 20 < pairs; }
};

inline Report compare (const std::vector<Features>& f)
{
    Report r;
    r.presets = (int) f.size();
    if (f.size() < 2) return r;
    std::vector<double> d;
    d.reserve (f.size() * (f.size() - 1) / 2);
    double lo = 1e9, hi = 0.0;
    for (size_t i = 0; i < f.size(); ++i)
    {
        if (f[i].silent) ++r.silent;
        if (f[i].hasDiscreteEchoes()) ++r.withEchoes;
        if (f[i].readsAsDelay()) ++r.readAsDelay;
        if (f[i].rms > 0) { lo = std::min (lo, f[i].rms); hi = std::max (hi, f[i].rms); }
        for (size_t j = i + 1; j < f.size(); ++j) d.push_back (difference (f[i], f[j]));
    }
    r.pairs = (long) d.size();
    std::sort (d.begin(), d.end());
    r.median = d[d.size() / 2];
    r.best = d.back();
    r.worst = d.front();
    double sum = 0.0; for (double v : d) sum += v;
    r.mean = sum / (double) d.size();
    for (double v : d) if (v < Report::kIdentical) ++r.identical;
    r.levelSpreadDb = (lo < 1e9 && hi > 0) ? 20.0 * std::log10 (hi / lo) : 0.0;
    return r;
}

}} // namespace dsp::measure
