#!/usr/bin/env python3
"""Check the pipeline without a speaker: a simulated room with a known response.

Run from work/ after gen_sweep.py.  Plays sweep.wav through known resonances
and a delay, adds noise, and checks that analyze.py finds the response and
fit.py corrects it.
"""
import numpy as np
from scipy.io import wavfile
from scipy.signal import fftconvolve, lfilter

import analyze
import fit

FS = 44100
# (fc, gain dB, Q).  Different per channel, so a swapped channel fails.
ROOM = {"L": [(190, 12, 4), (600, -5, 2)], "R": [(100, 15, 5), (250, 8, 3)]}
TOL_DB = 1.5


def peak_sos(fc, gain, q):
    A = 10 ** (gain / 40)
    w0 = 2 * np.pi * fc / FS
    al, c = np.sin(w0) / (2 * q), np.cos(w0)
    return [1 + al * A, -2 * c, 1 - al * A], [1 + al / A, -2 * c, 1 - al / A]


def room(x, ch):
    for fc, g, q in ROOM[ch]:
        x = lfilter(*peak_sos(fc, g, q), x)
    # A floor reflection, as a real room has.
    d = int(0.004 * FS)
    return fftconvolve(x, np.r_[1.0, np.zeros(d - 1), 0.2])[: len(x)]


_, s = wavfile.read("sweep.wav")
s = s.astype(float) / 32768
rec = room(s[:, 0], "L") + room(s[:, 1], "R")
rec = np.r_[np.zeros(int(4.2 * FS)), rec, np.zeros(FS)] * 0.1  # sender latency, far mic
rec += np.random.default_rng(1).normal(0, 10 ** (-70 / 20), len(rec))
wavfile.write("rec-selftest.wav", FS, (np.clip(rec, -1, 1) * 32767).astype(np.int16))

res, peaks = analyze.analyze("rec-selftest.wav")
assert len(peaks) == 6, peaks
g = analyze.GRID
failed = 0
for ch, bands in ROOM.items():
    resp = res[ch]
    ref = np.nanmean(resp[(g > 1000) & (g < 2000)])
    for fc, gain, _ in bands:
        got = resp[np.argmin(abs(g - fc))] - ref
        # 1/6 octave smoothing lowers a narrow resonance, so only a bound there.
        ok = abs(got) >= abs(gain) * 0.5 and np.sign(got) == np.sign(gain) and abs(got) <= abs(gain) + TOL_DB
        print(f"{ch} {fc} Hz: built {gain:+d} dB, measured {got:+.1f} dB {'ok' if ok else 'FAIL'}")
        failed += not ok
    filters, _, err = fit.fit_channel(g, resp)
    inb = (g >= fit.LO) & (g <= fit.HI)
    before = np.sqrt(np.mean(err[inb] ** 2))
    after = np.sqrt(np.mean((err + fit.model(filters, g))[inb] ** 2))
    ok = after < before / 2
    print(f"{ch}: {len(filters)} filters, error {before:.1f} -> {after:.1f} dB {'ok' if ok else 'FAIL'}")
    failed += not ok
print("measure selftest passed" if not failed else f"measure selftest FAILED ({failed})")
raise SystemExit(1 if failed else 0)
