#!/usr/bin/env python3
"""Per-action latencies from the mic envelope (end-to-end, any device) and
the speaker journal (ESP32 internals).
usage: analyze.py RUN_DIR [...]   -> RUN_DIR/results.tsv + summary on stdout"""
import csv, glob, json, os, re, sys
import numpy as np

ANSI = re.compile(r'\x1b\[[0-9;]*m')
GAP_KINDS = ('seek', 'next', 'prev', 'play', 'session-start', 'rc-start',
             'warm', 'seek3', 'seek2', 'seek1', 'next-burst', 'toggle-play',
             'next-then-seek', 'seek-after-next', 'seek-storm', 'np-storm',
             'seek-under-load', 'soak-seek', 'post-check', 'all-rc-start')
STOP_KINDS = ('pause', 'session-stop', 'rc-stop', 'toggle-pause', 'end')


def load_journal(run):
    seen, ev = set(), []
    for f in sorted(glob.glob(os.path.join(run, 'journal-*.log'))):
        for line in open(f, errors='replace'):
            line = ANSI.sub('', line).rstrip()
            m = re.match(r'[IWE] \((\d+)\) (\S+): (.*)', line)
            if not m or line in seen:
                continue
            seen.add(line)
            ev.append((int(m.group(1)), m.group(2), m.group(3), line[0]))
    ev.sort(key=lambda e: e[0])
    return ev


def clock_map(run, ev=None):
    """epoch_ms = log_ms + b.  Coarse b from the uptime_s tick edges, fine b
    from the "Seek hold mode" marks (request midpoint vs its log line)."""
    raw = [l.rstrip('\n').split('\t') for l in open(os.path.join(run, 'clock.tsv'))]
    marks = [(float(a), float(b)) for a, b, c in raw if c == 'MARK']
    rows = [(float(a), float(b), float(c)) for a, b, c in raw if c != 'MARK']
    coarse = clock_edges(rows)
    if coarse is None:
        return None
    logs = sorted(u for u, tag, msg, lvl in (ev or []) if 'Seek hold mode -> 1' in msg)
    def clusters(xs):
        out = []
        for x in xs:
            if out and x - out[-1][-1] < 5000:
                out[-1].append(x)
            else:
                out.append([x])
        return out
    mids = clusters(sorted((a + b) / 2 for a, b in marks))
    offs = []
    for mc in mids:
        # the log cluster whose coarse time is nearest this mark cluster
        lcs = [lc for lc in clusters(logs) if len(lc) == len(mc)]
        if not lcs:
            continue
        lc = min(lcs, key=lambda lc: abs(coarse(lc[0]) - mc[0]))
        if abs(coarse(lc[0]) - mc[0]) > 5000:
            continue
        offs += [m - u for m, u in zip(mc, lc)]
    if len(offs) >= 3:
        off = float(np.median(offs))
        print(f'  clock: fine offset from {len(offs)} marks, spread '
              f'{np.ptp(offs):.0f} ms; coarse-fine = {coarse(0) - off:.0f} ms')
        return lambda up: up + off
    return coarse


def clock_edges(rows):
    pts = []
    for p, c in zip(rows, rows[1:]):
        if c[2] == p[2] + 1 and c[0] - p[1] < 200:
            # tick happened between the previous response and this request
            # (roughly); use the midpoints of both polls
            e = ((p[0] + p[1]) / 2 + (c[0] + c[1]) / 2) / 2
            pts.append((c[2] * 1000.0, e))
    pts = np.array(pts)
    if len(pts) == 0:
        return None
    if np.ptp(pts[:, 0]) > 60000:
        a, b = np.polyfit(pts[:, 0], pts[:, 1], 1)
    else:
        a, b = 1.0, float(np.median(pts[:, 1] - pts[:, 0]))
    return lambda up: a * up + b


HF = None   # (t, hfpeak) of the current run, for the pop detector


def mic_env(run):
    global HF
    d = np.loadtxt(os.path.join(run, 'mic.env'))
    t, band = d[:, 0], d[:, 2]
    sm = np.convolve(band, np.ones(5) / 5, mode='same')   # 50 ms
    lo, hi = np.percentile(sm, [5, 80])
    mid = (lo + hi) / 2
    floor = float(np.median(sm[sm < mid]))
    music = float(np.median(sm[sm >= mid]))
    global LEVELS
    LEVELS = (floor, music)
    HF = (t, d[:, 5], sm) if d.shape[1] >= 6 else None
    return t, sm, floor


def transients(t_lo, t_hi):
    """Isolated >2.5 kHz peaks: a 10 ms block at least 15 dB above the
    median of its +-100 ms neighbourhood (excluding itself).  A pop is a
    step/click, broadband and short; music onsets under our 150 ms fade-in
    are not.  Returns [(t, excess_db)]."""
    if HF is None:
        return []
    t, h, sm = HF
    floor, music = LEVELS
    quiet = floor + 0.3 * (music - floor)
    i0, i1 = np.searchsorted(t, [t_lo, t_hi])
    out = []
    for i in range(max(i0, 20), min(i1, len(h) - 20)):
        nb = np.concatenate([h[i - 10:i - 1], h[i + 2:i + 11]])
        ex = h[i] - np.median(nb)
        # only in silence: the band level around it (+-200 ms, excluding
        # the block) is at the room floor, i.e. no music is playing there
        ctx = np.concatenate([sm[i - 20:i - 3], sm[i + 4:i + 21]])
        if ex >= 15 and h[i] == h[max(0, i - 2):i + 3].max() and np.median(ctx) < quiet:
            out.append((t[i], ex))
    return out


def first_sustained(t, x, start, stop, cond, dur_ms, frac):
    """First index i with t[i] >= start where cond holds for >= frac of the
    next dur_ms."""
    i0 = np.searchsorted(t, start); i1 = np.searchsorted(t, stop)
    n = max(1, int(dur_ms / 10))
    c = cond(x).astype(float)
    if i1 - i0 < n:
        return None
    cs = np.concatenate([[0], np.cumsum(c)])
    for i in range(i0, i1 - n):
        if c[i] and (cs[i + n] - cs[i]) >= frac * n:
            return i
    return None


def analyse(run):
    meta = json.load(open(os.path.join(run, 'meta.json')))
    acts = [l.rstrip('\n').split('\t') for l in open(os.path.join(run, 'actions.tsv'))]
    acts = [(float(a[0]), float(a[1]), a[2], a[3], a[4] if len(a) > 4 else '') for a in acts]
    t, sm, floor = mic_env(run)
    floor, music = LEVELS
    on, off = floor + 0.5 * (music - floor), floor + 0.3 * (music - floor)
    ev = load_journal(run) if meta['device'] == 'esp' else []
    to_ep = clock_map(run, ev) if ev else None
    evt = [(to_ep(u), tag, msg, lvl) for u, tag, msg, lvl in ev] if to_ep else []
    rows = []
    for k, (t0, t1, label, cmd, res) in enumerate(acts):
        nxt = acts[k + 1][0] if k + 1 < len(acts) else t0 + 10000
        win_end = min(nxt, t0 + 8000)
        r = {'run': os.path.basename(run), 'device': meta['device'],
             'label': label, 'cmd': cmd, 't0': round(t0), 'ae_ms': round(t1 - t0),
             'next_ms': round(nxt - t0), 'res': res}
        is_stop = cmd == 'pause' or (cmd.startswith('only') and 'MacBook' in cmd)
        is_vol = cmd.startswith('dvol')
        if not is_stop and not is_vol and cmd.startswith(('seek', 'next', 'prev', 'play', 'start', 'only')):
            # silence then sound
            si = first_sustained(t, sm, t0, win_end, lambda x: x < off, 60, 0.8)
            if si is not None:
                r['cut_ms'] = round(t[si] - t0)
                so = first_sustained(t, sm, t[si], win_end, lambda x: x > on, 150, 0.8)
                r['sound_ms'] = round(t[so] - t0) if so is not None else None
                r['gap_ms'] = round(t[so] - t[si]) if so is not None else None
            else:
                r['cut_ms'] = None; r['sound_ms'] = None
        elif is_stop:
            si = first_sustained(t, sm, t0, win_end, lambda x: x < off, 300, 0.9)
            r['stop_ms'] = round(t[si] - t0) if si is not None else None
        elif is_vol:
            i0, i1 = np.searchsorted(t, [t0 - 2000, t0])
            j0, j1 = np.searchsorted(t, [t0 + 1500, min(nxt, t0 + 3500)])
            if i1 > i0 and j1 > j0:
                L0, L1 = np.median(sm[i0:i1]), np.median(sm[j0:j1])
                r['vol_step_db'] = round(L1 - L0, 1)
                if abs(L1 - L0) >= 5:
                    mid = (L0 + L1) / 2
                    cond = (lambda x: x < mid) if L1 < L0 else (lambda x: x > mid)
                    si = first_sustained(t, sm, t0, t0 + 3000, cond, 200, 0.7)
                    r['vol_ms'] = round(t[si] - t0) if si is not None else None
        pops = transients(t0, min(nxt, t0 + 6000))
        if pops:
            r['pops'] = ' '.join(f'+{tt - t0:.0f}ms/{ex:.0f}dB' for tt, ex in pops)[:120]
            r['pop_max_db'] = round(max(ex for _, ex in pops), 1)
        if evt:
            journal_metrics(r, t0, nxt, evt)
        rows.append(r)
    return rows, floor


def journal_metrics(r, t0, nxt, evt):
    lo, hi = t0 - 150, min(nxt - 150, t0 + 9000)
    win = [e for e in evt if lo <= e[0] < hi]
    first = lambda pat, after=lo: next((e for e in win if e[0] >= after and re.search(pat, e[2])), None)
    cmd = r['cmd']
    trig = imm = None
    if cmd.startswith(('seek', 'next', 'prev')):
        imm = first(r'FLUSHBUFFERED immediate')
        dfr = first(r'FLUSHBUFFERED deferred')
        r['flush'] = ('imm' if imm else '') + ('+def' if dfr else '')
        cands = [e for e in (imm, dfr, first(r'rate=0 -> PAUSE')) if e]
        trig = min(cands, key=lambda e: e[0]) if cands else None
        if trig:
            # latencies after the trigger are measured from the immediate
            # flush when there is one (that is what starts the new audio)
            r['first_rtsp'] = re.sub(r'sid=\d+ ', '', trig[2])[:28]
    elif cmd == 'pause':
        trig = first(r'rate=0 -> PAUSE')
    elif cmd == 'play':
        trig = first(r'rate=1\.0 -> PLAY')
    elif cmd.startswith('only') and r['label'].endswith('start'):
        trig = first(r'New client connected|SETUP: has_streams=0')
    elif cmd.startswith('only'):
        trig = first(r'TEARDOWN')
    elif cmd.startswith('dvol'):
        trig = first(r'rtsp_conn: sid=\d+ volume')
    if not trig:
        r['j'] = 'none'
        return
    r['rtsp_ms'] = round(trig[0] - t0)
    if cmd in ('pause',) or r['label'].endswith('stop') or cmd.startswith('dvol'):
        return
    base = imm or trig
    if imm:
        r['imm_ms'] = round(imm[0] - t0)
    anc = first(r'^Anchor:', base[0])
    acq = first(r'^Acquired:', base[0])
    if anc:
        r['anchor_ms'] = round(anc[0] - base[0])
        m = re.search(r'domain=(\w+)', anc[2]); r['domain'] = m.group(1)
    if acq:
        r['acq_ms'] = round(acq[0] - base[0])
        r['acq_e2e_ms'] = round(acq[0] - t0)
        for key, pat in (('err_us', r'err=([-\d]+) us'), ('dropped', r'dropped=(\d+)'),
                         ('trimmed', r'trimmed=(\d+)')):
            m = re.search(pat, acq[2]); r[key] = int(m.group(1)) if m else None
        m = re.search(r'domain=(\w+)', acq[2]); r['acq_domain'] = m.group(1) if m else ''
        r['start'] = (re.search(r'start=(\w+)', acq[2]) or re.search(r'(re-acquire)', acq[2]) or [None, ''])[1]
    old = first(r'before it (\d+) old packets', base[0])
    if old:
        r['old_pkts'] = int(re.search(r'before it (\d+) old', old[2]).group(1))
    bad = [e for e in win if e[0] >= trig[0] and (e[3] in 'WE') and
           not re.search(r'AAC decode error|Failed to decode aac|decoder reset OK', e[2])]
    if bad:
        r['warn'] = ' | '.join(sorted(set(re.sub(r'[-\d]+', '#', e[2])[:70] for e in bad)))[:300]


COLS = ['run', 'device', 'label', 'cmd', 't0', 'ae_ms', 'next_ms', 'cut_ms', 'sound_ms', 'gap_ms',
        'stop_ms', 'vol_step_db', 'vol_ms', 'flush', 'first_rtsp', 'rtsp_ms', 'imm_ms', 'anchor_ms', 'acq_ms', 'acq_e2e_ms',
        'err_us', 'dropped', 'trimmed', 'domain', 'acq_domain', 'start', 'old_pkts', 'pop_max_db', 'pops', 'j', 'warn', 'res']

if __name__ == '__main__':
    for run in sys.argv[1:]:
        rows, floor = analyse(run)
        with open(os.path.join(run, 'results.tsv'), 'w') as f:
            w = csv.DictWriter(f, COLS, delimiter='\t', extrasaction='ignore')
            w.writeheader(); w.writerows(rows)
        print(f'== {run}  floor={LEVELS[0]:.1f} music={LEVELS[1]:.1f} dB  actions={len(rows)}')
        for r in rows:
            print('  '.join(f'{k}={r[k]}' for k in COLS[2:] if r.get(k) not in (None, '')
                            and k not in ('t0', 'res')))
