#!/usr/bin/env python3
"""Aggregate results.tsv of runs: per (device, action) mean/median/p90/max
of the end-to-end mic latency, plus journal-side numbers for the ESP32.
usage: stats.py RUN_DIR... [--json OUT]"""
import csv, json, sys
import numpy as np

CAT = {  # label -> (action, mic metric)
    'seek': ('seek', 'sound_ms'), 'next': ('next', 'sound_ms'),
    'prev': ('prev', 'sound_ms'),
    'pause-short': ('pause', 'stop_ms'), 'pause-long': ('pause', 'stop_ms'),
    'play-short': ('play (după 2 s)', 'sound_ms'),
    'play-long': ('play (după 60 s)', 'sound_ms'),
    'session-stop': ('stop sesiune', 'stop_ms'),
    'session-start': ('start sesiune', 'sound_ms'),
    'volume': ('volum', 'vol_ms'),
    'seek3': ('seek la 3 s', 'sound_ms'), 'seek2': ('seek la 2 s', 'sound_ms'),
    'seek1': ('seek la 1 s', 'sound_ms'),
    'seek-under-load': ('seek sub load HTTP', 'sound_ms'),
    'soak-seek': ('seek (anduranță)', 'sound_ms'),
    'post-check': ('seek după haos', 'sound_ms'),
}


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def summarize(vals):
    v = np.array([x for x in vals if x is not None])
    if len(v) == 0:
        return None
    return {'n': int(len(v)), 'mean': float(v.mean()), 'median': float(np.median(v)),
            'p90': float(np.percentile(v, 90)), 'min': float(v.min()), 'max': float(v.max())}


def main(runs, out_json=None):
    rows = []
    for r in runs:
        rows += list(csv.DictReader(open(f'{r}/results.tsv'), delimiter='\t'))
    groups = {}
    for x in rows:
        if x['label'] not in CAT:
            continue
        act, metric = CAT[x['label']]
        groups.setdefault((x['device'], act, metric), []).append(x)
    res = []
    for (dev, act, metric), xs in sorted(groups.items()):
        mic = [num(x.get(metric)) for x in xs]
        fails = sum(1 for m in mic if m is None or m > 3000)
        e = {'device': dev, 'action': act, 'metric': metric, 'count': len(xs),
             'mic': summarize(mic), 'mic_values': mic, 'fail_or_unmeasured': fails}
        if dev == 'esp':
            e['acq_e2e'] = summarize([num(x.get('acq_e2e_ms')) for x in xs])
            e['rtsp'] = summarize([num(x.get('rtsp_ms')) for x in xs])
            e['acq_from_flush'] = summarize([num(x.get('acq_ms')) for x in xs])
            e['err_us_abs'] = summarize([abs(num(x['err_us'])) for x in xs if num(x.get('err_us')) is not None])
            e['dropped_nonzero'] = sum(1 for x in xs if (num(x.get('dropped')) or 0) > 0)
            e['domain_local'] = sum(1 for x in xs if x.get('acq_domain') == 'local' or x.get('domain') == 'local')
            e['pops'] = sum(1 for x in xs if (num(x.get('pop_max_db')) or 0) >= 25)
            e['warns'] = sorted(set(x['warn'] for x in xs if x.get('warn')))[:8]
        else:
            e['pops'] = sum(1 for x in xs if (num(x.get('pop_max_db')) or 0) >= 25)
        res.append(e)
        m = e['mic']
        ms = (f"mean {m['mean']:5.0f} med {m['median']:5.0f} p90 {m['p90']:5.0f} max {m['max']:5.0f} (n={m['n']})"
              if m else 'no mic data')
        extra = ''
        if dev == 'esp' and e.get('acq_e2e'):
            a = e['acq_e2e']; extra = f" | jurnal: acq med {a['median']:.0f}"
            if e.get('rtsp'):
                extra += f" rtsp med {e['rtsp']['median']:.0f}"
            extra += f" drop>0:{e['dropped_nonzero']} local:{e['domain_local']}"
        print(f"{dev:4} {act:22} {metric:9} {ms}  fail/unmeas {fails}/{len(xs)} pops {e['pops']}{extra}")
    if out_json:
        json.dump(res, open(out_json, 'w'), indent=1)


if __name__ == '__main__':
    a = sys.argv[1:]
    oj = None
    if '--json' in a:
        i = a.index('--json'); oj = a[i + 1]; del a[i:i + 2]
    main(a, oj)
