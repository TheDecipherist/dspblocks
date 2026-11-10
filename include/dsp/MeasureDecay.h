// Measuring a reverb's decay time honestly.
//
// Every reverb unit in this workspace has a published decay time somewhere - a
// front-panel number, a spec sheet, a factory table - and that number is the
// only external check on whether the engine is right. So measuring it has to be
// done the way the acoustics world does it, not the way that is easiest.
//
// THREE THINGS THIS GETS RIGHT, each because the naive version is wrong:
//
// 1. T30, NOT "-60 dB FROM PEAK". A real tail does not decay in a straight line
//    all the way down: it starts steep, and the last 20 dB disappear into the
//    noise floor and into whatever late diffusion is left. Timing the fall to
//    -60 dB from the peak therefore reads LONG on a dense reverb and SHORT on a
//    sparse one, and neither error is constant. The standard answer is to fit
//    the straight part - between -5 dB and -35 dB - and EXTRAPOLATE to -60. The
//    -5 dB start skips the direct sound and the early reflections, which are not
//    part of the decay at all.
//
// 2. A BACKWARD-INTEGRATED CURVE. Point-by-point RMS of a decaying noise tail is
//    itself noisy, so the slope fit wanders. Schroeder's trick is to integrate
//    the squared impulse response from the END backwards, which turns a ragged
//    decay into a monotonic curve and makes the fit stable.
//
// 3. IT RETURNS A CONFIDENCE, AND THE CALLER MUST LOOK AT IT. A gated or
//    reversed program has no exponential decay to fit, and a test that reports
//    "2.4 s" for a gate is worse than one that reports nothing. r2 below about
//    0.9 means the tail is not exponential and the number must not be used.
//
// WHY THIS EXISTS AT ALL: the SRV-330 shipped with its reverb decaying at HALF
// the intended time, because the call site trusted a library setter instead of
// measuring what came out. It was eventually corrected with a x2.05 fudge at the
// call site, which is a measurement that should have been a test. Four units in
// one night needed this, so it is here once.
#pragma once
#include "Filters.h"
#include <vector>
#include <cmath>
#include <cstddef>

namespace dsp
{

struct DecayResult
{
    double t60      = 0.0;    // seconds, extrapolated from the -5..-35 dB slope
    double r2       = 0.0;    // fit quality; BELOW 0.9 MEANS DO NOT USE t60
    double peakDb   = 0.0;
    bool   usable   = false;  // r2 >= 0.9 and a real slope was found
};

// tail: the impulse response, mono, from the moment the input stops.
inline DecayResult measureT60 (const std::vector<float>& tail, double sampleRate,
                               double fitFromDb = -5.0, double fitToDb = -35.0)
{
    DecayResult r;
    const std::size_t n = tail.size();
    if (n < 64 || sampleRate <= 0.0) return r;

    // Schroeder backward integration of the squared response.
    std::vector<double> curve (n, 0.0);
    double acc = 0.0;
    for (std::size_t i = n; i-- > 0;)
    {
        acc += double (tail[i]) * double (tail[i]);
        curve[i] = acc;
    }
    if (curve[0] <= 0.0) return r;

    const double ref = curve[0];
    r.peakDb = 10.0 * std::log10 (ref);

    // Collect the samples whose level lies in the fit window.
    std::vector<double> xs, ys;
    xs.reserve (n); ys.reserve (n);
    for (std::size_t i = 0; i < n; ++i)
    {
        if (curve[i] <= 0.0) break;
        const double db = 10.0 * std::log10 (curve[i] / ref);
        if (db <= fitFromDb && db >= fitToDb)
        {
            xs.push_back (double (i) / sampleRate);
            ys.push_back (db);
        }
    }
    if (xs.size() < 16) return r;      // not enough of a straight part to fit

    // Least-squares slope, in dB per second.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const double m = double (xs.size());
    for (std::size_t i = 0; i < xs.size(); ++i)
    { sx += xs[i]; sy += ys[i]; sxx += xs[i]*xs[i]; sxy += xs[i]*ys[i]; }
    const double den = m * sxx - sx * sx;
    if (std::abs (den) < 1e-12) return r;
    const double slope = (m * sxy - sx * sy) / den;         // dB/s, negative
    const double icept = (sy - slope * sx) / m;
    if (slope >= -1e-6) return r;                            // not decaying

    // r^2 of the fit.
    const double mean = sy / m;
    double ssTot = 0, ssRes = 0;
    for (std::size_t i = 0; i < xs.size(); ++i)
    {
        const double pred = slope * xs[i] + icept;
        ssTot += (ys[i] - mean) * (ys[i] - mean);
        ssRes += (ys[i] - pred) * (ys[i] - pred);
    }
    r.r2 = ssTot > 0.0 ? 1.0 - ssRes / ssTot : 0.0;

    // Extrapolate the fitted line to -60 dB.
    r.t60 = -60.0 / slope;
    r.usable = r.r2 >= 0.9 && r.t60 > 0.0;
    return r;
}

// ---------------------------------------------------------------------------
// BAND-LIMITED DECAY, for the machines whose decay time is published PER BAND.
//
// A lot of these boxes do not have one reverb time, they have two or three. The
// Lexicon PCM 60 prints "low-frequency reverb time is increased by approximately
// 50% below 800 Hz"; the AMS RMX 16's LOW filter is "a quarter of the indicated
// time at -9 to four times at +9"; the Sony DRE-2000 has RT LOW FREQ RANGE and
// RT HIGH FREQ RANGE as multipliers x0.5 to x2.0 and x0.25 to x1.0; dspblocks'
// own ReverbTank has had setLowDecayMultiplier since the start. Nothing could
// MEASURE any of it, so every one of those laws was asserted by reading the code
// that implemented it.
//
// TWO THINGS THIS GETS RIGHT, each because the obvious version is wrong:
//
// 1. ZERO PHASE, so the band filter does not smear the start of the tail into
//    the fit window. The filter runs forward and then backward over the
//    reversed signal, which cancels the phase exactly and squares the
//    magnitude - a 4th-order Butterworth applied twice is 8th-order and
//    symmetric. A causal filter delays the low band more than the high one and
//    the two bands' fits then start at different places in the decay.
//
// 2. MEASURE AWAY FROM THE CROSSOVER. A caller asking "what is the decay below
//    800 Hz" must not measure AT 800 Hz: a reverb's band split is usually a
//    gentle one-pole, so at the corner the two bands are mixed half and half and
//    the measured ratio comes out compressed towards 1. An octave or more clear
//    of the corner is the usable region, and that is the caller's choice to
//    make, which is why this takes two edges and not a crossover.
//
// loHz <= 0 means no high-pass; hiHz <= 0 or above Nyquist means no low-pass.
inline std::vector<float> bandFiltered (const std::vector<float>& x, double sampleRate,
                                        double loHz, double hiHz)
{
    std::vector<float> y = x;
    if (y.empty() || sampleRate <= 0.0) return y;
    const double nyq = sampleRate * 0.5;

    // Two cascaded biquads with Butterworth Q's make a 4th-order section; run
    // forward and backward it is 8th-order, zero phase.
    constexpr double kQ[2] = { 0.54119610, 1.30656296 };

    const auto pass = [&] (Biquad::Type type, double hz)
    {
        Biquad a, b;
        a.prepare (sampleRate); b.prepare (sampleRate);
        a.set (type, hz, kQ[0]); b.set (type, hz, kQ[1]);
        for (auto& v : y) v = b.process (a.process (v));
        // Backward pass over the reversed signal, from a cleared state.
        a.reset(); b.reset();
        a.set (type, hz, kQ[0]); b.set (type, hz, kQ[1]);
        for (std::size_t i = y.size(); i-- > 0;) y[i] = b.process (a.process (y[i]));
    };

    if (loHz > 0.0 && loHz < nyq) pass (Biquad::Type::HighPass, loHz);
    if (hiHz > 0.0 && hiHz < nyq) pass (Biquad::Type::LowPass, hiHz);
    return y;
}

/** measureT60 on one band of the tail. Same contract, same `usable` rule: a band
    whose r2 is below 0.9 has no exponential decay in it and its t60 must not be
    used. Pass band EDGES that are clear of the machine's crossover - see the
    note above. */
inline DecayResult measureBandT60 (const std::vector<float>& tail, double sampleRate,
                                   double loHz, double hiHz,
                                   double fitFromDb = -5.0, double fitToDb = -35.0)
{
    return measureT60 (bandFiltered (tail, sampleRate, loHz, hiHz), sampleRate,
                       fitFromDb, fitToDb);
}

// Convenience: does the measured decay match a published figure?
// tolerance is FRACTIONAL (0.15 = within 15 percent), because published decay
// times are quoted to two significant figures and a fixed absolute tolerance is
// either useless at 0.2 s or meaningless at 10 s.
inline bool decayMatches (const DecayResult& r, double publishedSeconds,
                          double tolerance = 0.15)
{
    if (! r.usable || publishedSeconds <= 0.0) return false;
    return std::abs (r.t60 - publishedSeconds) <= tolerance * publishedSeconds;
}

} // namespace dsp
