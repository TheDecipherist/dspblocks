#!/usr/bin/env python3
"""Trim each piano recording to what the loader actually keeps, and store it as FLAC.

The loader finds the onset, backs off 5 ms, and keeps 8 seconds from there. Everything before
the onset and after the cap is read and thrown away on every load, so it is pure disk: the
Steinway's files are 35 seconds and 8 are used.

This does the same arithmetic offline, so the loaded result is unchanged. The onset threshold
and the run-up are the loader's own (Source/Steinway.cpp), deliberately duplicated here rather
than guessed - if that law changes, this script changes with it.
"""
import os, sys, subprocess, wave, struct, math

THRESH = 0.02          # the loader's onset threshold, 2 per cent
RUNUP  = 0.005         # 5 ms kept before the strike
CAP    = 8.0           # Steinway.h kMaxSeconds

def onset_of(path):
    w = wave.open(path, 'rb')
    sr, ch, sw, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
    scan = min(n, int(sr * 6))                  # the loader scans the head only
    data = w.readframes(scan); w.close()
    if sw != 2: return 0.0, sr
    cnt = len(data) // 2
    v = struct.unpack('<%dh' % cnt, data[:cnt * 2])
    lim = int(THRESH * 32768)
    for i in range(0, len(v), ch):
        if any(abs(v[i + c]) > lim for c in range(min(ch, len(v) - i))):
            return max(0.0, i / ch / sr - RUNUP), sr
    return 0.0, sr

def convert(src, dst):
    start, sr = onset_of(src)
    r = subprocess.run(['ffmpeg', '-v', 'quiet', '-y', '-ss', '%.4f' % start,
                        '-t', '%.3f' % CAP, '-i', src, '-c:a', 'flac', dst], check=False)
    return r.returncode == 0 and os.path.exists(dst)

folder = sys.argv[1]
keep = sys.argv[2] if len(sys.argv) > 2 else None    # only this dynamic, for a one-layer set
ok = skipped = 0
for f in sorted(os.listdir(folder)):
    if not f.endswith('.wav') or not f.startswith('Piano.'): continue
    dyn = f.split('.')[1]
    if keep and dyn != keep:
        skipped += 1
        continue
    src = os.path.join(folder, f)
    dst = src[:-4] + '.flac'
    if convert(src, dst): ok += 1
print('  %-34s %d written, %d dynamics dropped' % (folder.split('/')[-1], ok, skipped))
