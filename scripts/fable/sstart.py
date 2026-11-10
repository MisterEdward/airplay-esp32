#!/usr/bin/env python3
"""Session start/stop timing from the speaker's journal: no microphone, so it
works with the volume at zero.

Music on the Mac plays to the Mac's own output; N times the speaker is
selected alone (a new AirPlay session), left to play, and deselected.  The
journal lines of each session give, from the RTSP connection:

  setup   "SETUP: Stream setup" -> "stream ready"   (our work before the reply)
  data    "stream ready" -> "Buffered data connection"
  anchor  connection -> first anchor
  sound   connection -> "Acquired" (first sample on its schedule)
  click   the Mac's select call -> "Acquired", in the Mac's clock
  stop    first TEARDOWN -> "sid=N closed" (the RTSP client's cleanup is done)

usage: sstart.py TAG [CYCLES] [--analyze]
Data: $FABLE_RUNS/TAG/journal.log, marks.txt.  SPEAKER / MAC_OUTPUT override
the AirPlay device names.  The device volumes are not touched.
"""
import json
import os
import re
import statistics
import subprocess
import sys
import time
import urllib.request

IP = os.environ.get("SPEAKER_IP", "192.168.68.104")
MAC = "A4:CB:8F:F8:2F:14"
SPEAKER = os.environ.get("SPEAKER", "Bedroom Speakers")
MAC_OUTPUT = os.environ.get("MAC_OUTPUT", "Edward’s MacBook Pro")
ANSI = re.compile(r"\x1b\[[0-9;]*m")
PLAY_S, IDLE_S = 12, 7


def get(path, timeout=8):
    with urllib.request.urlopen(f"http://{IP}{path}", timeout=timeout) as r:
        return r.read(), r.headers


def osa(script):
    return subprocess.run(["osascript", "-e", script], capture_output=True,
                          text=True).stdout.strip()


def only(name):
    """Select exactly this AirPlay device; returns (epoch before, after)."""
    other = MAC_OUTPUT if name == SPEAKER else SPEAKER
    t0 = time.time()
    osa(f'''tell application "Music"
set selected of AirPlay device "{name}" to true
set selected of AirPlay device "{other}" to false
end tell''')
    return t0, time.time()


def journal_end():
    cursor = 0
    while True:
        body, h = get(f"/api/logs?after={cursor}")
        cursor = int(h["X-Log-Cursor"])
        if len(body) < 8192:
            return cursor


def journal_from(cursor):
    out = []
    while True:
        body, h = get(f"/api/logs?after={cursor}")
        cursor = int(h["X-Log-Cursor"])
        out.append(body)
        if len(body) < 8192:
            # Strip colours after joining: a chunk can end inside a sequence.
            return ANSI.sub("", b"".join(out).decode("utf-8", "replace"))


def clock_offset():
    """Mac epoch minus the journal's clock, in seconds."""
    best = None
    for _ in range(5):
        t0 = time.time()
        s = json.loads(get("/api/status")[0])
        t1 = time.time()
        off = (t0 + t1) / 2 - s["firmware"]["log_ms"] / 1000
        if best is None or t1 - t0 < best[0]:
            best = (t1 - t0, off)
    return best[1]


def drive(d, cycles):
    info = json.loads(get("/api/system/info")[0])["info"]
    if info["mac"].upper() != MAC:
        sys.exit(f"{IP} is {info['mac']}, not the speaker")
    marks = open(os.path.join(d, "marks.txt"), "w")

    def mark(what, t0, t1):
        marks.write(f"{t0:.3f} {t1:.3f} {what}\n")
        marks.flush()

    only(MAC_OUTPUT)
    osa('tell application "Music" to play (track 1 of playlist '
        '"Replay All Time")')
    time.sleep(8)
    cursor = journal_end()
    marks.write(f"offset {clock_offset():.3f}\n")
    for i in range(cycles):
        mark("start", *only(SPEAKER))
        time.sleep(PLAY_S)
        mark("stop", *only(MAC_OUTPUT))
        time.sleep(IDLE_S)
    osa('tell application "Music" to pause')
    time.sleep(2)
    marks.write(f"offset {clock_offset():.3f}\n")
    marks.close()
    open(os.path.join(d, "journal.log"), "w").write(journal_from(cursor))


def analyze(d):
    offs, starts = [], []
    for line in open(os.path.join(d, "marks.txt")):
        p = line.split()
        if p[0] == "offset":
            offs.append(float(p[1]))
        elif p[2] == "start":
            starts.append(float(p[0]))
    offset = statistics.mean(offs)
    sessions, cur, warn = [], None, 0
    pat = re.compile(r"^(.) \((\d+)\) ([\w-]+): (.*)")
    for line in open(os.path.join(d, "journal.log")):
        m = pat.match(line)
        if not m:
            continue
        ms, tag, msg = int(m[2]), m[3], m[4]
        if "connected from" in msg and tag == "rtsp_server":
            cur = {"conn": ms}
            sessions.append(cur)
        if cur is None:
            continue
        if msg.startswith("SETUP: Stream setup"):
            cur.setdefault("setup", ms)
        elif "stream ready" in msg:
            cur.setdefault("ready", ms)
        elif msg.startswith("Buffered data connection #"):
            cur.setdefault("data", ms)
        elif tag == "audio_time" and msg.startswith("Anchor:"):
            cur.setdefault("anchor", ms)
        elif tag == "audio_time" and msg.startswith("Acquired:"):
            if "acq" not in cur:
                cur["acq"] = ms
                e = re.search(r"err=([+-]\d+) us.*domain=(\w+)", msg)
                cur["err"], cur["domain"] = int(e[1]), e[2]
        elif "TEARDOWN" in msg:
            cur.setdefault("teardown", ms)
            cur["teardown_last"] = ms
        elif "Event port task did not exit" in msg:
            cur["event_timeout"] = cur.get("event_timeout", 0) + 1
            warn += 1
        elif tag == "rtsp_conn" and " closed: requests=" in msg:
            cur["gone"] = ms  # the RTSP client task has finished its cleanup
        elif msg.startswith("Buffered stream stopped"):
            cur["stopped"] = ms
    rows = []
    print("  # conn_s  setup  data anchor  sound  click   err_us domain  "
          "stop  evt_timeout")
    for i, s in enumerate(x for x in sessions if "acq" in x):
        click = None
        t_acq = s["acq"] / 1000 + offset
        prev = [t for t in starts if t <= t_acq]
        if prev and t_acq - prev[-1] < 15:
            click = (t_acq - prev[-1]) * 1000
        stop = (s.get("gone", s.get("stopped", 0)) - s["teardown"]
                if "teardown" in s else None)
        r = dict(setup=s.get("ready", 0) - s.get("setup", 0),
                 data=s.get("data", 0) - s.get("ready", 0),
                 anchor=s.get("anchor", 0) - s["conn"],
                 sound=s["acq"] - s["conn"], click=click, stop=stop)
        rows.append(r)
        print(f" {i + 1:2d} {s['conn'] / 1000:6.0f} {r['setup']:6d} "
              f"{r['data']:5d} {r['anchor']:6d} {r['sound']:6d} "
              f"{click if click is None else round(click):>6} "
              f"{s['err']:+8d} {s['domain']:>6} "
              f"{stop if stop is not None else '-':>5} "
              f"{s.get('event_timeout', 0):>6}")
    if rows:
        def med(k):
            v = [r[k] for r in rows if r[k] is not None]
            return round(statistics.median(v)) if v else "-"
        print(f" median      {med('setup'):6} {med('data'):5} "
              f"{med('anchor'):6} {med('sound'):6} {med('click'):>6} "
              f"{'':15} {med('stop'):>5}   (n={len(rows)}, "
              f"event-port timeouts: {warn})")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    d = os.path.join(os.environ.get("FABLE_RUNS",
                                    os.path.expanduser("~/fable-runs")),
                     args[0])
    os.makedirs(d, exist_ok=True)
    if "--analyze" not in sys.argv:
        drive(d, int(args[1]) if len(args) > 1 else 6)
    analyze(d)


if __name__ == "__main__":
    main()
