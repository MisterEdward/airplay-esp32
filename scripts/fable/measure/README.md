# Speaker response measurement

Measures the speaker at the listening position with the MacBook's microphone
and fits EQ filters for `/peq`.  No REW needed.  Music on the Mac plays the
sweep over AirPlay, on the speaker only.

Keep results somewhere durable: `work/` survives a reboot, `/tmp` does not
(the 2026-10-24 raw data was lost that way; only its filters remain, in
`data/www/peq.html`).

## One-time setup

From this directory:

```bash
python3 -m venv venv
venv/bin/pip install -r requirements.txt
mkdir -p work && cd work && ../venv/bin/python ../gen_sweep.py
../venv/bin/python ../selftest.py    # simulated room, no speaker needed
```

## Measure

Put the MacBook where your head is, lid open, plugged in.  TV and PS5 off.  Set
the EQ on `/peq` to Original or flat, unless you are checking a tune.  Takes
~90 s and is loud: not at night.  From `work/`:

```bash
../measure.sh base 60                # TAG, AirPlay volume
../venv/bin/python ../analyze.py rec-base.wav
```

Healthy: `6 sweeps found`, SNR above 40 dB, repeat within a few dB.  One
repeat far off means a noise during the take.

## Fit and upload

```bash
../venv/bin/python ../fit.py resp-base.npz       # L.txt, R.txt, fit-base.png
curl -X POST --data-binary @L.txt "http://192.168.68.104/api/peq/import?ch=L"
curl -X POST --data-binary @R.txt "http://192.168.68.104/api/peq/import?ch=R"
```

`fit.py`'s defaults are the accurate tune, which measured well and sounded
thin.  `presets.py` holds the voicings chosen by ear and writes `presets.json`
for `const PRESETS=` in `data/www/peq.html`:

```bash
../venv/bin/python ../presets.py resp-base.npz
```

Automatic preamp is per channel, which shifts the balance when the channels
need different amounts.  If `applied_preamp_db` differs in `GET /api/peq`, set
the lower value as `preamp_db` on both.

## Check it

Measure again with the EQ on, then compare, with the same voicing variables as
the fit:

```bash
../measure.sh eq 60
../venv/bin/python ../analyze.py rec-eq.wav
../venv/bin/python ../compare.py resp-base.npz resp-eq.npz compare.png
```

## Limits

- The mic is uncalibrated, so `fit.py` corrects 80 Hz - 2 kHz only.  Treble is
  for the ear and the shelf slider.
- One mic position: the tune is right where the MacBook stood.
- 1/6 octave smoothing lowers narrow peaks (a simulated +15 dB, Q 5 reads about
  +12 dB), so a fit can under-correct them.  The second measurement shows it.
- `192.168.68.x` is also used by other networks: check you are home first
  (`mac` in `/api/system/info` is `A4:CB:8F:F8:2F:14`).
