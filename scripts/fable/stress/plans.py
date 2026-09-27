"""Action plans: each yields (at_seconds, command, label).  Commands go to
drive.js; SNAP/JOURNAL/LOAD are handled by run.py.  $DEV = device under test."""
import os, random, subprocess, threading, time, urllib.request

MAC = 'edward’s MacBook Pro'
# seek targets (s): inside the first ~150 s so any track in the list fits
SEEKS = [95, 40, 130, 60, 110, 25, 145, 75, 50, 120,
         35, 100, 65, 140, 45, 85, 115, 30, 70, 125]


def stage1(dev):
    """Realistic: 20 seeks/10 s, 10 next, 10 prev, 10 short pauses,
    5 long pauses, 5 session stop/start, 10 volume changes."""
    P, t = [], 0.0
    def at(dt, cmd, label):
        nonlocal t
        t += dt; P.append((t, cmd, label))
    at(0, 'only $DEV', 'setup')
    at(1, f"start {os.environ.get('START', '0')}", 'setup')
    at(12, 'SNAP', 'seek-block'); at(0, 'seek 30', 'warm')
    for s in SEEKS:
        at(10, f'seek {s}', 'seek')
    at(10, 'JOURNAL', '')
    for _ in range(10):
        at(10, 'next', 'next')
    for _ in range(10):
        at(10, 'prev', 'prev')
    at(10, 'JOURNAL', '')
    for _ in range(10):
        at(8, 'pause', 'pause-short'); at(2, 'play', 'play-short')
    for _ in range(5):
        at(8, 'pause', 'pause-long'); at(60, 'play', 'play-long')
    at(8, 'JOURNAL', '')
    if dev != 'mac':
        for _ in range(5):
            at(10, f'only {MAC}', 'session-stop'); at(5, 'only $DEV', 'session-start')
    for i in range(10):
        at(6, f'dvol $DEV={15 if i % 2 == 0 else 39}', 'volume')
    at(6, 'pause', 'end')
    return P


def stage2(dev):
    """Agitated user."""
    P, t = [], 0.0
    def at(dt, cmd, label):
        nonlocal t
        t += dt; P.append((t, cmd, label))
    at(0, 'only $DEV', 'setup'); at(1, 'start 20', 'setup')
    at(10, 'SNAP', 'seek3')
    rnd = random.Random(2)
    for gap, n, lab in ((3, 15, 'seek3'), (2, 15, 'seek2'), (1, 15, 'seek1')):
        for _ in range(n):
            at(gap, f'seek {rnd.randint(20, 150)}', lab)
        at(6, 'SNAP', lab + '-end')
    at(0, 'JOURNAL', '')
    for _ in range(10):
        at(0.5, 'next', 'next-burst')
    at(10, 'SNAP', 'next-burst-end')
    for _ in range(20):
        at(1, 'pause', 'toggle-pause'); at(1, 'play', 'toggle-play')
    at(8, 'JOURNAL', '')
    for v in list(range(39, 4, -5)) + list(range(5, 45, 5)):
        at(0.3, f'dvol $DEV={v}', 'vol-sweep')
    at(5, 'dvol $DEV=39', 'vol-sweep')
    for _ in range(8):
        at(8, 'next', 'next-then-seek'); at(0.3, f'seek {rnd.randint(30, 120)}', 'seek-after-next')
    at(10, 'pause', 'end')
    return P


def stage3(dev):
    """Unrealistic."""
    P, t = [], 0.0
    def at(dt, cmd, label):
        nonlocal t
        t += dt; P.append((t, cmd, label))
    rnd = random.Random(3)
    at(0, 'only $DEV', 'setup'); at(1, 'start 30', 'setup')
    at(10, 'SNAP', 'seek-storm')
    # 3a: seek every 200-500 ms for 60 s
    end = t + 60
    while t < end:
        at(rnd.uniform(0.2, 0.5), f'seek {rnd.randint(20, 150)}', 'seek-storm')
    at(12, 'SNAP', 'seek-storm-end'); at(0, 'JOURNAL', '')
    # 3b: next/prev as fast as the driver goes, 30 s
    end = t + 30
    k = 0
    while t < end:
        at(0.05, 'next' if k % 2 == 0 else 'prev', 'np-storm'); k += 1
    at(12, 'SNAP', 'np-storm-end')
    # 3c: mixed chaos 200 ms: seek/next/pause/play
    ops = ['seek', 'next', 'pause', 'play', 'prev', 'seek']
    end = t + 45
    while t < end:
        o = rnd.choice(ops)
        at(0.2, f'seek {rnd.randint(20, 150)}' if o == 'seek' else o, 'chaos')
    at(0.5, 'play', 'chaos-end'); at(12, 'SNAP', 'chaos-end'); at(0, 'JOURNAL', '')
    # 3d: reconnect storm: speaker <-> Mac (silent) every 2 s, 2 min
    if dev != 'mac':
        for i in range(30):
            at(2, f'only {MAC}', 'rc-stop'); at(2, 'only $DEV', 'rc-start')
        at(15, 'SNAP', 'rc-storm-end'); at(0, 'JOURNAL', '')
    # 3e: HTTP load during playback, then load + seeks
    if dev == 'esp':
        at(0, 'LOAD status:20:90', ''); at(0, 'LOAD journal:3:90', '')
        at(0, 'LOAD upload:1:90', '')
    at(30, 'SNAP', 'load-only')
    for _ in range(20):
        at(3, f'seek {rnd.randint(20, 150)}', 'seek-under-load')
    at(12, 'SNAP', 'load-end'); at(0, 'JOURNAL', '')
    # 3f: everything at once for 60 s (load still running until 90 s ends,
    # restart it)
    if dev == 'esp':
        at(0, 'LOAD status:20:70', ''); at(0, 'LOAD upload:1:70', '')
    end = t + 60
    while t < end:
        o = rnd.choice(ops + ['rc'])
        if o == 'rc':
            at(0.3, f'only {MAC}', 'all-rc-stop'); at(1.0, 'only $DEV', 'all-rc-start')
        else:
            at(0.3, f'seek {rnd.randint(20, 150)}' if o == 'seek' else o, 'all')
    at(1, 'only $DEV', 'all-end'); at(0.5, 'play', 'all-end')
    at(20, 'SNAP', 'all-end')
    at(0, 'seek 60', 'post-check'); at(12, 'seek 100', 'post-check')
    at(12, 'pause', 'end')
    return P


def soak(dev, minutes=90):
    """Endurance: a seek every minute, a storm every 15 min."""
    P, t = [], 0.0
    rnd = random.Random(4)
    def at(dt, cmd, label):
        nonlocal t
        t += dt; P.append((t, cmd, label))
    at(0, 'only $DEV', 'setup'); at(1, f"start {os.environ.get('START', '40')}", 'setup')
    for m in range(1, minutes + 1):
        if m % 15 == 0:
            for _ in range(20):
                at(0.4, f'seek {rnd.randint(20, 150)}', 'soak-storm')
            for _ in range(6):
                at(0.3, 'next', 'soak-storm')
            at(0.3, 'pause', 'soak-storm'); at(3, 'play', 'soak-storm')
            at(15, 'SNAP', f'soak-{m}')
            at(45 - 13, f'seek {rnd.randint(20, 150)}', 'soak-seek')
        else:
            at(60 if m > 1 else 15, f'seek {rnd.randint(20, 150)}', 'soak-seek')
        if m % 4 == 0:
            at(0, 'JOURNAL', '')
    at(10, 'pause', 'end')
    return P


def seektest(dev):
    """Step 0 verification: a few seeks."""
    P = [(0, 'only $DEV', 'setup'), (1, 'start 0', 'setup')]
    for i, s in enumerate([90, 40, 130, 60, 110]):
        P.append((14 + 10 * i, f'seek {s}', 'seek'))
    P.append((70, 'pause', 'end'))
    return P


PLANS = {'stage1': stage1, 'stage2': stage2, 'stage3': stage3,
         'soak': soak, 'seektest': seektest,
         'soak5': lambda d: soak(d, 5)}


def load(spec, ip):
    """kind:rate_hz:seconds -- HTTP load against the speaker."""
    kind, rate, secs = spec.split(':')
    rate, end = float(rate), time.time() + float(secs)
    payload = b'\0' * (256 * 1024)
    while time.time() < end:
        try:
            if kind == 'status':
                urllib.request.urlopen(f'http://{ip}/api/status', timeout=3).read()
            elif kind == 'journal':
                urllib.request.urlopen(f'http://{ip}/api/logs/download', timeout=20).read()
            elif kind == 'download':
                urllib.request.urlopen(f'http://{ip}/api/speedtest/download?bytes=1048576', timeout=20).read()
            elif kind == 'upload':
                urllib.request.urlopen(urllib.request.Request(
                    f'http://{ip}/api/speedtest/upload', data=payload,
                    method='POST'), timeout=20).read()
        except Exception:
            pass
        time.sleep(1.0 / rate)


def popcheck(dev):
    P = [(0, 'only $DEV', 'setup'), (1, 'start 0', 'setup'), (12, 'seek 60', 'warm')]
    t = 12
    for i in range(8):
        t += 6; P.append((t, 'pause', 'pause-short'))
        t += 3; P.append((t, 'play', 'play-short'))
    for s in (100, 40, 130):
        t += 7; P.append((t, f'seek {s}', 'seek'))
    t += 8; P.append((t, 'pause', 'end'))
    return P


PLANS['popcheck'] = popcheck


def noisetest(dev):
    """Paused speaker, forced WiFi traffic: are the seek-gap crackles
    electrical (WiFi/power) or data?  Each window is 15 s."""
    P = [(0, 'only $DEV', 'setup'), (1, 'start 2', 'setup'),
         (12, 'pause', 'noise-pause'),
         (20, 'MARKQ quiet', ''),                    # 20-35 s: quiet reference
         (35, 'LOAD upload:20:15', ''), (35, 'MARKQ upload', ''),     # 35-50
         (55, 'LOAD download:20:15', ''), (55, 'MARKQ download', ''), # 55-70
         (75, 'LOAD status:20:15', ''), (75, 'MARKQ status', ''),     # 75-90
         (95, 'MARKQ quiet2', ''),                   # 95-110 quiet again
         (110, 'play', 'play-long'), (120, 'pause', 'end')]
    return P


PLANS['noisetest'] = noisetest


def seekab(dev):
    """20 seeks 7 s apart (A/B diagnostics)."""
    P = [(0, 'only $DEV', 'setup'), (1, 'start 3', 'setup')]
    for i, s in enumerate(SEEKS):
        P.append((12 + 7 * i, f'seek {s}', 'seek'))
    P.append((12 + 7 * 20 + 3, 'pause', 'end'))
    return P


PLANS['seekab'] = seekab


def groupstress(dev):
    """ESP32 + TV as one AirPlay group (Edward allowed it)."""
    P, t = [], 0.0
    rnd = random.Random(5)
    def at(dt, cmd, label):
        nonlocal t
        t += dt; P.append((t, cmd, label))
    at(0, 'group Bedroom Speakers|Bedroom TV', 'setup'); at(2, 'start 5', 'setup')
    at(15, 'SNAP', 'group-start')
    for s in SEEKS[:10]:
        at(8, f'seek {s}', 'seek')
    for _ in range(5):
        at(8, 'pause', 'pause-short'); at(3, 'play', 'play-short')
    for _ in range(5):
        at(10, 'next', 'next')
    at(10, 'JOURNAL', '')
    end = t + 30
    while t < end:
        at(rnd.uniform(0.3, 0.5), f'seek {rnd.randint(20, 150)}', 'seek-storm')
    at(12, 'SNAP', 'group-storm-end')
    for s in SEEKS[10:15]:
        at(8, f'seek {s}', 'post-check')
    at(8, 'pause', 'end')
    return P


PLANS['groupstress'] = groupstress


def sstart(dev):
    """Session start timing: 6 x (Mac-only 6 s, then speaker, play 12 s)."""
    P, t = [(0, 'only $DEV', 'setup'), (1, 'start 6', 'setup')], 1.0
    t = 12.0
    for _ in range(6):
        P.append((t, f'only {MAC}', 'session-stop')); t += 6
        P.append((t, 'only $DEV', 'session-start')); t += 12
    P.append((t, 'seek 60', 'seek')); t += 8
    P.append((t, 'pause', 'end'))
    return P


PLANS['sstart'] = sstart


PLANS['soak240'] = lambda d: soak(d, 240)
