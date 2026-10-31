#!/usr/bin/env python3
"""Build the five presets of data/www/peq.html from a measurement.

usage: presets.py resp-TAG.npz      -> presets.json

Paste presets.json as `const PRESETS=` in peq.html and upload the page.  The
voicings were chosen by ear on 2026-10-24: the accurate tune (fit.py's
defaults) cut the bass and the body of voices too much.
"""
import json
import os
import re
import subprocess
import sys

FIT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fit.py")
RESP = sys.argv[1]

WARM = dict(TARGET_DB=10, TARGET_FC=200, TARGET_Q=0.5, MAX_BOOST=0, MAX_CUT=-8,
            MAX_TOTAL_CUT=-8, MAX_FILTERS=4)
BOOM = dict(WARM, MAX_FILTERS=2)
BALANCED = dict(TARGET_DB=6, TARGET_FC=160, TARGET_Q=0.6, MAX_BOOST=0, MAX_CUT=-10,
                MAX_TOTAL_CUT=-10, MAX_FILTERS=5)
AIR = {"type": "HS", "fc": 6000, "gain": 3, "q": 0.71}


def fit(**voicing):
    env = dict(os.environ, **{k: str(v) for k, v in voicing.items()})
    subprocess.run([sys.executable, FIT, RESP], env=env, check=True, capture_output=True)
    chans = []
    for ch in ("L", "R"):
        bands = []
        with open(f"{ch}.txt") as f:
            for line in f:
                m = re.search(r"PK\s+Fc\s+([\d.]+) Hz\s+Gain\s+([-\d.]+) dB\s+Q\s+([\d.]+)", line)
                if m:
                    bands.append({"type": "PK", "fc": round(float(m[1]), 1),
                                  "gain": round(float(m[2]), 2), "q": round(float(m[3]), 2)})
        chans.append(bands)
    return chans


def cfg(chans, extra=None):
    return {"enabled": True,
            "channels": [{"preamp_db": 0, "bands": b + ([extra] if extra else [])} for b in chans]}


warm = fit(**WARM)
presets = [
    {"name": "Original", "desc": "No EQ: the speakers as they are", "cfg": {"enabled": False}},
    {"name": "Boom fix", "desc": "Only the 1-2 worst bass resonances per side", "cfg": cfg(fit(**BOOM))},
    {"name": "Warm", "desc": "Full bass, the boomy notes tamed", "cfg": cfg(warm)},
    {"name": "Balanced", "desc": "Less bass, cleaner low-mids", "cfg": cfg(fit(**BALANCED))},
    {"name": "Warm + air", "desc": "Warm, +3 dB above 6 kHz (about 3 dB quieter overall)",
     "cfg": cfg(warm, AIR)},
]
with open("presets.json", "w") as f:
    json.dump(presets, f)
for p in presets:
    ch = p["cfg"].get("channels")
    print(p["name"], "| bypass" if not ch else "| " + " / ".join(
        ", ".join(f"{b['type']} {b['fc']:.0f} {b['gain']:+.1f}" for b in c["bands"]) for c in ch))
