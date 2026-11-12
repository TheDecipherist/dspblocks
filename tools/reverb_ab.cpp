// Renders the same signal through BOTH reverb topologies and writes the pair
// out, because the question this library cannot answer on its own is which one
// sounds better and that is decided by listening, not by a test.
//
//     cmake --build build --target dsp_reverb_ab
//     ./build/dsp_reverb_ab [output directory]
//
// For each room it writes two 32-bit float stereo WAVs at 48 kHz -
// `<room>-schroeder.wav` and `<room>-figure8.wav` - fed the same impulse, the
// same noise burst and the same tone burst, with the two tanks asked for the
// SAME decay time, damping and high cut. Everything that differs between the
// two files is structure.
//
// It also prints what can be measured, so the listening starts from numbers:
// RT60 against what was asked for, normalised echo density at four instants
// (how fast the tank fills in), and how far a steady tone's level wanders
// (whether the tank is standing still, which is what a metallic tail is).
#include "dsp/Blocks.h"
#include "dsp/Constants.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>

namespace {

void writeU32 (std::FILE* f, uint32_t v) { std::fwrite (&v, 4, 1, f); }
void writeU16 (std::FILE* f, uint16_t v) { std::fwrite (&v, 2, 1, f); }

/** 32-bit float stereo WAV: no quantisation to argue about afterwards. */
bool writeWav (const std::string& path, const std::vector<float>& l,
               const std::vector<float>& r, double sr)
{
    std::FILE* f = std::fopen (path.c_str(), "wb");
    if (f == nullptr) return false;
    const uint32_t frames = (uint32_t) std::min (l.size(), r.size());
    const uint32_t dataBytes = frames * 2 * 4;
    std::fwrite ("RIFF", 1, 4, f); writeU32 (f, 36 + dataBytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); writeU32 (f, 16);
    writeU16 (f, 3); writeU16 (f, 2);                       // IEEE float, stereo
    writeU32 (f, (uint32_t) sr); writeU32 (f, (uint32_t) sr * 2 * 4);
    writeU16 (f, 8); writeU16 (f, 32);
    std::fwrite ("data", 1, 4, f); writeU32 (f, dataBytes);
    for (uint32_t i = 0; i < frames; ++i) { std::fwrite (&l[i], 4, 1, f); std::fwrite (&r[i], 4, 1, f); }
    std::fclose (f);
    return true;
}

/** The test signal: an impulse, then a noise burst, then a tone burst, each
    followed by enough silence to hear the tail out. An impulse alone tells the
    truth about the structure and almost nothing about the sound. */
std::vector<float> testSignal (double sr)
{
    const int gap = (int) (sr * 3.0);
    std::vector<float> x;
    x.push_back (1.0f);
    x.resize (x.size() + gap, 0.0f);
    uint32_t sd = 987654321u;
    for (int i = 0; i < (int) (sr * 0.03); ++i)
    { sd = sd * 1664525u + 1013904223u; x.push_back (0.7f * (float) ((int32_t) sd) / 2.147483648e9f); }
    x.resize (x.size() + gap, 0.0f);
    for (int i = 0; i < (int) (sr * 0.25); ++i)
    { const double t = i / sr, env = std::min (1.0, t / 0.005) * std::exp (-t * 6.0);
      x.push_back ((float) (0.7 * env * std::sin (2.0 * dsp::kPi * 440.0 * t))); }
    x.resize (x.size() + gap, 0.0f);
    return x;
}

double rt60 (const std::vector<float>& mono, double sr)
{
    std::vector<double> sm (mono.size(), 0.0); double acc = 0;
    for (size_t i = 0; i < mono.size(); ++i) { acc += 0.0005 * (std::fabs ((double) mono[i]) - acc); sm[i] = acc; }
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
}

/** How far a steady tone's output level wanders, in dB. A linear time-invariant
    tank settles on one level and reads ~0; anything above a decibel is a tank
    that keeps moving. */
template <typename Tank>
double toneWander (Tank& t, double sr)
{
    std::vector<double> env; double e = 0.0;
    const double a = 1.0 - std::exp (-1.0 / (0.03 * sr));
    for (int i = 0; i < (int) (sr * 8); ++i)
    {
        const float x = (float) std::sin (2.0 * dsp::kPi * 1000.0 * i / sr);
        float l, r; t.process (x, x, l, r);
        e += a * (std::fabs ((double) l) - e);
        if (i > (int) (sr * 3)) env.push_back (e);
    }
    if (env.empty()) return 0.0;
    double m = 0; for (double v : env) m += v; m /= (double) env.size();
    double v2 = 0; for (double v : env) v2 += (v - m) * (v - m);
    return m > 0 ? 20.0 * std::log10 (1.0 + std::sqrt (v2 / (double) env.size()) / m) : 0.0;
}

constexpr double kDecay = 2.5, kDamp = 8000.0, kHighCut = 12000.0, kCrossover = 500.0;

/** A clean impulse response at the same settings, for the numbers. */
std::vector<float> impulseRender (const dsp::ReverbTopology& topo, double sr)
{
    dsp::ReverbTank t; t.prepare (sr, topo);
    t.setDecaySeconds (kDecay); t.setDamping (kDamp); t.setHighCut (kHighCut); t.setCrossover (kCrossover);
    std::vector<float> o;
    for (int i = 0; i < (int) (sr * 12); ++i)
    { const float x = i == 0 ? 1.0f : 0.0f; float l, r; t.process (x, x, l, r); o.push_back (0.5f * (l + r)); }
    return o;
}
std::vector<float> impulseRender (const dsp::Figure8Topology& topo, double sr)
{
    dsp::Figure8Tank t; t.prepare (sr, topo);
    t.setDecaySeconds (kDecay); t.setDamping (kDamp); t.setHighCut (kHighCut); t.setCrossover (kCrossover);
    std::vector<float> o;
    for (int i = 0; i < (int) (sr * 12); ++i)
    { const float x = i == 0 ? 1.0f : 0.0f; float l, r; t.process (x, x, l, r); o.push_back (0.5f * (l + r)); }
    return o;
}

} // namespace

int main (int argc, char** argv)
{
    const double sr = 48000.0;
    std::string dir = argc > 1 ? argv[1] : ".";
    if (! dir.empty() && dir.back() != '/') dir += '/';

    const auto sig = testSignal (sr);

    struct Pair { const char* name; dsp::ReverbTopology a; dsp::Figure8Topology b; };
    const Pair pairs[] = {
        { "plate",   dsp::topologies::plate(),   dsp::figure8::plate() },
        { "hall",    dsp::topologies::hall(),    dsp::figure8::hall() },
        { "room",    dsp::topologies::room(),    dsp::figure8::room() },
        { "chamber", dsp::topologies::chamber(), dsp::figure8::chamber() },
    };

    std::printf ("Both tanks asked for the same thing: decay %.1f s, damping %.0f Hz, "
                 "high cut %.0f Hz, crossover %.0f Hz.\n\n", kDecay, kDamp, kHighCut, kCrossover);
    std::printf ("%-9s %-11s %7s %8s %8s %8s %8s %8s\n",
                 "room", "tank", "RT60", "d(20ms)", "d(40ms)", "d(80ms)", "d(160ms)", "wander");

    for (const auto& p : pairs)
    {
        // --- the Schroeder chain ---------------------------------------------
        std::vector<float> al, ar, mono;
        {
            dsp::ReverbTank t; t.prepare (sr, p.a);
            t.setDecaySeconds (kDecay); t.setDamping (kDamp); t.setHighCut (kHighCut);
            t.setCrossover (kCrossover); t.setLowDecayMultiplier (1.2);
            for (float x : sig) { float l, r; t.process (x, x, l, r); al.push_back (l); ar.push_back (r); }
            // Measure on an impulse-only render. Measuring on the composite
            // signal above reads the noise burst as a retrigger and returns a
            // decay time that belongs to no part of it.
            mono = impulseRender (p.a, sr);
            dsp::ReverbTank w; w.prepare (sr, p.a);
            w.setDecaySeconds (kDecay); w.setDamping (kDamp); w.setHighCut (kHighCut); w.setCrossover (kCrossover);
            std::printf ("%-9s %-11s %7.2f %8.3f %8.3f %8.3f %8.3f %7.2f dB\n", p.name, "schroeder",
                         rt60 (mono, sr),
                         dsp::measure::echoDensity (mono, sr, 0.020),
                         dsp::measure::echoDensity (mono, sr, 0.040),
                         dsp::measure::echoDensity (mono, sr, 0.080),
                         dsp::measure::echoDensity (mono, sr, 0.160),
                         toneWander (w, sr));
            if (! writeWav (dir + p.name + "-schroeder.wav", al, ar, sr)) { std::fprintf (stderr, "cannot write into %s - does it exist?\n", dir.c_str()); return 1; }
        }

        // --- the figure of eight ---------------------------------------------
        {
            std::vector<float> bl, br;
            dsp::Figure8Tank t; t.prepare (sr, p.b);
            t.setDecaySeconds (kDecay); t.setDamping (kDamp); t.setHighCut (kHighCut);
            t.setCrossover (kCrossover); t.setLowDecayMultiplier (1.2);
            for (float x : sig) { float l, r; t.process (x, x, l, r); bl.push_back (l); br.push_back (r); }
            mono = impulseRender (p.b, sr);
            dsp::Figure8Tank w; w.prepare (sr, p.b);
            w.setDecaySeconds (kDecay); w.setDamping (kDamp); w.setHighCut (kHighCut); w.setCrossover (kCrossover);
            std::printf ("%-9s %-11s %7.2f %8.3f %8.3f %8.3f %8.3f %7.2f dB\n", p.name, "figure8",
                         rt60 (mono, sr),
                         dsp::measure::echoDensity (mono, sr, 0.020),
                         dsp::measure::echoDensity (mono, sr, 0.040),
                         dsp::measure::echoDensity (mono, sr, 0.080),
                         dsp::measure::echoDensity (mono, sr, 0.160),
                         toneWander (w, sr));
            if (! writeWav (dir + p.name + "-figure8.wav", bl, br, sr)) { std::fprintf (stderr, "cannot write into %s - does it exist?\n", dir.c_str()); return 1; }
        }
    }

    std::printf ("\nWritten to %s. Levels are NOT matched between the two - a tank's own\n"
                 "output gain is the unit's business - so trim before deciding.\n", dir.c_str());
    return 0;
}
