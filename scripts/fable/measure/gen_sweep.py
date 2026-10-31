#!/usr/bin/env python3
"""Exponential sine sweeps for measuring the speaker through AirPlay.

Layout (44.1 kHz stereo): per repetition, L-only, R-only, then both, each a
SWEEP_S sweep followed by GAP_S of silence.  Writes sweep.wav and the inverse
filter (inverse.npy) used by analyze.py, into the current directory.
"""
import numpy as np
from scipy.io import wavfile

FS = 44100
F1, F2 = 20.0, 20000.0
SWEEP_S = 8.0
GAP_S = 4.0
LEAD_S = 3.0
REPS = 2
AMP = 10 ** (-10 / 20)  # -10 dBFS

t = np.arange(int(SWEEP_S * FS)) / FS
R = np.log(F2 / F1)
sweep = np.sin(2 * np.pi * F1 * SWEEP_S / R * (np.exp(t * R / SWEEP_S) - 1))
fade = int(0.02 * FS)
win = np.ones_like(sweep)
win[:fade] = 0.5 - 0.5 * np.cos(np.pi * np.arange(fade) / fade)
win[-fade:] = win[:fade][::-1]
sweep *= win * AMP

# Farina inverse: time-reversed sweep, -6 dB/octave envelope.
inverse = sweep[::-1] * np.exp(-t * R / SWEEP_S)
inverse /= np.abs(np.fft.rfft(np.convolve(sweep, inverse)[: 2 * len(sweep)])).max()

slot = int((SWEEP_S + GAP_S) * FS)
out = np.zeros((int(LEAD_S * FS) + REPS * 3 * slot, 2))
starts = []
pos = int(LEAD_S * FS)
for _ in range(REPS):
    for chans, name in (((0,), "L"), ((1,), "R"), ((0, 1), "LR")):
        for c in chans:
            out[pos : pos + len(sweep), c] = sweep
        starts.append(name)
        pos += slot

wavfile.write("sweep.wav", FS, (out * 32767).astype(np.int16))
np.save("inverse.npy", inverse)
print(f"sweep.wav {len(out) / FS:.1f} s, slots {starts}")
