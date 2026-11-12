// Behavioural tests: does each block actually do the thing it is named after?
//
// Not golden-file comparisons. A golden file locks in whatever the code did on
// the day it was written, including its bugs. These ask the question a listener
// would: is the echo where I asked for it, did the pitch actually move, does the
// phaser put notches where a phaser puts them.
#include <cstdio>
#include <cmath>
#include <vector>
#include <complex>
#include <algorithm>
#include "dsp/Blocks.h"
#include "dsp/Constants.h"

static int failures = 0;
static void check (bool ok, const char* what, double got = 0, double want = 0)
{
    if (ok) { std::printf ("  ok    %s\n", what); return; }
    std::printf ("  FAIL  %s   (got %g, wanted %g)\n", what, got, want);
    ++failures;
}

// Dominant frequency of a buffer, by finding the largest DFT bin.
static double dominantHz (const std::vector<float>& x, double sr)
{
    const int N = 8192;
    if ((int) x.size() < N) return 0.0;
    const size_t off = x.size() - N;
    double best = 0; int bestK = 0;
    for (int k = 4; k < N / 2; ++k)
    {
        double re = 0, im = 0;
        const double w = 2.0 * dsp::kPi * k / N;
        for (int i = 0; i < N; ++i) { re += x[off + i] * std::cos (w * i); im -= x[off + i] * std::sin (w * i); }
        const double m = re * re + im * im;
        if (m > best) { best = m; bestK = k; }
    }
    return bestK * sr / N;
}

int main()
{
    const double sr = 48000.0;

    // ---- delay: is the echo where it was asked for? -----------------------
    std::printf ("FeedbackDelay\n");
    {
        dsp::FeedbackDelay d;
        d.prepare (sr, 1.0);
        d.setDelaySeconds (0.1);       // 100 ms = 4800 samples
        d.setFeedback (0.0f);
        d.setDamping (20000.0);
        std::vector<float> out;
        for (int i = 0; i < 12000; ++i) out.push_back (d.process (i == 0 ? 1.0f : 0.0f));
        int peak = 0; float best = 0;
        for (int i = 1; i < (int) out.size(); ++i) if (std::fabs (out[i]) > best) { best = std::fabs (out[i]); peak = i; }
        check (std::abs (peak - 4800) <= 2, "echo lands at the requested 100 ms", peak, 4800);

        // feedback should give repeats, and they should decay
        dsp::FeedbackDelay f;
        f.prepare (sr, 1.0); f.setDelaySeconds (0.05); f.setFeedback (0.5f); f.setDamping (20000.0);
        std::vector<float> o2;
        for (int i = 0; i < 20000; ++i) o2.push_back (f.process (i == 0 ? 1.0f : 0.0f));
        const float r1 = std::fabs (o2[2400]), r2 = std::fabs (o2[4800]), r3 = std::fabs (o2[7200]);
        check (r1 > 0.4f && r2 > 0.2f && r3 > 0.08f, "three repeats present", r3, 0.08);
        check (r2 < r1 && r3 < r2, "repeats decay", r2, r1);

        // A delay of ZERO must be a delay of one sample, not a delay of the
        // whole buffer. Reading happens before writing, so `w` points at the
        // slot about to be written and asking for 0 reads the OLDEST sample.
        // The H3000's Reverb Factory hit this: predelay 0 ms put the reverb
        // three seconds late and the algorithm measured as silent.
        dsp::FeedbackDelay z;
        z.prepare (sr, 1.6); z.setDelaySeconds (0.0); z.setFeedback (0.0f); z.setDamping (20000.0);
        std::vector<float> o3;
        for (int i = 0; i < 8000; ++i) o3.push_back (z.process (i == 0 ? 1.0f : 0.0f));
        int zp = -1;
        for (int i = 0; i < (int) o3.size(); ++i) if (std::fabs (o3[i]) > 0.5f) { zp = i; break; }
        check (zp >= 0 && zp <= 2, "a delay of zero comes out within a sample or two", zp, 1);
    }

    // ---- pitch: did it actually shift? ------------------------------------
    std::printf ("PitchShifter\n");
    {
        for (double semis : { 12.0, 7.0, -12.0 })
        {
            dsp::PitchShifter p;
            p.prepare (sr);
            p.setSemitones (semis);
            p.setWindow (0.050);
            std::vector<float> out;
            double ph = 0;
            for (int i = 0; i < 40000; ++i)
            {
                const float v = (float) std::sin (ph); ph += 2.0 * dsp::kPi * 440.0 / sr;
                out.push_back (p.process (v));
            }
            const double want = 440.0 * std::pow (2.0, semis / 12.0);
            const double got = dominantHz (out, sr);
            char msg[96]; std::snprintf (msg, sizeof msg, "440 Hz shifted %+g semitones -> %.0f Hz", semis, want);
            check (std::fabs (got - want) / want < 0.04, msg, got, want);
        }
    }

    // ---- phaser: notches, and the right number of them ---------------------
    std::printf ("Phaser\n");
    {
        dsp::Phaser ph;
        ph.prepare (sr);
        ph.setStages (4);
        ph.setRate (0.0);            // frozen, so the response is measurable
        ph.setDepth (0.0f);
        ph.setMix (0.5f);
        ph.setFeedback (0.0f);
        ph.setRange (800.0, 800.0);  // pinned
        // sweep a sine and look for dips
        int notches = 0; double prev = 1e9, prevPrev = 1e9;
        for (int f = 100; f < 8000; f += 25)
        {
            dsp::Phaser q; q.prepare (sr); q.setStages (4); q.setRate (0.0);
            q.setDepth (0.0f); q.setMix (0.5f); q.setFeedback (0.0f); q.setRange (800.0, 800.0);
            double e = 0, phase = 0;
            for (int i = 0; i < 4000; ++i)
            { const float v = (float) std::sin (phase); phase += 2.0 * dsp::kPi * f / sr;
              const float y = q.process (v); if (i > 2000) e += (double) y * y; }
            if (prevPrev > prev && prev < e && prev < 0.25 * prevPrev) ++notches;
            prevPrev = prev; prev = e;
        }
        check (notches >= 1 && notches <= 3, "a 4-stage phaser puts 1-2 notches in the band", notches, 2);
    }

    // ---- flanger vs chorus: the delay range is the difference -------------
    std::printf ("Flanger and Chorus are different effects\n");
    {
        dsp::Flanger fl; fl.prepare (sr);
        fl.setDelayRange (0.5, 5.0); fl.setRate (0.5); fl.setDepth (1.0f);
        fl.setFeedback (0.7f); fl.setMix (0.5f); fl.setDamping (12000.0);
        double fe = 0;
        for (int i = 0; i < 48000; ++i)
        { const float y = fl.process (i % 3 ? 0.1f : -0.1f); fe += (double) y * y; }

        dsp::Chorus ch; ch.prepare (sr);
        ch.setVoices (3); ch.setRate (0.5); ch.setDelay (22.0, 6.0); ch.setMix (0.5f);
        double cl = 0, cr = 0;
        for (int i = 0; i < 48000; ++i)
        { float a, b; ch.process (i % 3 ? 0.1f : -0.1f, a, b); cl += (double) a * a; cr += (double) b * b; }
        check (fe > 0.0 && cl > 0.0, "both produce output", fe, 1);
        check (std::fabs (cl - cr) / (cl + cr) < 0.5, "chorus is stereo but balanced", std::fabs (cl-cr)/(cl+cr), 0);
    }

    // ---- tremolo: amplitude only, and depth 1 reaches silence -------------
    std::printf ("Tremolo\n");
    {
        dsp::Tremolo t; t.prepare (sr); t.setRate (4.0); t.setDepth (1.0f);
        float lo = 1e9f, hi = 0.0f;
        for (int i = 0; i < 48000; ++i)
        { float a, b; t.process (1.0f, a, b); lo = std::min (lo, std::fabs (a)); hi = std::max (hi, std::fabs (a)); }
        check (lo < 0.02f, "depth 1 reaches near silence", lo, 0);
        check (hi > 0.98f, "and full level at the top", hi, 1);
    }


    // ---- filters: is the corner where it was asked for? -------------------
    std::printf ("Filters\n");
    {
        // one-pole at 1 kHz should be -3 dB there and -20 dB a decade up
        auto onePoleGain = [&] (double fc, double f)
        {
            dsp::OnePole p; p.prepare (sr); p.setCutoff (fc);
            double e = 0, ph = 0;
            for (int i = 0; i < 20000; ++i)
            { const float v = (float) std::sin (ph); ph += 2.0 * dsp::kPi * f / sr;
              const float y = p.lp (v); if (i > 10000) e += (double) y * y; }
            return std::sqrt (e / 10000.0) * std::sqrt (2.0);
        };
        const double at = onePoleGain (1000.0, 1000.0);
        check (std::fabs (20 * std::log10 (at) + 3.0) < 0.6, "one-pole is -3 dB at its corner", 20 * std::log10 (at), -3.0);

        // SVF resonance: a high Q must actually peak at the cutoff
        auto svfGain = [&] (double fc, double q, double f)
        {
            dsp::StateVariableFilter s; s.prepare (sr); s.set (fc, q);
            double e = 0, ph = 0;
            for (int i = 0; i < 20000; ++i)
            { const float v = (float) std::sin (ph); ph += 2.0 * dsp::kPi * f / sr;
              const float y = s.lowpass (v); if (i > 10000) e += (double) y * y; }
            return std::sqrt (e / 10000.0) * std::sqrt (2.0);
        };
        const double q8 = svfGain (1000.0, 8.0, 1000.0), q07 = svfGain (1000.0, 0.7071, 1000.0);
        check (q8 > q07 * 5.0, "SVF Q 8 peaks well above Q 0.7 at the corner", q8 / q07, 5.0);
        check (svfGain (1000.0, 0.7071, 8000.0) < 0.05, "SVF low-pass rejects two octaves up", svfGain (1000.0, 0.7071, 8000.0), 0.05);

        // Ladder: 24 dB/octave, and it self-oscillates rather than dying or
        // exploding. Both halves matter - a ladder whose resonance is limited in
        // GAIN cannot self-oscillate at all, and one whose output is clamped
        // buzzes instead of singing.
        auto ladderGain = [&] (double fc, float res, double f)
        {
            dsp::LadderFilter l; l.prepare (sr); l.set (fc, res);
            double e = 0, ph = 0;
            for (int i = 0; i < 20000; ++i)
            { const float v = (float) std::sin (ph); ph += 2.0 * dsp::kPi * f / sr;
              const float y = l.process (v * 0.2f); if (i > 10000) e += (double) y * y; }
            return std::sqrt (e / 10000.0) * std::sqrt (2.0) / 0.2;
        };
        const double lPass = ladderGain (1000.0, 0.0f, 100.0);
        const double lOct1 = ladderGain (1000.0, 0.0f, 2000.0);
        const double lOct2 = ladderGain (1000.0, 0.0f, 4000.0);
        const double slope = 20.0 * std::log10 (lOct1 / lOct2);
        check (std::fabs (20 * std::log10 (lPass) ) < 1.0, "ladder passes below its corner", 20 * std::log10 (lPass), 0.0);
        check (slope > 18.0 && slope < 30.0, "ladder rolls off about 24 dB/octave", slope, 24.0);
        check (ladderGain (1000.0, 0.9f, 1000.0) > lOct1 * 4.0, "ladder resonance peaks at the corner",
               ladderGain (1000.0, 0.9f, 1000.0) / lOct1, 4.0);
        {
            // PER-STAGE SATURATION (Sub37 B-22). Off, the ladder is the one every
            // machine already had: its output for a fixed signal is pinned to the
            // value measured before the switch existed (143.141899516). On, a quiet
            // signal passes as before and a loud one is compressed harder.
            const auto fingerprint = [&] (bool stages)
            {
                double sum = 0.0;
                for (const float res : { 0.0f, 0.6f, 1.1f })
                    for (const float amp : { 0.05f, 0.5f, 1.5f })
                    {
                        dsp::LadderFilter l; l.prepare (48000.0); l.set (700.0, res); l.setStageSaturation (stages);
                        for (int i = 0; i < 4800; ++i)
                        {
                            const double t = i / 48000.0;
                            const float x = amp * (float) (std::sin (2 * dsp::kPi * 110 * t) + 0.5 * std::sin (2 * dsp::kPi * 1300 * t)
                                                           + 0.25 * std::sin (2 * dsp::kPi * 5000 * t));
                            sum += (double) l.process (x) * (1.0 + (i % 7));
                        }
                    }
                return sum;
            };
            const double off = fingerprint (false);
            check (std::fabs (off - 143.141899516) < 1.0e-6, "ladder with stage saturation OFF is unchanged", off, 143.141899516);
            const auto level = [&] (bool stages, float amp)
            {
                dsp::LadderFilter l; l.prepare (sr); l.set (2000.0, 0.0f); l.setStageSaturation (stages);
                double e = 0, ph = 0;
                for (int i = 0; i < 20000; ++i)
                { const float y = l.process (amp * (float) std::sin (ph)); ph += 2.0 * dsp::kPi * 100.0 / sr; if (i > 10000) e += (double) y * y; }
                return 10.0 * std::log10 (e / 10000.0);
            };
            const double quiet = level (true, 0.05f) - level (false, 0.05f), loud = level (true, 1.3f) - level (false, 1.3f);
            check (std::fabs (quiet) < 0.2, "stage saturation leaves a quiet signal alone (dB)", quiet, 0.0);
            check (loud < -1.0, "stage saturation compresses a loud one (dB)", loud, -1.0);
        }
        {
            // Past unity loop gain with no input at all it must ring, stay
            // finite, and be at the cutoff - not at DC and not at Nyquist.
            dsp::LadderFilter l; l.prepare (sr); l.set (500.0, 1.15f);
            double peak = 0; int zc = 0; float prev = 0;
            for (int i = 0; i < (int) sr; ++i)
            {
                const float y = l.process (i < 4 ? 0.5f : 0.0f);
                if (i > (int) sr / 2)
                { peak = std::max (peak, (double) std::fabs (y));
                  if ((y > 0) != (prev > 0)) ++zc; prev = y; }
            }
            const double hz = zc / 2.0 / 0.5;
            check (std::isfinite (peak) && peak > 0.01, "ladder self-oscillates past unity", peak, 0.01);
            check (peak < 10.0, "ladder self-oscillation stays bounded", peak, 10.0);
            check (std::fabs (hz - 500.0) < 120.0, "ladder self-oscillates at its cutoff", hz, 500.0);
        }

        // a comb must RING, at the pitch it was tuned to, for the time asked for
        dsp::CombFilter c; c.prepare (sr, 0.2);
        c.setTuning (200.0); c.setDamping (20000.0); c.setDecaySeconds (1.0);
        std::vector<float> co;
        for (int i = 0; i < 96000; ++i) co.push_back (c.process (i < 8 ? 1.0f : 0.0f));
        // 60 dB in 1 s: energy at 1 s should be about 1e-6 of the energy at the
        // start, and there should still be something there at all.
        auto rms = [&] (int from, int len) { double e = 0; for (int i = from; i < from + len; ++i) e += (double) co[i] * co[i]; return std::sqrt (e / len); };
        const double r0 = rms (240, 2400), r1s = rms (48000, 2400);
        const double fell = 20.0 * std::log10 (r1s / std::max (r0, 1e-12));
        check (fell < -50.0 && fell > -75.0, "a comb set to decay in 1 s falls about 60 dB in 1 s", fell, -60.0);
        check (std::fabs (dominantHz (co, sr) - 200.0) < 12.0, "and it rings at the tuned pitch", dominantHz (co, sr), 200.0);

        // A comb is not a delay, and the difference is the SPACING of the
        // repeats: under about 30 ms the ear fuses them into a pitch, over
        // about 80 ms it counts them as echoes. Same block, same feedback,
        // two entirely different effects, which is why "Swept Combs" and
        // "Swept Delays" are different algorithms on these machines.
        auto repeatSpacingMs = [&] (double delaySec)
        {
            dsp::CombFilter k; k.prepare (sr, 1.0); k.setDelaySeconds (delaySec);
            k.setFeedback (0.8f); k.setDamping (20000.0);
            std::vector<float> a;
            for (int i = 0; i < 96000; ++i) a.push_back (k.process (i == 0 ? 1.0f : 0.0f));
            int first = -1, second = -1;
            for (int i = 1; i < 96000; ++i)
                if (std::fabs (a[i]) > 0.2f) { if (first < 0) first = i; else if (i > first + 8) { second = i; break; } }
            return (first < 0 || second < 0) ? 0.0 : (second - first) * 1000.0 / sr;
        };
        const double shortSp = repeatSpacingMs (0.005), longSp = repeatSpacingMs (0.5);
        check (shortSp > 0 && shortSp < 30.0, "a 5 ms comb repeats fast enough to fuse into a pitch", shortSp, 5.0);
        check (longSp > 80.0, "the same block at 500 ms is an echo, not a comb", longSp, 500.0);
    }

    // ---- drive: does it add harmonics, and only when driven? --------------
    std::printf ("Drive\n");
    {
        dsp::SoftClip sc; sc.setThreshold (1.0f); sc.setDrive (1.0f);
        double quietErr = 0;
        for (int i = 0; i < 1000; ++i)
        { const float v = 0.01f * (float) std::sin (i * 0.1); quietErr += std::fabs (sc.process (v) - v); }
        check (quietErr / 1000.0 < 1e-4, "soft clip is transparent well below threshold", quietErr / 1000.0, 0);

        // hard drive on a sine must produce energy at 3f that was not there
        auto thirdHarmonic = [&] (float amp, float drive)
        {
            dsp::SoftClip s; s.setThreshold (1.0f); s.setDrive (drive);
            // f sits exactly on a DFT bin (bin 128 of 8192 at 48 kHz), so 3f
            // lands exactly on bin 384 and neither reading is spectral leakage
            // from the fundamental. At 500 Hz the leakage alone read as 0.1%
            // distortion on a stage that was doing nothing.
            double re = 0, im = 0, fund = 0, fim = 0; const int N = 8192; const double f = sr * 128.0 / N;
            for (int i = 0; i < N; ++i)
            { const float y = s.process (amp * (float) std::sin (2.0 * dsp::kPi * f * i / sr));
              const double w3 = 2.0 * dsp::kPi * 3.0 * f * i / sr, w1 = 2.0 * dsp::kPi * f * i / sr;
              re += y * std::cos (w3); im -= y * std::sin (w3);
              fund += y * std::cos (w1); fim -= y * std::sin (w1); }
            const double h3 = std::sqrt (re * re + im * im), h1 = std::sqrt (fund * fund + fim * fim);
            return h3 / std::max (h1, 1e-9);
        };
        // Third-harmonic distortion as a FRACTION of the fundamental: near zero
        // when the stage is not being asked for more than it has, and large when
        // it is. That is what "transparent until driven" has to mean.
        check (thirdHarmonic (0.02f, 1.0f) < 1e-3, "an undriven soft clip makes almost no third harmonic", thirdHarmonic (0.02f, 1.0f), 0);
        check (thirdHarmonic (1.0f, 8.0f) > 0.15, "driving hard makes a large third harmonic", thirdHarmonic (1.0f, 8.0f), 0.15);

        // bit crush: 4 bits must quantise to visibly few distinct values
        dsp::BitCrusher bc; bc.prepare (sr); bc.setBits (4); bc.setRateDivider (1);
        std::vector<float> vals;
        for (int i = 0; i < 4000; ++i) vals.push_back (bc.process ((float) std::sin (i * 0.01)));
        std::sort (vals.begin(), vals.end());
        vals.erase (std::unique (vals.begin(), vals.end()), vals.end());
        check ((int) vals.size() <= 20, "4-bit crush yields few distinct levels", (int) vals.size(), 20);
    }

    // ---- reverb tank: does it decay in the time it was asked for? ---------
    std::printf ("ReverbTank\n");
    {
        auto measureRt60 = [&] (dsp::ReverbTopology topo, double want)
        {
            dsp::ReverbTank t;
            t.prepare (sr, topo);
            t.setDecaySeconds (want); t.setLowDecayMultiplier (1.0);
            t.setCrossover (20.0); t.setDamping (20000.0); t.setHighCut (20000.0);
            t.setSize (1.0); t.setDiffusion (0.7); t.setSpin (0.0); t.setWander (0.0);
            std::vector<float> env;
            for (int i = 0; i < (int) (sr * 12); ++i)
            { float a, b; t.process (i < 64 ? 1.0f : 0.0f, i < 64 ? 1.0f : 0.0f, a, b); env.push_back (std::fabs (a) + std::fabs (b)); }
            // smooth, then fit the slope between -20 and -50 dB
            std::vector<double> sm (env.size(), 0.0); double acc = 0;
            for (size_t i = 0; i < env.size(); ++i) { acc += 0.0005 * (env[i] - acc); sm[i] = acc; }
            double peak = 0; size_t peakAt = 0;
            for (size_t i = 0; i < sm.size(); ++i) if (sm[i] > peak) { peak = sm[i]; peakAt = i; }
            if (peak <= 0) return -1.0;
            long i20 = -1, i50 = -1;
            for (size_t i = peakAt; i < sm.size(); ++i)
            { const double db = 20.0 * std::log10 (std::max (sm[i], 1e-12) / peak);
              if (i20 < 0 && db < -20.0) i20 = (long) i;
              if (i50 < 0 && db < -50.0) { i50 = (long) i; break; } }
            if (i20 < 0 || i50 < 0) return -1.0;
            return 60.0 * ((double) (i50 - i20) / sr) / 30.0;
        };
        // EVERY supplied topology, not just the hall. Testing one of them let a
        // real bug ship: `room` and `ambience` set crossCoupling below 1, which
        // scaled the cross-feedback path and threw away loop gain the decay law
        // knew nothing about, and `room` decayed in 0.16 of the time it was
        // asked for at 4 seconds. The hall passed the whole time, because its
        // coupling is 1.
        struct Topo { const char* name; dsp::ReverbTopology t; };
        const Topo topos[] = {
            { "hall", dsp::topologies::hall() },       { "plate", dsp::topologies::plate() },
            { "room", dsp::topologies::room() },       { "chamber", dsp::topologies::chamber() },
            { "ambience", dsp::topologies::ambience() },
            { "cathedral", dsp::topologies::cathedral() },
        };
        for (const auto& tp : topos)
            for (double want : { 0.6, 2.0, 5.0 })
            {
                const double got = measureRt60 (tp.t, want);
                char msg[128];
                std::snprintf (msg, sizeof msg, "%s asked for %.1f s decays in about that", tp.name, want);
                check (got > 0 && std::fabs (got - want) / want < 0.25, msg, got, want);
            }

        // a plate and a hall must not be the same effect
        auto render = [&] (dsp::ReverbTopology topo)
        {
            dsp::ReverbTank t; t.prepare (sr, topo);
            t.setDecaySeconds (2.0); t.setDamping (8000.0); t.setHighCut (12000.0);
            t.setSize (1.0); t.setDiffusion (0.7); t.setSpin (0.5); t.setWander (8.0);
            std::vector<float> o;
            for (int i = 0; i < (int) (sr * 3); ++i)
            { float a, b; t.process (i < 32 ? 1.0f : 0.0f, i < 32 ? 1.0f : 0.0f, a, b); o.push_back (a); }
            return o;
        };
        const auto h = render (dsp::topologies::hall()), pl = render (dsp::topologies::plate());
        double num = 0, den = 0;
        for (size_t i = 0; i < h.size(); ++i) { const double d = h[i] - pl[i]; num += d * d; den += (double) h[i] * h[i] + (double) pl[i] * pl[i]; }
        check (den > 0 && num / den > 0.2, "a plate and a hall render differently", den > 0 ? num / den : 0, 0.2);
    }


    // ---- the figure-of-eight tank ----------------------------------------
    // A SECOND topology, not a replacement: nothing above changes because this
    // exists. What is tested here is the thing that was got wrong twice in the
    // Schroeder tank and would have been got wrong a third time - the decay law
    // and the loop time behind it - plus stability at the top of the range and
    // the modulation that is the reason to have this block at all.
    std::printf ("Figure8Tank\n");
    {
        auto rt60Of = [&] (dsp::Figure8Topology topo, double want, double sz)
        {
            dsp::Figure8Tank t;
            t.prepare (sr, topo);
            t.setDecaySeconds (want); t.setLowDecayMultiplier (1.0);
            t.setCrossover (20.0); t.setDamping (20000.0); t.setHighCut (20000.0);
            t.setBandwidth (20000.0);
            t.setSize (sz); t.setDiffusion (0.7);
            t.setModDepth (0.0);            // a wandering tap smears the fit
            std::vector<float> env;
            const int n = (int) (sr * std::max (14.0, want * 3.5));
            for (int i = 0; i < n; ++i)
            { const float x = i < 64 ? 1.0f : 0.0f; float a, b; t.process (x, x, a, b);
              env.push_back (std::fabs (a) + std::fabs (b)); }
            std::vector<double> sm (env.size(), 0.0); double acc = 0;
            for (size_t i = 0; i < env.size(); ++i) { acc += 0.0005 * (env[i] - acc); sm[i] = acc; }
            double peak = 0; size_t peakAt = 0;
            for (size_t i = 0; i < sm.size(); ++i) if (sm[i] > peak) { peak = sm[i]; peakAt = i; }
            if (peak <= 0) return -1.0;
            long i20 = -1, i50 = -1;
            for (size_t i = peakAt; i < sm.size(); ++i)
            { const double db = 20.0 * std::log10 (std::max (sm[i], 1e-12) / peak);
              if (i20 < 0 && db < -20.0) i20 = (long) i;
              if (i50 < 0 && db < -50.0) { i50 = (long) i; break; } }
            if (i20 < 0 || i50 < 0) return -1.0;
            return 60.0 * ((double) (i50 - i20) / sr) / 30.0;
        };

        struct Topo { const char* name; dsp::Figure8Topology t; };
        const Topo topos[] = {
            { "plate",   dsp::figure8::plate() },   { "hall",    dsp::figure8::hall() },
            { "room",    dsp::figure8::room() },    { "chamber", dsp::figure8::chamber() },
            { "ambience", dsp::figure8::ambience() }, { "cathedral", dsp::figure8::cathedral() },
        };
        for (const auto& tp : topos)
            for (double want : { 1.0, 2.0, 5.0 })
            {
                const double got = rt60Of (tp.t, want, 1.0);
                char msg[128];
                std::snprintf (msg, sizeof msg, "%s asked for %.1f s decays in about that", tp.name, want);
                check (got > 0 && std::fabs (got - want) / want < 0.15, msg, got, want);
            }

        // fitSizeForDecay: a short decay is unreachable at Size 1.0 on every one
        // of these circuits, and the tank has to shrink itself rather than
        // quietly return a tail three times too long.
        for (const auto& tp : topos)
            for (double want : { 0.4, 0.8 })
            {
                dsp::Figure8Tank t; t.prepare (sr, tp.t);
                t.setDamping (20000.0); t.setHighCut (20000.0);
                t.setDecaySeconds (want);
                t.fitSizeForDecay (want, 1.0);
                // Measure with the fitted size in place.
                std::vector<double> env; env.reserve ((size_t) (sr * 8));
                float a = 0.0f, b = 0.0f;
                for (int i = 0; i < (int) (sr * 8); ++i)
                { const float x = (i == 0) ? 1.0f : 0.0f; t.process (x, x, a, b);
                  env.push_back (std::fabs ((double) a) + std::fabs ((double) b)); }
                std::vector<double> sm (env.size(), 0.0); double acc = 0;
                for (size_t i = 0; i < env.size(); ++i) { acc += 0.0005 * (env[i] - acc); sm[i] = acc; }
                double peak = 0; size_t peakAt = 0;
                for (size_t i = 0; i < sm.size(); ++i) if (sm[i] > peak) { peak = sm[i]; peakAt = i; }
                long i20 = -1, i50 = -1;
                for (size_t i = peakAt; i < sm.size(); ++i)
                { const double db = 20.0 * std::log10 (std::max (sm[i], 1e-12) / peak);
                  if (i20 < 0 && db < -20.0) i20 = (long) i;
                  if (i50 < 0 && db < -50.0) { i50 = (long) i; break; } }
                const double got = (i20 >= 0 && i50 >= 0) ? 60.0 * ((double) (i50 - i20) / sr) / 30.0 : -1.0;
                char msg[128];
                std::snprintf (msg, sizeof msg, "%s reaches %.1f s once Size is fitted", tp.name, want);
                check (got > 0 && std::fabs (got - want) / want < 0.25, msg, got, want);
            }

        // 0.4 s is not merely "hard" at Size 1.0 on the long circuits, it is
        // unreachable, and the point of the fit is that it stops being so. If
        // this ever passes without fitting, the fit has stopped being needed and
        // the constant above wants re-measuring.
        {
            dsp::Figure8Tank t; t.prepare (sr, dsp::figure8::hall());
            t.setDamping (20000.0); t.setHighCut (20000.0);
            t.setDecaySeconds (0.4);
            t.setSize (1.0);
            float a = 0.0f, b = 0.0f; t.process (0.0f, 0.0f, a, b);
            check (t.loopSeconds() * 0.78 > 0.4 * 1.2,
                   "unfitted, a hall's circuit puts 0.4 s out of reach",
                   t.loopSeconds() * 0.70, 0.4);
        }

        // THE TRAP. The decay law has to count each allpass for more than its own
        // delay or short times are unreachable and the tail floors out. Here the
        // allpasses ARE the loop, so the effect is large and measurable: the
        // effective loop time must be well clear of the nominal one, and a short
        // decay must actually arrive. `room` at Size 0.35 has a circuit short
        // enough that 0.3 s is physically reachable - at Size 1.0 it is not, and
        // no arithmetic can make it so, which is what Size is for.
        {
            dsp::Figure8Tank t; t.prepare (sr, dsp::figure8::plate());
            t.setDecaySeconds (2.0); float a, b; t.process (0.0f, 0.0f, a, b);
            check (t.loopSeconds() > t.nominalLoopSeconds() * 1.2,
                   "an allpass in the loop counts for more than its own delay",
                   t.loopSeconds() / t.nominalLoopSeconds(), 1.2);
            const double s3 = rt60Of (dsp::figure8::room(), 0.3, 0.35);
            const double s6 = rt60Of (dsp::figure8::room(), 0.6, 0.35);
            check (s3 > 0 && std::fabs (s3 - 0.3) / 0.3 < 0.25, "a 0.3 s decay is reachable", s3, 0.3);
            check (s6 > 0 && std::fabs (s6 - 0.6) / 0.6 < 0.15, "and so is 0.6 s", s6, 0.6);
        }

        // Stability at the top of everything: infinite decay, maximum diffusion,
        // maximum size, maximum modulation, driven hard for thirty seconds.
        {
            dsp::Figure8Tank t; t.prepare (sr, dsp::figure8::hall());
            t.setInfinite (true); t.setDiffusion (1.0); t.setSize (2.5);
            t.setModDepth (8.0); t.setModRate (4.0);
            t.setDamping (20000.0); t.setHighCut (20000.0); t.setBandwidth (20000.0);
            // Two INDEPENDENT noise streams, not one and its inverse: the tank is
            // fed the mono sum, so anti-phase input is silence going in and the
            // test would pass on a block that did nothing at all.
            uint32_t sd = 12345; float worst = 0.0f; bool finite = true; double last = 0.0;
            auto noise = [&sd] { sd = sd * 1664525u + 1013904223u; return (float) ((int32_t) sd) / 2.147483648e9f; };
            for (int i = 0; i < (int) (sr * 30); ++i)
            {
                float a, b; t.process (noise(), noise(), a, b);
                if (! (std::isfinite (a) && std::isfinite (b))) finite = false;
                worst = std::max (worst, std::max (std::fabs (a), std::fabs (b)));
                if (i > (int) (sr * 29)) last += (double) a * a;
            }
            check (finite, "infinite decay at maximum everything stays finite", finite ? 1 : 0, 1);
            check (worst < 12.0f, "and bounded", worst, 12.0);
            check (last > 0.0, "and has not collapsed to silence", last, 1e-12);
        }

        // The modulation is ON unless a caller turns it off. A steady tone into a
        // static allpass tank comes out at a steady level, because a static tank
        // is linear and time-invariant; a modulated one keeps moving. That
        // standing-still is what a metallic tail sounds like, and the reason this
        // block does not gate its modulation on anything.
        {
            auto wanderDb = [&] (double depthMs)
            {
                dsp::Figure8Tank t; t.prepare (sr, dsp::figure8::hall());
                t.setDecaySeconds (2.5); t.setDamping (8000.0); t.setHighCut (12000.0);
                if (depthMs >= 0.0) t.setModDepth (depthMs);
                std::vector<double> env; double e = 0.0;
                const double a = 1.0 - std::exp (-1.0 / (0.03 * sr));
                for (int i = 0; i < (int) (sr * 8); ++i)
                {
                    const float x = (float) std::sin (2.0 * dsp::kPi * 1000.0 * i / sr);
                    float l, r; t.process (x, x, l, r);
                    e += a * (std::fabs ((double) l) - e);
                    if (i > (int) (sr * 3)) env.push_back (e);
                }
                double m = 0; for (double v : env) m += v; m /= (double) env.size();
                double v2 = 0; for (double v : env) v2 += (v - m) * (v - m);
                v2 = std::sqrt (v2 / (double) env.size());
                return m > 0 ? 20.0 * std::log10 (1.0 + v2 / m) : 0.0;
            };
            const double on = wanderDb (-1.0), off = wanderDb (0.0);
            check (on > 1.0, "the tank modulates without being asked to", on, 1.0);
            check (off < 0.2, "and holds still when it is asked to stop", off, 0.2);
        }

        // A plate and a hall must not be the same effect, and neither of them may
        // measure as a comb: this is an allpass structure, not a comb bank.
        {
            auto render = [&] (dsp::Figure8Topology topo)
            {
                dsp::Figure8Tank t; t.prepare (sr, topo);
                t.setDecaySeconds (2.5); t.setDamping (8000.0); t.setHighCut (12000.0);
                std::vector<float> o;
                for (int i = 0; i < (int) (sr * 10); ++i)
                { const float x = (i == 0) ? 1.0f : 0.0f; float a, b; t.process (x, x, a, b); o.push_back (a); }
                return o;
            };
            const auto pl = render (dsp::figure8::plate()), hl = render (dsp::figure8::hall());
            double num = 0, den = 0;
            for (size_t i = 0; i < pl.size(); ++i)
            { const double d = pl[i] - hl[i]; num += d * d; den += (double) pl[i] * pl[i] + (double) hl[i] * hl[i]; }
            check (den > 0 && num / den > 0.2, "a plate and a hall render differently", den > 0 ? num / den : 0, 0.2);
            check (dsp::measure::combSpacingMs (pl, sr) == 0.0,
                   "and a figure-of-eight tank has no comb spacing either",
                   dsp::measure::combSpacingMs (pl, sr), 0.0);
            // It also has to become diffuse. A tank that never fills in is a
            // multi-tap delay wearing a reverb's name.
            const double d80 = dsp::measure::echoDensity (pl, sr, 0.08);
            check (d80 > 0.6, "and it is diffuse by 80 ms", d80, 0.6);
        }
    }

    // ---- comb spacing: is a flanger a different effect from a chorus? ------
    // The spectrogram cannot answer this. Its bands are a fraction of an octave
    // wide and a comb 500 Hz apart falls inside one, so a flanger at 2 ms and a
    // chorus at 25 ms read as the same spectrum. The whole difference between
    // those two effects IS that number.
    std::printf ("Comb spacing\n");
    {
        auto ir = [&] (int kind)
        {
            std::vector<float> o;
            dsp::Flanger fl; fl.prepare (sr);
            fl.setDelayRange (1.4, 2.8); fl.setRate (0.3); fl.setDepth (1.0f);
            fl.setFeedback (0.5f); fl.setMix (0.5f);
            dsp::Chorus ch; ch.prepare (sr);
            ch.setVoices (2); ch.setRate (0.3); ch.setDelay (25.0, 3.0); ch.setMix (0.5f);
            dsp::ReverbTank tk; tk.prepare (sr, dsp::topologies::hall());
            tk.setDecaySeconds (2.0); tk.setDamping (8000.0); tk.setHighCut (12000.0);
            tk.setSize (1.0); tk.setDiffusion (0.7); tk.setSpin (0.5); tk.setWander (8.0);
            for (int i = 0; i < (int) (sr * 10); ++i)
            {
                const float x = (i == 0) ? 1.0f : 0.0f;
                if (kind == 0) o.push_back (fl.process (x));
                else if (kind == 1) { float a, b; ch.process (x, a, b); o.push_back (a); }
                else { float a, b; tk.process (x, x, a, b); o.push_back (a); }
            }
            return o;
        };
        const double fl = dsp::measure::analyse (ir (0), sr).combMs;
        const double ch = dsp::measure::analyse (ir (1), sr).combMs;
        const double hl = dsp::measure::analyse (ir (2), sr).combMs;
        check (fl > 1.0 && fl < 4.0, "a 2 ms flanger measures in the flanging window", fl, 2.0);
        check (ch > 20.0 && ch < 32.0, "a 25 ms chorus measures in the chorus window", ch, 25.0);
        check (hl == 0.0, "a reverb has no comb spacing at all", hl, 0.0);
        dsp::measure::Features a = dsp::measure::analyse (ir (0), sr);
        dsp::measure::Features b = dsp::measure::analyse (ir (1), sr);
        check (dsp::measure::difference (a, b) > 0.15,
               "and a flanger and a chorus therefore read as different effects",
               dsp::measure::difference (a, b), 0.15);
    }

    // ---- dynamics: does the compressor actually reduce gain? --------------
    std::printf ("Dynamics\n");
    {
        dsp::Compressor c; c.prepare (sr);
        c.setThresholdDb (-20.0f); c.setRatio (8.0f); c.setKneeDb (0.0f); c.setTimes (1.0, 50.0);
        double loudOut = 0, quietOut = 0;
        for (int i = 0; i < 24000; ++i) { const float v = 1.0f * (float) std::sin (2.0 * dsp::kPi * 200.0 * i / sr); const float y = c.process (v); if (i > 12000) loudOut += (double) y * y; }
        c.reset();
        for (int i = 0; i < 24000; ++i) { const float v = 0.01f * (float) std::sin (2.0 * dsp::kPi * 200.0 * i / sr); const float y = c.process (v); if (i > 12000) quietOut += (double) y * y; }
        const double loudRms = std::sqrt (loudOut / 12000.0), quietRms = std::sqrt (quietOut / 12000.0);
        // input differs by 40 dB; at 8:1 above threshold the output should differ by far less
        const double outDb = 20.0 * std::log10 (loudRms / std::max (quietRms, 1e-9));
        check (outDb < 25.0 && outDb > 2.0, "40 dB in becomes much less than 40 dB out at 8:1", outDb, 40.0);

        dsp::Ducker d; d.prepare (sr); d.setThresholdDb (-30.0f); d.setDepthDb (-18.0f); d.setTimes (1.0, 50.0);
        float ducked = 1.0f;
        for (int i = 0; i < 24000; ++i) ducked = d.process (1.0f, 1.0f);
        check (ducked < 0.2f, "a loud key ducks the wet path", ducked, 0.2);
        for (int i = 0; i < 48000; ++i) ducked = d.process (1.0f, 0.0f);
        check (ducked > 0.95f, "and it comes back when the key stops", ducked, 1.0);
    }

    // ---- sampler and stutter ----------------------------------------------
    std::printf ("Sampler\n");
    {
        dsp::Sampler s; s.prepare (sr, 1.0);
        s.record();
        for (int i = 0; i < 4800; ++i) s.process ((float) std::sin (2.0 * dsp::kPi * 440.0 * i / sr));
        s.stop();
        check (std::fabs (s.recordedSeconds() - 0.1) < 0.005, "recorded the length it was fed", s.recordedSeconds(), 0.1);
        s.setLoop (true); s.setSpeed (1.0); s.setReverse (false); s.setWindow (0.0, 1.0);
        s.play();
        std::vector<float> out;
        for (int i = 0; i < 40000; ++i) out.push_back (s.process (0.0f));
        check (std::fabs (dominantHz (out, sr) - 440.0) < 12.0, "plays back at the recorded pitch", dominantHz (out, sr), 440.0);

        // half speed is an octave down
        s.setSpeed (0.5); s.play();
        std::vector<float> half;
        for (int i = 0; i < 40000; ++i) half.push_back (s.process (0.0f));
        check (std::fabs (dominantHz (half, sr) - 220.0) < 8.0, "half speed is an octave down", dominantHz (half, sr), 220.0);

        // stutter must repeat: a slice heard more than once
        dsp::Stutter st; st.prepare (sr, 1.0);
        st.setSliceSeconds (0.05); st.setRepeats (4);
        std::vector<float> so;
        // mid-slice, so the impulse is not sitting on the splice crossfade
        for (int i = 0; i < 48000; ++i) so.push_back (st.process (i % 2400 == 1200 ? 1.0f : 0.0f));
        int spikes = 0; for (float v : so) if (std::fabs (v) > 0.4f) ++spikes;
        // 4 capture/play cycles of 1 pass-through plus 4 repeats each
        check (spikes >= 12, "stutter repeats its slices", spikes, 12);
    }

    // ---- diatonic shift: the interval follows the input note --------------
    std::printf ("DiatonicShifter\n");
    {
        // In C major a third above C is E (+4 semitones); above D it is F (+3).
        auto ratioFor = [&] (double hz)
        {
            dsp::DiatonicShifter d; d.prepare (sr);
            d.setScale (dsp::Scale::major (0)); d.setSteps (2); d.setWindow (0.05);
            for (int i = 0; i < (int) (sr * 0.4); ++i) d.process ((float) std::sin (2.0 * dsp::kPi * hz * i / sr));
            return d.currentRatio();
        };
        const double rC = ratioFor (261.63), rD = ratioFor (293.66);
        const double semC = 12.0 * std::log2 (rC), semD = 12.0 * std::log2 (rD);
        check (std::fabs (semC - 4.0) < 0.6, "a third above C is a major third", semC, 4.0);
        check (std::fabs (semD - 3.0) < 0.6, "a third above D is a minor third", semD, 3.0);
    }

    std::printf ("\n%s\n", failures == 0 ? "all blocks behave" : "SOME BLOCKS ARE NOT WHAT THEY CLAIM");
    return failures == 0 ? 0 : 1;
}
