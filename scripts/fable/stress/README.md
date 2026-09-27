# Stress harness

Drives Music on the Mac as the AirPlay sender, measures what the speaker
does, and builds the report page.  Everything runs from this directory; data
goes to `$FABLE_RUNS` (default `~/fable-runs/<run>/`).

## One-time setup

```bash
swiftc -O miclevel.swift -o miclevel   # mic level logger (hardware timestamps)
swiftc -O taprec.swift -o taprec       # lossless recorder for the USB capture
```

Use `~/.platformio/penv/bin/python3` or any Python 3 with numpy.

## Run

```bash
python3 run.py RUN esp PLAN            # mic next to the speaker
CAPTURE=1 START=3 python3 run.py RUN esp soak240   # on the wire, DAC muted
zsh guard.sh RUN &                     # recovers Music's idle disconnects, logs ping
python3 analyze.py $FABLE_RUNS/RUN     # -> results.tsv
python3 stats.py $FABLE_RUNS/RUN
```

Plans (`plans.py`): `stage1` (realistic), `stage2` (agitated), `stage3`
(absurd: storms, reconnects, HTTP load), `soak`/`soak240` (a seek a minute, a
storm every 15 min), `sstart` (session start timing), `popcheck`,
`groupstress`, `seekab`, `noisetest`.  DEVICE is `esp`, `tv` or `mac`.
`$DEV` in a plan is the device under test; TV volume is capped at 20 %.

- **CAPTURE=1** needs the capture firmware (`GET /api/debug/capture?on=1&mute=1`)
  and the board on the Mac's USB.  `taprec` records "Bedroom Speakers" input,
  `tap2env.py` turns it into the same level file the analysis uses.
- **Mac-side logs** explain sender behaviour: `/usr/bin/log stream --level debug
  --predicate 'process == "AirPlayXPCHelper" AND subsystem == "com.apple.airplay"'`
  (`log` alone is a zsh builtin).
- **Clock**: journal timestamps are mapped to the Mac clock with
  `/api/debug/seek_mode?m=1` marks (harmless, it is the default).

## Report

```bash
python3 gen_data.py && python3 gen_report.py   # -> $FABLE_RUNS/fable-stres.html
```

Run names and labels expected by `gen_data.py` are the ones of the 2026-09-26/27
campaign (st1-esp, st1-tv, ..., soak6); edit `COMPARE`/`SK` for new runs.
