#!/usr/bin/env python3
"""Turn a taprec capture (RUN/tap.pcm + tap.idx) into RUN/mic.env with the
same 6 columns as miclevel: epoch_ms rms_db band_db hf_db peak_db hfpeak_db
per 10 ms block (band = 100-800 Hz, hf = >2.5 kHz), so analyze.py works
unchanged.  Digital silence is floored at -120 dB."""
import sys
import numpy as np
run = sys.argv[1]
x = np.fromfile(f'{run}/tap.pcm', '<i2').reshape(-1, 2)[:, 0].astype(np.float32) / 32768
idx = np.loadtxt(f'{run}/tap.idx', ndmin=2)
B = 480                                   # 10 ms at 48 kHz
n = len(x) // B
blk = x[:n * B].reshape(n, B)
starts = np.arange(n) * B
epoch = np.interp(starts, idx[:, 0], idx[:, 1])
db = lambda v: 10 * np.log10(v + 1e-12)
rms = db(np.mean(blk ** 2, axis=1))
peak = 20 * np.log10(np.max(np.abs(blk), axis=1) + 1e-6)
spec = np.abs(np.fft.rfft(blk * np.hanning(B), axis=1)) ** 2
f = np.fft.rfftfreq(B, 1 / 48000)
band = db(spec[:, (f >= 100) & (f < 800)].sum(axis=1))
hf = db(spec[:, f >= 2500].sum(axis=1))
d = np.abs(np.diff(blk, axis=1, prepend=blk[:, :1]))     # sample-step peak
hfpk = 20 * np.log10(np.max(d, axis=1) + 1e-6)
out = np.column_stack([epoch, rms, band, hf, peak, hfpk])
out[:, 1:] = np.maximum(out[:, 1:], -120)
np.savetxt(f'{run}/mic.env', out, fmt='%.1f')
print('blocks', n, 'span s', round((epoch[-1] - epoch[0]) / 1000, 1))
