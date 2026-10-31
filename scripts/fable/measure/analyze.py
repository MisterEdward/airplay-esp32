#!/usr/bin/env python3
"""Impulse and frequency responses from a sweep recording.

usage: analyze.py rec-TAG.wav [rec-TAG2.wav ...]

Writes resp-TAG.npz (grid, then L, R, LR in dB, 1/6 octave) and prints a
summary.  Reads inverse.npy from the current directory.
"""
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import fftconvolve

FS = 44100
SLOT_N = int(12.0 * FS)
NAMES = ["L", "R", "LR", "L", "R", "LR"]
PRE_S, POST_S = 0.005, 0.300  # IR window around the peak
GRID = np.geomspace(20, 20000, 480)  # 1/48 octave


def smooth(f, mag2, frac):
    """Power average over 1/frac octave around each grid point."""
    out = np.empty_like(GRID)
    k = 2 ** (1 / (2 * frac))
    for i, fc in enumerate(GRID):
        sel = (f >= fc / k) & (f <= fc * k)
        out[i] = mag2[sel].mean() if sel.any() else np.nan
    return 10 * np.log10(out + 1e-20)


def response(h, peak):
    a, b = peak - int(PRE_S * FS), peak + int(POST_S * FS)
    seg = h[a:b].copy()
    tail = int(0.05 * FS)
    seg[-tail:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(tail) / tail)
    head = int(PRE_S * FS)
    seg[:head] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(head) / head)
    n = 1 << 16
    spec = np.fft.rfft(seg, n)
    f = np.fft.rfftfreq(n, 1 / FS)
    return f, np.abs(spec) ** 2


def noise_floor(h, peak):
    # Before the sweep's harmonics: a stretch of pure noise in the IR.
    a = peak - int(2.0 * FS)
    seg = h[a : a + int(POST_S * FS)]
    return 10 * np.log10(np.mean(seg**2) + 1e-30)


def analyze(path):
    rate, x = wavfile.read(path)
    assert rate == FS, rate
    x = x.astype(float) / 32768
    h = fftconvolve(x, np.load("inverse.npy"))
    env = np.abs(h)
    # The first IR is the L sweep of the first repetition; the rest follow at
    # SLOT_N.
    first = int(np.argmax(env > env.max() * 0.2))
    first += int(np.argmax(env[first : first + 2000]))
    peaks = []
    for i in range(len(NAMES)):
        lo, hi = first + i * SLOT_N - 4000, first + i * SLOT_N + 4000
        if hi > len(env):
            break
        peaks.append(lo + int(np.argmax(env[lo:hi])))
    res = {}
    for name in ("L", "R", "LR"):
        p2, snr = [], []
        for nm, pk in zip(NAMES, peaks):
            if nm == name:
                f, m2 = response(h, pk)
                p2.append(m2)
                snr.append(10 * np.log10(np.max(h[pk - 50 : pk + 50] ** 2)) - noise_floor(h, pk))
        res[name] = smooth(f, np.mean(p2, axis=0), 6)
        res[name + "_snr"] = float(np.mean(snr))
        band = (GRID > 80) & (GRID < 2000)
        res[name + "_repeat_db"] = (
            float(np.max(np.abs(smooth(f, p2[0], 6) - smooth(f, p2[1], 6))[band]))
            if len(p2) == 2 else float("nan"))
    return res, peaks


if __name__ == "__main__":
    for path in sys.argv[1:]:
        res, peaks = analyze(path)
        tag = path.rsplit("rec-", 1)[-1].rsplit(".", 1)[0]
        np.savez(f"resp-{tag}.npz", grid=GRID, **res)
        print(f"{path}: {len(peaks)} sweeps found, first at {peaks[0] / FS:.2f} s")
        pts = [40, 63, 100, 160, 250, 500, 1000, 2000, 4000, 8000, 12000, 16000]
        for name in ("L", "R", "LR"):
            r = res[name]
            ref = np.nanmean(r[(GRID > 300) & (GRID < 2000)])
            row = " ".join(f"{p}:{r[np.argmin(abs(GRID - p))] - ref:+5.1f}" for p in pts)
            print(f"  {name:2s} snr {res[name + '_snr']:.0f} dB  "
                  f"repeat {res[name + '_repeat_db']:.1f} dB | {row}")
