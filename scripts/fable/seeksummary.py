#!/usr/bin/env python3
"""Summarise every immediate flush in a speaker journal.

usage: seeksummary.py JOURNAL.log

For each FLUSHBUFFERED immediate: time until sound (next `Acquired`), how
many old packets arrived before the new position, frames dropped at
acquisition and the alignment error.  Healthy: sound 0.4-0.9 s, old 0,
dropped 0, |err| < 100 us.
"""
import re
import sys

flush = None
old = 0
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"\((\d+)\)", line)
    if not m:
        continue
    t = int(m.group(1))
    if "FLUSHBUFFERED immediate" in line:
        flush, old = t, 0
    elif "before it" in line and flush:
        old = int(re.search(r"before it (\d+) old", line).group(1))
    elif "Acquired" in line and flush:
        dropped = re.search(r"dropped=(\d+)", line).group(1)
        err = re.search(r"err=([-+\d]+) us", line).group(1)
        domain = re.search(r"domain=(\w+)", line).group(1)
        print(f"t={flush:>8} sound +{t - flush:5d} ms  old={old:4d}  "
              f"dropped={dropped:>4}  err={err:>6} us  domain={domain}")
        flush = None
