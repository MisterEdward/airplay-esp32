#!/usr/bin/env python3
"""Before/after plot of two measurements against the fit target.

usage: compare.py resp-BEFORE.npz resp-AFTER.npz OUT.png

Use the same voicing environment as fit.py, so the target is the same.
"""
import sys

import matplotlib
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from fit import HI, LO, target_curve

a, b, out = np.load(sys.argv[1]), np.load(sys.argv[2]), sys.argv[3]
g = a["grid"]
inb = (g >= LO) & (g <= HI)
mids = (g >= 300) & (g <= HI)
fig, axes = plt.subplots(3, 1, figsize=(10, 11), sharex=True)
for ax, ch in zip(axes, ("L", "R", "LR")):
    # Plot both against the BEFORE curve's mids, so a level change from the EQ
    # shows as one; score each against its own mids.
    ref = np.nanmean(a[ch][mids])
    t = target_curve(g)
    ea = a[ch] - ref - t
    eb = b[ch] - np.nanmean(b[ch][mids]) - t
    print(f"{ch:2s}: error vs target 80-2k rms {np.sqrt(np.mean(ea[inb] ** 2)):.1f} -> "
          f"{np.sqrt(np.mean(eb[inb] ** 2)):.1f} dB, "
          f"max {np.max(np.abs(ea[inb])):.1f} -> {np.max(np.abs(eb[inb])):.1f} dB")
    ax.semilogx(g, a[ch] - ref, color="0.6", label="before")
    ax.semilogx(g, b[ch] - ref, color="C0", label="after")
    ax.semilogx(g, t, "--", color="C3", label="target")
    ax.axvspan(LO, HI, color="C0", alpha=0.05)
    ax.set_ylim(-30, 25)
    ax.set_title(ch + (" (both speakers)" if ch == "LR" else ""))
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(loc="lower left", fontsize=8)
axes[-1].set_xlabel("Hz")
fig.tight_layout()
fig.savefig(out, dpi=110)
