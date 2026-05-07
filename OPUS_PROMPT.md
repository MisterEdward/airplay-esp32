# Prompt for Opus

You are working in the repository:

```text
/Users/edward/airplay-esp32
```

Read `AI_HANDOFF.md` first, especially section 5.
Also read `OPUS_LOGS.md` for curated logs from the tests.
Also inspect the audio, RTSP, timing, stream, mDNS, and ES8388 paths before changing code.

Important user instruction:

If you have questions, stop and ask the user before continuing.
Do not guess around unclear hardware or listening-test details.

## Problem

The project runs AirPlay 2 on an ESP32-A1S with ES8388.
Phone playback is basically perfect.
Safari / YouTube in-player AirPlay selection is also good.
MacBook system audio tray AirPlay output is bad.

Bad behavior from macOS system tray:

* jitter / drift-like sound,
* audio sometimes behind video,
* periodic sample skip / repeat feeling,
* after some bad experiments, static.

Good behavior:

* iPhone / phone sounds clean.
* Safari video-player AirPlay picker sounds clean.
* Safari player appears to pause video briefly, then resumes in sync.

## Key Finding

Logs show the bad and good paths use different AirPlay stream types.

Bad macOS system tray path:

```text
type=96
codec=ALAC
sr=44100
spf=352
transport=realtime UDP
```

Good Safari/player/phone path:

```text
type=103
codec=AAC / AAC-ELD path
sr=44100
spf=1024
transport=buffered TCP
```

So this is probably not a simple 48 kHz vs 44.1 kHz mismatch in the observed logs.
It is much more likely a realtime `type=96` timing / pacing / buffering issue.

## What Was Tried and Reverted

Do not repeat these blindly.

* APLL / I2S clock patch.
  * Caused static / no audio / speaker boom.
  * Reverted.
* Realtime anchor rebase / bypass.
  * Did not fix tray audio.
  * Reverted.
* Advertising `type=103` and `UDP,TCP` to macOS tray.
  * macOS tray still selected `type=96`.
  * Reverted.
* Dropping / duplicating one PCM sample in `audio_output.c`.
  * Caused static.
  * Reverted.

## Current Repo State From Codex

Only diagnostic changes should remain.

Files touched:

* `main/rtsp/rtsp_handlers.c`
* `main/audio/audio_receiver.c`
* `main/audio/audio_output.c`
* `main/audio/audio_resample.c`
* `main/audio/audio_timing.c`

These mostly add logs for stream format, setup type, source rate, early frames, and resampler mismatch warnings.

Build command:

```bash
~/.platformio/penv/bin/pio run -e esp32-a1s
```

Last known build result:

```text
SUCCESS
```

Firmware artifact:

```text
.pio/build/esp32-a1s/firmware.bin
```

## What I Want You To Do

First, inspect the whole relevant path before editing:

* RTSP SETUP / RECORD / FLUSH / SETRATEANCHORTIME handlers.
* `audio_receiver`.
* `audio_stream_realtime`.
* `audio_stream_buffered`.
* `audio_buffer`.
* `audio_timing`.
* `audio_output`.
* PTP / NTP clock code.
* ES8388 / I2S init code.
* mDNS / AirPlay advertised capability code.

Then propose or implement the safest next step.

Preferred next step:

Add low-risk telemetry for realtime `type=96` instead of changing sound behavior immediately.

Useful telemetry:

* RTP sequence gaps.
* Packet arrival jitter.
* RTP timestamp delta.
* Realtime buffer depth min/max/average per second.
* Buffer underruns.
* Late packets.
* NACK / retransmission counts.
* PTP lock state.
* PTP offset and jitter.
* I2S write cadence.

Compare logs from:

* Mac system tray YouTube.
* Safari YouTube AirPlay picker.
* iPhone Apple Music / YouTube.

## Likely Root Cause Area

The clean buffered path suggests decoder, ES8388, and basic I2S can work.
The bad path points at realtime UDP ALAC timing.

Most likely causes:

* realtime `type=96` playout pacing does not match sender clock closely enough,
* PTP offset / anchor calculation is wrong or noisy,
* buffer scheduling holds or releases frames at the wrong time,
* packet jitter / retransmission handling is visible as audio artifacts,
* proper rate correction is missing.

Avoid crude output sample dropping.
If rate correction is needed, design a controlled servo / resampler.

## Extra Cleanup

There are log dumps like:

```text
DBG S
DBG K
DBG srp_K
DBG enc(Read)
DBG dec(Write)
```

These leak secrets and spam logs.
Please remove them or hide them behind a compile-time debug flag.
This is cleanup, not the main audio fix.
