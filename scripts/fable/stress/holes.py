#!/usr/bin/env python3
"""Find short dropouts (30-150 ms of digital zeros between audible audio,
hard edges) in a capture run and print the journal events around each.
usage: holes.py RUN_DIR"""
import re, sys
import numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
import analyze as A
run = sys.argv[1]
x = np.fromfile(f'{run}/tap.pcm', '<i2').reshape(-1, 2)[:, 0].astype(np.int32)
idx = np.loadtxt(f'{run}/tap.idx', ndmin=2)
ev = A.load_journal(run); to = A.clock_map(run, ev)
nz = (x != 0).astype(np.int8)
d = np.diff(np.concatenate([[1], nz, [1]]))
zs = np.flatnonzero(d == -1); ze = np.flatnonzero(d == 1)
n = 0
for a, b in zip(zs, ze):
    dur = (b - a) / 48
    pre = np.abs(x[max(0, a - 240):a]).max() if a else 0
    post = np.abs(x[b:b + 240]).max() if b < len(x) else 0
    if 30 <= dur <= 150 and pre > 300 and post > 300:
        n += 1
        ep = np.interp(a, idx[:, 0], idx[:, 1])
        print(f'== hole {dur:.0f} ms at capture {a/48000:.2f} s (pre {pre}, post {post})')
        for u, tag, msg, lvl in ev:
            e = to(u)
            if ep - 1500 <= e <= ep + 300 and not re.search('Playout|DMAP|Album|Artist|Genre|Title|Progress|led', msg):
                print(f'   {e-ep:+6.0f} {tag}: {msg[:105]}')
print('holes:', n)
