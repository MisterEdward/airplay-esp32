#!/usr/bin/env python3
"""Passive watcher for the speaker: keeps what a dropout needs, with wall time.

Runs on the Mac while anyone plays (the iPhone included).  Sends no AirPlay
command and changes nothing on the board; it only reads.

  ping.txt      one line a second per host:  epoch host rtt_ms|lost
  status.jsonl  /api/status every few seconds, with the Mac's epoch
  journal.log   the speaker's journal, followed through /api/logs?after=
                (the board keeps only ~190 KB, 15-20 min of playback)

usage: watch.py [--also IP ...] [--status-s N] [NAME]
  --also IP   ping this host too (the sender, e.g. the iPhone), to tell a
              speaker-side link stall from a sender-side one
Data goes to $FABLE_RUNS/NAME (default ~/fable-runs/watch-<date>).  Ctrl-C ends.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.request

IP = os.environ.get("SPEAKER_IP", "192.168.68.104")
MAC = "A4:CB:8F:F8:2F:14"
ANSI = re.compile(r"\x1b\[[0-9;]*m")
stop = threading.Event()


def get(path, timeout=6):
    with urllib.request.urlopen(f"http://{IP}{path}", timeout=timeout) as r:
        return r.read(), r.headers


def pinger(host, out, lock):
    while not stop.is_set():
        t = time.time()
        try:
            r = subprocess.run(["ping", "-c", "1", "-t", "2", host],
                               capture_output=True, text=True, timeout=4)
            m = re.search(r"time=([0-9.]+)", r.stdout)
            rtt = m.group(1) if m else "lost"
        except subprocess.TimeoutExpired:
            rtt = "lost"
        with lock:
            out.write(f"{t:.2f} {host} {rtt}\n")
            out.flush()
        stop.wait(max(0.0, 1.0 - (time.time() - t)))


def status(out, period):
    while not stop.is_set():
        t = time.time()
        try:
            body, _ = get("/api/status")
            line = json.dumps({"t": round(t, 2), "s": json.loads(body)},
                              separators=(",", ":"))
        except Exception as e:  # the board may be mid-OTA or off the air
            line = json.dumps({"t": round(t, 2), "error": str(e)[:80]})
        out.write(line + "\n")
        out.flush()
        stop.wait(period)


def journal(out):
    cursor, boot = None, None
    while not stop.is_set():
        try:
            # First call: a cursor past the end is reset to the oldest byte.
            body, h = get(f"/api/logs?after={cursor if cursor else 0}")
            if boot and h["X-Log-Boot"] != boot:
                out.write(f"# watch: board rebooted, new boot {h['X-Log-Boot']}\n")
                cursor = 0
                boot = h["X-Log-Boot"]
                continue
            boot = h["X-Log-Boot"]
            missed = int(h.get("X-Log-Missed", "0"))
            if missed:
                out.write(f"# watch: {missed} bytes overwritten before pull\n")
            cursor = int(h["X-Log-Cursor"])
            if body:
                out.write(f"# watch: pulled at epoch {time.time():.2f}\n")
                out.write(ANSI.sub("", body.decode("utf-8", "replace")))
                if not body.endswith(b"\n"):
                    out.write("\n")
                out.flush()
            if len(body) >= 8192:
                continue  # more waiting
        except Exception as e:
            out.write(f"# watch: pull failed at {time.time():.2f}: {e}\n")
        stop.wait(5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name", nargs="?",
                    default=time.strftime("watch-%Y%m%d-%H%M%S"))
    ap.add_argument("--also", action="append", default=[])
    ap.add_argument("--status-s", type=float, default=5.0)
    a = ap.parse_args()

    info = json.loads(get("/api/system/info")[0])["info"]
    if info["mac"].upper() != MAC:
        sys.exit(f"{IP} is {info['mac']}, not the speaker ({MAC}): not watching")

    d = os.path.join(os.environ.get("FABLE_RUNS",
                                    os.path.expanduser("~/fable-runs")), a.name)
    os.makedirs(d, exist_ok=True)
    ping_f = open(os.path.join(d, "ping.txt"), "a")
    lock = threading.Lock()
    threads = [threading.Thread(target=pinger, args=(h, ping_f, lock))
               for h in [IP] + a.also]
    threads.append(threading.Thread(
        target=status, args=(open(os.path.join(d, "status.jsonl"), "a"),
                             a.status_s)))
    threads.append(threading.Thread(
        target=journal, args=(open(os.path.join(d, "journal.log"), "a"),)))
    for t in threads:
        t.daemon = True
        t.start()
    print(f"watching {IP} -> {d}", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        stop.set()


if __name__ == "__main__":
    main()
