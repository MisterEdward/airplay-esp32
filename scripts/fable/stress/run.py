#!/usr/bin/env python3
"""Stress-test orchestrator.  Drives Music (bin/drive.js), logs the mic
envelope (bin/mic.py), snapshots the speaker, downloads its journal.

usage: run.py RUN_ID DEVICE PLAN
  DEVICE: esp | tv | mac     PLAN: name in plans.py
Output in runs/RUN_ID/: actions.tsv, mic.env, snaps.jsonl, clock.tsv,
journal-N.log, meta.json
"""
import json, os, subprocess, sys, threading, time, urllib.request
sys.path.insert(0, os.path.dirname(__file__))
import plans

IP = '192.168.68.104'   # the ONLY board we touch; never .103
DEV = {'esp': 'Bedroom Speakers', 'tv': 'Bedroom TV',
       'mac': 'edward’s MacBook Pro'}
HERE = os.path.dirname(os.path.abspath(__file__))
run_id, device, plan_name = sys.argv[1:4]
RUNS = os.environ.get('FABLE_RUNS', os.path.expanduser('~/fable-runs'))
OUT = os.path.join(RUNS, run_id)
os.makedirs(OUT, exist_ok=True)
now_ms = lambda: time.time() * 1000


def http(path, timeout=5):
    t0 = now_ms()
    try:
        with urllib.request.urlopen(f'http://{IP}{path}', timeout=timeout) as r:
            body = r.read()
        return t0, now_ms(), body
    except Exception as e:
        return t0, now_ms(), None


snaps = open(os.path.join(OUT, 'snaps.jsonl'), 'a')
def snap(label):
    rec = {'label': label, 'epoch_ms': now_ms()}
    for k, p in (('status', '/api/status'), ('info', '/api/system/info'),
                 ('tasks', '/api/tasks')):
        _, _, b = http(p)
        try:
            rec[k] = json.loads(b) if b else None
        except Exception:
            rec[k] = None
    snaps.write(json.dumps(rec) + '\n'); snaps.flush()
    return rec


clock = open(os.path.join(OUT, 'clock.tsv'), 'a')
def clock_sync(secs=4.0):
    """Poll uptime_s at ~20 Hz; log (t_req, t_resp, uptime_s).  The edges
    where uptime_s ticks pin ESP uptime to Mac epoch."""
    end = time.time() + secs
    while time.time() < end:
        t0, t1, b = http('/api/tasks', timeout=2)
        if b:
            try:
                u = json.loads(b)['uptime_s']
                clock.write(f'{t0:.1f}\t{t1:.1f}\t{u}\n')
            except Exception:
                pass
        time.sleep(0.03)
    # fine marks: this request logs "Seek hold mode -> 1" (1 is already the
    # default, so it changes nothing) at the moment it is handled
    for _ in range(8):
        t0, t1, b = http('/api/debug/seek_mode?m=1', timeout=2)
        if b:
            clock.write(f'{t0:.1f}\t{t1:.1f}\tMARK\n')
        time.sleep(0.15)
    clock.flush()


jn = [0]
def journal(tag=''):
    _, _, b = http('/api/logs/download', timeout=30)
    if b is None:
        return False
    jn[0] += 1
    with open(os.path.join(OUT, f'journal-{jn[0]:02d}{tag}.log'), 'wb') as f:
        f.write(b)
    return True


# ---- mic + driver
CAPTURE = os.environ.get('CAPTURE') == '1'
if CAPTURE:   # record the speaker's USB capture tap, DAC muted
    http('/api/debug/capture?on=1&mute=1')
    mic = subprocess.Popen([os.path.join(HERE, 'taprec'), os.path.join(OUT, 'tap')],
                           stdout=open(os.path.join(OUT, 'mic.err'), 'w'), stderr=subprocess.STDOUT)
else:
    mic = subprocess.Popen([os.path.join(HERE, 'miclevel')], stdout=open(os.path.join(OUT, 'mic.env'), 'w'), stderr=open(os.path.join(OUT, 'mic.err'), 'w'))
#
drv = subprocess.Popen(['osascript', '-l', 'JavaScript',
                        os.path.join(HERE, 'drive.js')],
                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                       bufsize=1)
acts = open(os.path.join(OUT, 'actions.tsv'), 'a')
def do(cmd, label=''):
    cmd = cmd.replace('$DEV', DEV[device])
    if cmd.startswith('dvol ') and 'TV' in cmd:     # Edward: TV max 20 %
        name, v = cmd[5:].rsplit('=', 1)
        cmd = f'dvol {name}={min(int(v), 20)}'
    drv.stdin.write(cmd + '\n'); drv.stdin.flush()
    line = drv.stdout.readline().strip()
    parts = line.split(' ', 4)
    acts.write(f'{parts[0]}\t{parts[1]}\t{label}\t{cmd}\t'
               f'{parts[4] if len(parts) > 4 else ""}\n')
    acts.flush()
    return line

stop_bg = threading.Event()
def background():
    """Periodic snapshot + journal pull (the journal wraps in ~15 min)."""
    last_j = time.time()
    while not stop_bg.wait(30):
        if device == 'esp':
            snap('periodic')
            if time.time() - last_j > 240:
                journal(); last_j = time.time()

meta = {'run': run_id, 'device': device, 'plan': plan_name,
        'start_ms': now_ms()}
snap('before'); clock_sync()
time.sleep(1.5)   # let the mic's t0 settle
bg = threading.Thread(target=background, daemon=True); bg.start()
t_start = time.time()
try:
    for at, cmd, label in plans.PLANS[plan_name](device):
        if cmd == 'SNAP':
            snap(label); continue
        if cmd == 'JOURNAL':
            if device == 'esp':
                journal()
            continue
        if cmd.startswith('MARKQ '):   # label a time window (no command)
            d = t_start + at - time.time()
            if d > 0:
                time.sleep(d)
            acts.write(f'{now_ms():.0f}\t{now_ms():.0f}\t{cmd[6:]}\tMARK\t\n'); acts.flush()
            continue
        if cmd.startswith('LOAD '):   # side load against the speaker
            d = t_start + at - time.time()
            if d > 0:
                time.sleep(d)
            threading.Thread(target=plans.load, args=(cmd[5:], IP),
                             daemon=True).start()
            continue
        d = t_start + at - time.time()
        if d > 0:
            time.sleep(d)
        do(cmd, label)
finally:
    stop_bg.set()
    time.sleep(3)
    snap('after'); clock_sync()
    if device == 'esp':
        journal('-end')
    drv.stdin.write('quit\n'); drv.stdin.flush()
    mic.terminate(); mic.wait()
    if CAPTURE:
        subprocess.run([sys.executable, os.path.join(HERE, 'tap2env.py'), OUT])
    meta['end_ms'] = now_ms()
    json.dump(meta, open(os.path.join(OUT, 'meta.json'), 'w'))
    print('done', OUT)
