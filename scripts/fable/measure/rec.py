#!/usr/bin/env python3
"""Record the default input for N seconds at 48 kHz, write a 44.1 kHz WAV.

usage: rec.py OUT.wav SECONDS

sounddevice, not ffmpeg: ffmpeg's avfoundation input dropped chunks of the
MacBook mic (3.58 s of samples in a 4 s take), and a gap ruins deconvolution.
"""
import sys

import numpy as np
import sounddevice as sd
from scipy.io import wavfile
from scipy.signal import resample_poly

out, secs = sys.argv[1], float(sys.argv[2])
x = sd.rec(int(secs * 48000), samplerate=48000, channels=1, dtype="float32")
sd.wait()
y = resample_poly(x[:, 0].astype(float), 147, 160)
wavfile.write(out, 44100, (np.clip(y, -1, 1) * 32767).astype(np.int16))
print(f"{out}: {len(y) / 44100:.2f} s, peak {20 * np.log10(np.abs(y).max() + 1e-9):.1f} dBFS")
