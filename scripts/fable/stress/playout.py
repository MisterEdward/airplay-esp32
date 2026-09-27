#!/usr/bin/env python3
"""Time series from the journal's Playout lines (err, buffered, ptp_gap,
gaps, under) + session heap lines.  usage: playout.py RUN_DIR -> JSON"""
import glob, json, re, sys
ANSI = re.compile(r'\x1b\[[0-9;]*m')
run = sys.argv[1]
seen, pts, heap = set(), [], []
for f in sorted(glob.glob(f'{run}/journal-*.log')):
    for l in open(f, errors='replace'):
        l = ANSI.sub('', l)
        if l in seen:
            continue
        seen.add(l)
        m = re.match(r'I \((\d+)\) audio_time: Playout: err=([-\d]+) us .*?buffered=(\d+) .*?ptp_gap=([-\d]+) us .*?gaps=(\d+) under=(\d+) domain=(\w+)', l)
        if m:
            pts.append([int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)),
                        int(m.group(5)), int(m.group(6)), m.group(7)])
        m = re.search(r'\((\d+)\) audio_rt: Free heap: (\d+) internal \(largest block (\d+)\)', l)
        if m:
            heap.append([int(m.group(1)), int(m.group(2)), int(m.group(3))])
pts.sort(); heap.sort()
json.dump({'playout': pts, 'heap': heap}, sys.stdout)
