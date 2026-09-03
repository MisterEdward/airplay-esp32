# AirPlay v0.2.0 baseline test

Use the same iPhone, access point, track and volume for both firmware builds.

## Test order

1. Connect, play for 15 seconds, then disconnect. Repeat 20 times.
2. Start a track, pause for 10 seconds, then resume. Repeat 10 times.
3. Seek forward and backward. Repeat 10 times in each direction.
4. Skip forward and backward between tracks. Repeat 10 times.
5. Test an Apple Music Audio Mix transition.
6. Play continuously for at least one hour.

## What to save

Open `http://192.168.68.104/logs`, press **Clear**, then run the test. Press
**Download** when finished. Save one log file per firmware.

Filter the compact lifecycle data with:

```sh
rg "ap_metrics" serial.log
```

Useful markers are `FIRST_PCM`, `FIRST_I2S`, `FLUSH_BOUNDARY`, `SUMMARY` and
`ACTIVE_REPLACED`. A silent session with packets received but no `FIRST_PCM`
points toward decrypt/decode. `FIRST_PCM` without `FIRST_I2S` points toward
timing/output. A non-zero `i2s_under` points toward output-task starvation.
