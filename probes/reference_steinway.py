#!/usr/bin/env python3
"""MEASURE A REAL PIANO, so the model has something to be wrong against.

Six milestones were built with no reference at all: every judgement about whether
the thing sounded right was opinion against opinion. This reads the University of
Iowa Electronic Music Studios' Steinway recordings - free since 1997, no
restrictions - and extracts the numbers the model claims.

    python3 probes/reference_steinway.py reference/steinway

THE FIT HAS TO ITERATE, and that is not a detail. A partial on a stiff string sits
at n*f0*sqrt(1 + B*n^2), so at B = 1e-3 partial 24 is 25 per cent sharp. Searching
a fixed window around the HARMONIC finds the wrong peak and fits a NEGATIVE B,
which is physically impossible and is exactly what the first pass here reported.
So: predict from the current B, search near the prediction, refit, repeat.
"""
import sys, wave, glob, os
import numpy as np

NOTE = {'A0':27.5,'A1':55.0,'A2':110.0,'A3':220.0,'A4':440.0,'A5':880.0,
        'A6':1760.0,'A7':3520.0,'C4':261.6,'C5':523.3}

def load(p):
    w = wave.open(p, 'rb')
    d = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(float) / 32768
    return d, w.getframerate()

def analyse(d, sr, f0n, nmax=26, minPartials=None):
    # THE WINDOW HAS TO SUIT THE NOTE. A treble note is gone in about a second, so a
    # 1.5 s window analyses mostly silence; a bass note needs the length to resolve
    # closely spaced partials. And a high string has only a handful of partials below
    # Nyquist at all - A7 has six - so demanding eight of them meant the fit never
    # converged and returned B = 0.000000 for the entire top two octaves. That was the
    # probe failing, not the piano being harmonic.
    if minPartials is None:
        minPartials = 8 if f0n < 500 else (6 if f0n < 1500 else 4)
    a = int(sr * (0.05 if f0n > 800 else 0.15))
    N = min(int(sr * (0.6 if f0n > 800 else 1.5)), len(d) - a)
    x = d[a:a+N] * np.hanning(N)
    mag = np.abs(np.fft.rfft(x)); fr = np.fft.rfftfreq(N, 1/sr); peak = mag.max()
    B, f0, ps = 0.0, f0n, []
    for _ in range(8):
        ps = []
        for n in range(1, nmax+1):
            pred = n * f0 * np.sqrt(1 + B*n*n)
            if pred > sr * 0.47: break
            half = max(f0 * 0.35, pred * 0.012)
            sel = (fr >= pred-half) & (fr <= pred+half)
            if not sel.any(): continue
            i = np.argmax(mag[sel])
            if mag[sel][i] > peak * 3e-4:
                ps.append((n, fr[sel][i], 20*np.log10(mag[sel][i]/peak)))
        if len(ps) < minPartials: break
        xx = np.array([n*n for n,_,_ in ps], float)
        yy = np.array([(f/n)**2 for n,f,_ in ps], float)
        slope, inter = np.linalg.lstsq(np.vstack([xx, np.ones_like(xx)]).T, yy, rcond=None)[0]
        f0 = np.sqrt(max(inter, 1e-9)); B = max(0.0, slope / max(inter, 1e-9))
    return B, f0, ps

def main(folder):
    print("\n  A REAL STEINWAY\n")
    print(f"  {'file':20} {'f0 Hz':>9} {'B':>11}   strongest dips against neighbours")
    for p in sorted(glob.glob(os.path.join(folder, '*.wav'))):
        base = os.path.basename(p)[:-4]
        note = base.split('.')[-1]
        if note not in NOTE: continue
        d, sr = load(p)
        B, f0, ps = analyse(d, sr, NOTE[note])
        db = {n: v for n,_,v in ps}
        dips = [(n, (db[n-1]+db[n+1])/2 - db[n]) for n in range(3, 16)
                if n-1 in db and n in db and n+1 in db]
        dips.sort(key=lambda t: -t[1])
        tail = ", ".join(f"n={n} {v:.0f}dB" for n, v in dips[:3])
        print(f"  {base:20} {f0:9.2f} {B:11.6f}   {tail}")
    print()

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'reference/steinway')
