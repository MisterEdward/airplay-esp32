# Fable 5.1 — personal build for the Bedroom Speakers (ESP32-S3 + PCM5102A)

Branch `fable-5.1`, forked from upstream `v0.2.0`.  Everything below is
specific to this fork; the upstream README still applies for hardware,
pairing and the generic web UI.

## What changed versus v0.2.0

| Area | v0.2.0 | Fable 5.1 |
|---|---|---|
| I2S clock | disabled/enabled on every flush (pop, cursor reset) | enabled once at boot, never touched again |
| Transitions | hard cut | 150 ms fade-in on start/seek/resume, 100 ms fade-out on pause; one-block look-ahead fades even an abrupt flush |
| Clock domain | re-evaluated per frame; could jump local↔PTP mid-song | latched per anchor; waits ≤1.5 s for PTP lock to the anchor's clock, else local for that anchor |
| Post-seek alignment | whole-frame early/late gate, 710 ppm servo (~20 s to remove 15 ms) | exact: ceil(early) frames of silence or trim of expired samples, first sample within one sample period of schedule |
| Drift servo | single 710 ppm rate | tiered 710 / 1420 / 2841 ppm by error size, quietest-sample trims, hysteresis |
| Pipeline measurement | whole DMA descriptors (0..5 ms sawtooth) | interpolated inside the current descriptor + held block |
| Buffered TCP stream | one task: recv + decrypt + decode | reader → 384-slot PSRAM queue → decoder; post-seek packets held until anchor; idle TCP kept open (WARN every 8 s) |
| PTP | anchor reset the lock on every session | tracks the first source, keeps the lock when the anchor names it |
| Volume | one global level | remembered per sender (deviceID from SETUP), ramped, snapped at session start |
| Client hand-over | new session raced old session's teardown | new task waits (≤2.5 s) for the old session to release audio |
| Sources | AirPlay only (BT on ESP32) | AirPlay + USB speaker for the PC, arbitrated: AirPlay owns while connected (paused too), USB after 3 s |
| PC wake | — | USB remote wakeup + HID nudge, Wake-on-LAN magic packet |
| Logs | 8 KB ring, lost on tab close, va_list bug | 192 KB PSRAM journal, backlog on connect, download, per-tag runtime level |
| OTA | no rollback | bootloader rollback; image confirmed when AirPlay serves, else restart after 180 s |
| Output rate | 44.1 kHz | fixed 48 kHz (AirPlay resampled), required by the UAC path |

## Build

```bash
pio run -e esp32s3
```

The project directory must not contain spaces (PlatformIO refuses).  The
generated `sdkconfig.esp32s3` is git-ignored; delete it after changing any
`sdkconfig.defaults*` file so the new defaults apply.

Host tests for the pure modules (envelope, alignment, log journal,
per-source volume) and the production timing loop with fake PCM/clock input:

```bash
tests/host/run.sh
```

## Flashing

### First time after switching to this branch (once, over the COM port)

The rollback feature lives in the **bootloader**, which OTA never rewrites.
Flash the complete image over the UART/COM port once:

```bash
pio run -e esp32s3 -t upload
pio run -e esp32s3 -t uploadfs
```

`uploadfs` writes the SPIFFS image with the new web pages.

### Afterwards (OTA over WiFi)

Firmware: upload `.pio/build/esp32s3/firmware.bin` on the web page, or

```bash
curl -X POST --data-binary @.pio/build/esp32s3/firmware.bin \
  http://192.168.68.104/api/ota/update
```

**Web pages live in SPIFFS, which OTA does not touch.**  After a firmware
OTA that changed `data/www/*`, upload the pages too:

```bash
curl -X POST --data-binary @data/www/index.html \
  "http://192.168.68.104/api/fs/upload?path=/spiffs/www/index.html"
curl -X POST --data-binary @data/www/logs.html \
  "http://192.168.68.104/api/fs/upload?path=/spiffs/www/logs.html"
```

After an OTA the new image boots in `pending_verify` (visible in
`/api/status` → `firmware.ota_state`).  It is marked valid the moment
AirPlay is serving.  If it never reaches the network it restarts after
180 s and the bootloader boots the previous image.

## USB speaker (PC)

- The device enumerates on the **native USB (OTG) port** as "Bedroom
  Speakers" (UAC2, 48 kHz, 16-bit stereo) plus a HID keyboard used only for
  wake.  No drivers on Windows 11.
- Select it as the output device in Windows only when you want the PC to be
  the source; the arbiter does not look at USB activity.
- Host volume/mute are honoured (ramped, not stepped).
- **Feedback-endpoint format.** A full-speed device reports its rate to the
  host through the feedback endpoint; the USB spec says 10.14 in three
  bytes, macOS insists on exactly that, the Windows UAC2 class driver wants
  16.16 in four bytes, and the wrong one makes the host stop the stream
  after ~60 ms (macOS) or play garbage (Windows).  The format is chosen when
  the host opens the stream: *auto* (default) uses the Windows format when
  the host has asked for the Microsoft OS string descriptor (index 0xEE,
  which only Windows does) and the spec format otherwise.  Override in the
  web UI (PC card) or `POST /api/pc/config {"usb_feedback":"windows"|"mac"|"auto"}`;
  `/api/status` → `pc.feedback_format` shows what is in use.
  Note: Windows caches the 0xEE answer per VID/PID after the first plug-in,
  so if auto-detection ever misses, pick *Windows* explicitly.
- Windows must allow the device to wake the computer (Device Manager →
  the HID keyboard → Power Management) for USB wake from sleep.  From fully
  off only Wake-on-LAN works: enter the PC's Ethernet MAC in the web UI.

## Diagnostics

- `/logs` — live WebSocket viewer with backlog, filter, pause, copy, download
  of the whole journal, and per-tag runtime log level (up to DEBUG).  The
  `httpd*`, `event` and `esp-tls` tags are pinned at INFO whatever level
  the viewer asks for: at DEBUG every line pushed to the browser made the
  HTTP server log four more, which fed back until httpd stopped answering.
- TinyUSB's own messages (class requests, interface open/close, rejected
  requests) appear under the `tinyusb` tag.
- `sid=N #k METHOD took 312 ms` — an RTSP handler that answered slowly; a
  multiroom sender drops a speaker that replies late.
- `/api/status` — JSON: active source, AirPlay session (sender, volume,
  paused), now playing, timing (domain, acquisition error, servo state),
  PTP, USB/PC state, firmware/OTA state.
- Key log lines to look for:
  - `Anchor: rtp=… domain=ptp` — the schedule was latched on PTP time.
  - `Acquired: rtp=… err=+123 us silence=… trimmed=…` — how exactly the
    first sample after start/seek/resume landed (target: |err| < 1 sample).
  - `Playout: err=… filt=… servo=on/off …` — every ~1 s during playback.
  - `After FLUSHBUFFERED #N packet 1/3: rtp=… seq=…` — first three received
    packets after each request; `Buffered RTP jump` reports steps over four frames.
  - `Deferred flush drop` / `Late-frame drop` — first drop and every 100th.
  - `Deferred flush cancelled` — recovery after 2 s without media despite
    available frames, or an RTP jump more than 1 s below `from` after the
    boundary. Clears PCM and the old anchor; plays immediately until re-anchored.
  - `First frame queued for generation N … since_flush=… ms` — seek latency
    from FLUSHBUFFERED to the first decoded frame of the new position.
  - `Hand-over: old session released audio after … ms` — device switch.
  - `Output -> USB (AirPlay gone for 3 s)` — arbiter decisions.

## Test plan for the first hardware session

1. Flash bootloader + firmware + SPIFFS over COM; confirm boot log shows
   `Render task up: rate=48000`, `USB speaker ready`, `Output -> USB (boot)`.
2. Windows: device appears under Playback; play music; check `/api/status`
   `pc.ring_ms` stays near 10 and `pc.trims` grows slowly (a few per minute
   is normal; hundreds per minute means the adapter is fighting something).
3. AirPlay from the phone: fade-in audible? `Acquired: … err=` within a few
   hundred µs?  Seek 10 times: each `Acquired` within ±1 ms and no
   `Late-frame drain`.  Pause: fade-out, then `receiver paused`.
4. Multiroom with the Mac: after each seek compare by ear immediately.
   Expected: aligned from the first note.  If a constant offset remains,
   tune `PIPELINE_LATENCY_US` in `audio_timing.c` (positive = play earlier).
5. Disconnect AirPlay: `Output -> USB` after 3 s and PC audio resumes with a
   fade-in.  Reconnect while USB plays: USB fades out, AirPlay fades in.
6. Sleep the PC, press *Wake PC*: `Wake: sending USB remote-wakeup`, PC
   wakes.  Shut down, press *Wake PC*: `WoL magic packet … sent x3`.
7. Leave it playing for an hour from each source: `dma_underruns` must stay
   0, `usb.underruns/overruns` 0.
