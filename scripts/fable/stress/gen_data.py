#!/usr/bin/env python3
"""Collect everything the report needs into report-data.json."""
import csv, glob, json, os, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.environ.get('FABLE_RUNS', os.path.expanduser('~/fable-runs'))


def rows(run):
    p = f'{R}/{run}/results.tsv'
    return list(csv.DictReader(open(p), delimiter='\t')) if os.path.exists(p) else []


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def summ(vals, fail_over=3000):
    v = [x for x in vals if x is not None]
    out = {'vals': [round(x) for x in v], 'n_total': len(vals),
           'unmeasured': sum(1 for x in vals if x is None),
           'over3s': sum(1 for x in v if x > fail_over)}
    if v:
        a = np.array(v)
        out.update(med=float(np.median(a)), mean=float(a.mean()),
                   p90=float(np.percentile(a, 90)), max=float(a.max()), min=float(a.min()))
    return out


def pick(run, labels, metric):
    xs = [x for x in rows(run) if x['label'] in labels]
    return xs, summ([num(x.get(metric)) for x in xs])


def pops(xs, thr=25):
    return sum(1 for x in xs if (num(x.get('pop_max_db')) or 0) >= thr)


def esp_extra(xs):
    acq = [num(x.get('acq_e2e_ms')) for x in xs]
    err = [abs(num(x['err_us'])) for x in xs if num(x.get('err_us')) is not None]
    return {'acq': summ(acq), 'err_max_us': max(err) if err else None,
            'dropped_nonzero': sum(1 for x in xs if (num(x.get('dropped')) or 0) > 0),
            'local': sum(1 for x in xs if x.get('acq_domain') == 'local')}


COMPARE = [
    # (section, key, title, label list, metric, esp run, tv run, note)
    ('realist', 'seek', 'Seek', ['seek'], 'sound_ms', 'st1-esp', 'st1-tv', '20 seek-uri la 10 s'),
    ('realist', 'next', 'Next', ['next'], 'sound_ms', 'st1-esp', 'st1-tv', 'până la sunetul piesei noi'),
    ('realist', 'prev', 'Prev', ['prev'], 'sound_ms', 'st1-esp', 'st1-tv', ''),
    ('realist', 'pause', 'Pause', ['pause-short', 'pause-long'], 'stop_ms', 'st1-esp', 'st1-tv', 'până la liniște'),
    ('realist', 'play2', 'Play după 2 s', ['play-short'], 'sound_ms', 'st1-esp', 'st1-tv', ''),
    ('realist', 'play60', 'Play după 60 s', ['play-long'], 'sound_ms', 'st1-esp', 'st1-tv', ''),
    ('realist', 'sstart', 'Start sesiune', ['session-start'], 'sound_ms', 'st1-esp', 'st1-tv', 'selectezi boxa în Music'),
    ('realist', 'sstop', 'Stop sesiune', ['session-stop'], 'stop_ms', 'st1-esp', 'st1-tv', 'deselectezi boxa'),
    ('realist', 'vol', 'Volum', ['volume'], 'vol_ms', 'st1-esp', 'st1-tv', 'până la schimbarea nivelului'),
    ('agitat', 'seek3', 'Seek la 3 s', ['seek3'], 'sound_ms', 'st2-esp', 'st2-tv', ''),
    ('agitat', 'seek2', 'Seek la 2 s', ['seek2'], 'sound_ms', 'st2-esp', 'st2-tv', ''),
    ('agitat', 'seek1', 'Seek la 1 s', ['seek1'], 'sound_ms', 'st2-esp', 'st2-tv', ''),
    ('agitat', 'seekafternext', 'Seek la 0,3 s după next', ['seek-after-next'], 'sound_ms', 'st2-esp', 'st2-tv', ''),
    ('agitat', 'toggle', 'Pause în toggle de 1 s', ['toggle-pause'], 'stop_ms', 'st2-esp', 'st2-tv', ''),
    ('nerealist', 'seekload', 'Seek la 3 s după haos', ['seek-under-load'], 'sound_ms', 'st3-esp', 'st3-tv', 'ESP32 cu load HTTP de 20 Hz + upload, TV fără'),
    ('nerealist', 'rc', 'Reconectare (Music așteaptă)', ['rc-start', 'all-rc-start'], 'ae_ms', 'st3-esp', 'st3-tv', 'cât stă Music blocat la selectare, în furtuna de reconectări'),
]


def main():
    out = {'compare': [], 'group': {}, 'pops': {}, 'soak': None, 'totals': {}}
    for sec, key, title, labels, metric, er, tr, note in COMPARE:
        exs, es = pick(er, labels, metric)
        txs, ts = pick(tr, labels, metric)
        e = {'section': sec, 'key': key, 'title': title, 'metric': metric, 'note': note,
             'esp': es, 'tv': ts, 'esp_pops': pops(exs), 'tv_pops': pops(txs)}
        if metric != 'ae_ms':
            e['esp_journal'] = esp_extra(exs)
        out['compare'].append(e)
    # pops at play, all runs
    for dev, runs in (('esp', ['pop1', 'st1-esp', 'st2-esp', 'grp']), ('tv', ['st1-tv', 'st2-tv'])):
        xs = [x for r in runs for x in rows(r) if x['cmd'] == 'play']
        out['pops'][dev] = {'plays': len(xs), 'pops': pops(xs)}
    # group
    for lab, metric in (('seek', 'sound_ms'), ('play-short', 'sound_ms'), ('next', 'sound_ms'),
                        ('post-check', 'sound_ms'), ('pause-short', 'stop_ms')):
        xs, s = pick('grp', [lab], metric)
        out['group'][lab] = {'mic': s, **esp_extra(xs), 'pops': pops(xs)}
    # errors of the sender (Apple Events) per run
    for r in sorted(os.listdir(R)):
        a = f'{R}/{r}/actions.tsv'
        if not os.path.isdir(f'{R}/{r}') or not os.path.exists(a):
            continue
        L = [l.rstrip('\n').split('\t') for l in open(a)]
        L = [x for x in L if len(x) > 3 and x[3] != 'MARK']
        ae = [float(x[1]) - float(x[0]) for x in L]
        out['totals'][r] = {'actions': len(L), 'ae_max': max(ae) if ae else 0,
                            'errors': sum(1 for x in L if len(x) > 4 and x[4].startswith('ERR'))}
    # snapshots: resets
    # ESP stress runs only: the OTAs and power cycles of 09-27 were deliberate
    STRESS = ['s0', 'pop1', 'st1-esp', 'ledA', 'ledB', 'noise1', 'st2-esp',
              'st3-esp', 'grp', 'soak', 'soak3', 'soak5']
    resets, span_s = 0, 0
    for f in [f'{R}/{r}/snaps.jsonl' for r in STRESS if os.path.exists(f'{R}/{r}/snaps.jsonl')]:
        ups = []
        for l in open(f):
            d = json.loads(l)
            i = (d.get('info') or {}).get('info') or {}
            if i.get('uptime_s') is not None:
                ups.append((d['epoch_ms'], i['uptime_s']))
        ups.sort()
        resets += sum(1 for a, b in zip(ups, ups[1:]) if b[1] < a[1])
        if len(ups) > 1:
            span_s += (ups[-1][0] - ups[0][0]) / 1000
    out['resets'] = resets
    out['uptime_span'] = [0, span_s]
    # soak
    SK = 'soak6'
    if os.path.exists(f'{R}/{SK}/results.tsv'):
        po = json.loads(subprocess.check_output([sys.executable, os.path.join(HERE, 'playout.py'), f'{R}/{SK}']))
        xs, s = pick(SK, ['soak-seek'], 'sound_ms')
        snaps = []
        for l in open(f'{R}/{SK}/snaps.jsonl'):
            d = json.loads(l)
            i = (d.get('info') or {}).get('info') or {}
            st = d.get('status') or {}
            t = st.get('timing') or {}
            p = st.get('ptp') or {}
            snaps.append([d['epoch_ms'], i.get('uptime_s'), i.get('free_heap'),
                          t.get('gaps'), p.get('steps'), p.get('outliers'), t.get('domain')])
        out['soak'] = {'seek': s, 'seek_extra': esp_extra(xs), 'playout': po['playout'],
                       'heap': po['heap'], 'snaps': snaps,
                       'storm': summ([num(x.get('sound_ms')) for x in rows(SK) if x['label'] == 'soak-storm' and x['cmd'] == 'play'])}
        out['soak']['disconnects'] = [int(l.split()[0]) for l in open(f'{R}/{SK}/guard.log') if 'PHANTOM' in l]
        import re as _re
        tds = set()
        for f in glob.glob(f'{R}/{SK}/journal-*.log'):
            for l in open(f, errors='replace'):
                m = _re.search(r'\((\d+)\) rtsp_handlers: sid=\d+ TEARDOWN: has_streams=0', l)
                if m:
                    tds.add(int(m.group(1)))
        out['soak']['teardowns'] = sorted(tds)
    # where the time goes (ESP32, journal + mic)
    bd = {}
    for key, run, labels, trig in (('seek', 'st1-esp', ['seek'], 'imm_ms'),
                                   ('next', 'st1-esp', ['next'], 'imm_ms'),
                                   ('play', 'st1-esp', ['play-short', 'play-long'], 'rtsp_ms'),
                                   ('sstart', 'st1-esp', ['session-start'], 'rtsp_ms')):
        xs = [x for x in rows(run) if x['label'] in labels]
        def med(f):
            v = [f(x) for x in xs]
            v = [a for a in v if a is not None]
            return float(np.median(v)) if v else None
        g = lambda k: (lambda x: num(x.get(k)))
        bd[key] = {
            'mac': med(g(trig)),
            'sender': med(g('anchor_ms')),
            'esp': med(lambda x: (num(x['acq_ms']) - num(x['anchor_ms'])) if num(x.get('acq_ms')) is not None and num(x.get('anchor_ms')) is not None else None),
            'audible': med(lambda x: (num(x['sound_ms']) - num(x['acq_e2e_ms'])) if num(x.get('sound_ms')) is not None and num(x.get('acq_e2e_ms')) is not None and num(x['sound_ms']) - num(x['acq_e2e_ms']) < 600 else None),
            'total': med(g('sound_ms')),
        }
    out['breakdown'] = bd
    json.dump(out, open(os.path.join(R, 'report-data.json'), 'w'))
    print('ok', len(out['compare']), 'rows; resets', resets, 'total actions',
          sum(v['actions'] for v in out['totals'].values()))


main()
