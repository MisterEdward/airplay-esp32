#!/usr/bin/env python3
"""Fit PK filters (RBJ, 48 kHz like the firmware) to a measured response.

usage: fit.py resp-TAG.npz      -> L.txt, R.txt (REW format), fit-TAG.png

Corrects 80 Hz - 2 kHz only: the MacBook mic is uncalibrated, so treble is
left to the ear.  The defaults are the "accurate" tune: flat with a +4 dB low
shelf, which measured well and sounded thin.  Environment variables change
the voicing; presets.py holds the ones that were chosen by ear.
"""
import os
import sys

import numpy as np
from scipy.optimize import least_squares

FS = 48000
LO, HI = 80.0, 2000.0
MAX_FILTERS = int(os.environ.get("MAX_FILTERS", 6))
MAX_TOTAL_CUT = float(os.environ.get("MAX_TOTAL_CUT", -15.0))
TARGET_Q = float(os.environ.get("TARGET_Q", 0.707))
TARGET_DB = float(os.environ.get("TARGET_DB", 4.0))
TARGET_FC = float(os.environ.get("TARGET_FC", 105.0))
MAX_BOOST = float(os.environ.get("MAX_BOOST", 3.0))
MAX_CUT = float(os.environ.get("MAX_CUT", -12.0))
Q_MIN, Q_MAX = 0.5, 6.0
Q_MAX_MID = 2.5  # above MODE_HI: one mic point does not justify more
MODE_HI = 300.0  # room modes live below this
BOOST_LO, BOOST_Q_MAX = 300.0, 2.0
STOP_DB = 1.5


def rbj(kind, fc, gain, q, f):
    A = 10 ** (gain / 40)
    w0 = 2 * np.pi * fc / FS
    al = np.sin(w0) / (2 * q)
    c = np.cos(w0)
    if kind == "PK":
        b = [1 + al * A, -2 * c, 1 - al * A]
        a = [1 + al / A, -2 * c, 1 - al / A]
    else:  # LS
        sA = 2 * np.sqrt(A) * al
        b = [A * ((A + 1) - (A - 1) * c + sA), 2 * A * ((A - 1) - (A + 1) * c),
             A * ((A + 1) - (A - 1) * c - sA)]
        a = [(A + 1) + (A - 1) * c + sA, -2 * ((A - 1) + (A + 1) * c),
             (A + 1) + (A - 1) * c - sA]
    z = np.exp(-1j * 2 * np.pi * f / FS)
    h = (b[0] + b[1] * z + b[2] * z**2) / (a[0] + a[1] * z + a[2] * z**2)
    return 20 * np.log10(np.abs(h))


def target_curve(grid):
    return rbj("LS", TARGET_FC, TARGET_DB, TARGET_Q, grid)


def model(params, f):
    out = np.zeros_like(f)
    for fc, g, q in np.asarray(params, dtype=float).reshape(-1, 3):
        out += rbj("PK", fc, g, q, f)
    return out


def fit_channel(grid, resp):
    inb = (grid >= LO) & (grid <= HI)
    ref = np.nanmean(resp[(grid >= 300) & (grid <= HI)])
    target = ref + target_curve(grid)
    err = resp - target
    params, kinds = [], []
    for _ in range(MAX_FILTERS):
        r = np.where(inb, err + model(params, grid), 0)
        # Peaks first: a dip is half as urgent (often a cancellation), and
        # only broad dips above BOOST_LO get a boost.
        i_pk = int(np.argmax(r))
        rb = np.where(grid >= BOOST_LO, r, 0)
        i_dp = int(np.argmin(rb))
        pk, dp = r[i_pk], -rb[i_dp] * 0.5
        boost_left = MAX_BOOST - max(0.0, float(np.max(model(params, grid))))
        if boost_left < 0.5:
            dp = 0
        if max(pk, dp) < STOP_DB:
            break
        if pk >= dp:
            i, g, kind = i_pk, max(-pk, MAX_CUT), "cut"
        else:
            i, g, kind = i_dp, min(-rb[i_dp], boost_left), "boost"
        qmax = BOOST_Q_MAX if kind == "boost" else (Q_MAX if grid[i] < MODE_HI else Q_MAX_MID)
        best = None
        for q in np.geomspace(Q_MIN, qmax, 24):
            e = r + np.where(inb, rbj("PK", grid[i], g, q, grid), 0)
            cost = np.sum(e[inb] ** 2)
            if best is None or cost < best[0]:
                best = (cost, q)
        params += [grid[i], g, best[1]]
        kinds.append((kind, qmax))
    if not params:
        return np.empty((0, 3)), target, err
    p0 = np.array(params)
    lb, ub = [], []
    for kind, qmax in kinds:
        if kind == "cut":
            f_lo, f_hi = (LO * 0.9, MODE_HI * 1.1) if qmax == Q_MAX else (MODE_HI * 0.9, HI * 1.1)
            lb += [f_lo, MAX_CUT, Q_MIN]
            ub += [f_hi, 0.0, qmax]
        else:
            lb += [BOOST_LO, 0.0, Q_MIN]
            ub += [HI * 1.1, MAX_BOOST, BOOST_Q_MAX]
    lb, ub = np.array(lb), np.array(ub)
    p0 = np.clip(p0, lb + 1e-6, ub - 1e-6)
    outb = ((grid >= 30) & (grid < LO)) | ((grid > HI) & (grid <= 8000))

    def res(p):
        m = model(p, grid)
        over = np.maximum(0.0, m - MAX_BOOST)  # summed boost cap
        under = np.maximum(0.0, MAX_TOTAL_CUT - m)  # summed cut cap
        reg = 0.6 * np.sqrt(np.abs(p[1::3]))  # fewer, gentler filters
        return np.concatenate([(err + m)[inb], 0.5 * m[outb], 5 * over, 5 * under, reg])

    return least_squares(res, p0, bounds=(lb, ub)).x.reshape(-1, 3), target, err


def rew_lines(filters):
    lines = ["Filter Settings file", "", "Fable auto-fit (MacBook mic, uncalibrated)", "",
             "Equaliser: Generic"]
    for i, (fc, g, q) in enumerate(sorted(filters, key=lambda x: x[0]), 1):
        lines.append(f"Filter {i:2d}: ON  PK       Fc {fc:7.1f} Hz  Gain {g:6.2f} dB  Q {q:6.3f}")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    d = np.load(sys.argv[1])
    tag = sys.argv[1].rsplit("resp-", 1)[-1].rsplit(".", 1)[0]
    grid = d["grid"]
    fig, axes = plt.subplots(2, 1, figsize=(10, 8), sharex=True)
    for ax, ch in zip(axes, ("L", "R")):
        resp = d[ch]
        filters, target, err = fit_channel(grid, resp)
        corr = model(filters, grid)
        inb = (grid >= LO) & (grid <= HI)
        before = np.sqrt(np.mean(err[inb] ** 2))
        after = np.sqrt(np.mean((err + corr)[inb] ** 2))
        with open(f"{ch}.txt", "w") as out:
            out.write(rew_lines(filters))
        print(f"{ch}: {len(filters)} filters, error 80-2k rms {before:.1f} -> {after:.1f} dB")
        for fc, g, q in sorted(filters, key=lambda x: x[0]):
            print(f"   PK {fc:7.1f} Hz {g:+6.2f} dB Q {q:.2f}")
        ref = np.nanmean(resp[(grid >= 300) & (grid <= HI)])
        ax.semilogx(grid, resp - ref, label="measured", color="0.6")
        ax.semilogx(grid, resp - ref + corr, label="predicted with EQ", color="C0")
        ax.semilogx(grid, target - ref, "--", label="target", color="C3")
        ax.semilogx(grid, corr, label="EQ", color="C2", lw=1)
        ax.axvspan(LO, HI, color="C0", alpha=0.05)
        ax.set_ylim(-30, 25)
        ax.set_title(f"{ch} ({tag})")
        ax.grid(True, which="both", alpha=0.3)
        ax.legend(loc="lower left", fontsize=8)
    axes[-1].set_xlabel("Hz")
    fig.tight_layout()
    fig.savefig(f"fit-{tag}.png", dpi=110)
    print(f"fit-{tag}.png")
