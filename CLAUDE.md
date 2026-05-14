# CLAUDE.md — Project Notes for `airplay-esp32`

This file is loaded into every Claude conversation in this repo. It captures
context that is *not* obvious from a quick code read: architecture, invariants,
historical bug fixes that should not be re-broken, and tooling conventions.

## 1. What this project is

An **AirPlay 2 audio receiver** firmware for ESP32 / ESP32-S3 boards, plus
optional **Bluetooth A2DP sink** (on ESP32 only — ESP32-S3 has no classic BT
radio). It exposes itself on the LAN via mDNS (`_raop._tcp` + `_airplay._tcp`),
accepts RTSP sessions on port 7000, decrypts/decodes the audio stream, and
plays it through one of several DAC backends (ES8388, TAS5756, TAS5825M, PCM5102A,
S/PDIF, USB).

It also serves a small web UI (SPIFFS) for Wi-Fi provisioning, OTA, system
info, sound alerts, and a live log stream over WebSocket.

Upstream: <https://github.com/rbouteiller/airplay-esp32>. License: non-commercial.

## 2. Build / flash / target boards

* **Build system:** PlatformIO on top of ESP-IDF v5.x. Native ESP-IDF / idf.py
  workflows are *not* used here — always go through PlatformIO so the SPIFFS
  upload hook (`scripts/uploadfs_hook.py`) fires.
* **Default env:** `esp32-a1s` (Ai-Thinker ESP32-A1S board, ES8388 codec). This
  is the board the current developer flashes, so build verification should
  default to this env unless the user says otherwise.
* **Build command:**
  ```bash
  ~/.platformio/penv/bin/pio run -e esp32-a1s
  ```
  Firmware artifact: `.pio/build/esp32-a1s/firmware.bin` (OTA-able via the
  web UI `/api/ota/update`).
* **Other envs** (see `platformio.ini`): `squeezeamp`, `squeezeamp-bt`,
  `squeezeamp-4m`, `esp32s3`, `esparagus-audio-brick`,
  `esparagus-audio-brick-s3`, `esparagus-louder`, `esparagus-louder-s3`,
  `wrover`. Each pulls `sdkconfig.defaults` + a board-specific
  `sdkconfig.defaults.<board>` overlay.
* **SPIFFS:** `data/www/` is the web UI source. `scripts/uploadfs_hook.py`
  hooks the `upload` action so `pio run -t upload` flashes both firmware
  *and* SPIFFS. Skipping the hook (e.g. manual `idf.py flash`) leaves the
  web UI as a 404. If a developer reports "web UI 404 after flash", that's
  the cause.
* **Code formatting / lint:** `scripts/format.sh`, `scripts/lint.sh`. Style
  is mostly LLVM/Google C with 2-space indent (see existing code).
* **Clangd false-positives:** clangd in the IDE doesn't have ESP-IDF include
  paths wired up, so it reports thousands of "header not found" /
  "implicit function declaration" errors on ESP-IDF symbols
  (`ESP_LOGI`, `esp_timer_get_time`, etc.). **Ignore them** — trust the
  PlatformIO build output instead.

## 3. Top-level directory layout

```
main/
  main.c                — boot, service startup, BT/AirPlay coexistence
  audio/                — receive → decrypt → decode → buffer → time → play
  rtsp/                 — RTSP server, AirPlay handlers, FairPlay/HAP crypto
  hap/                  — HomeKit Accessory Protocol: SRP, TLV8, pair-setup,
                          pair-verify, HKDF-SHA512 (mbedtls-based)
  network/              — Wi-Fi, Ethernet (W5500), mDNS, PTP, NTP, OTA,
                          captive DNS, web server, log WS stream
  plist/                — Apple binary plist + XML plist parser/builder
  playback_control.c    — switches between AirPlay / BT / alerts
  buttons.c, led.c      — GPIO buttons + status LEDs
  settings.c            — NVS-backed settings (device name, volume, Wi-Fi)
  alac_magic_cookie.c   — ALAC codec config blob

components/
  dac_es8388/           — ES8388 codec init + analog mixer overrides
  dac_tas57xx/          — TAS5756 (SqueezeAMP)
  dac_tas58xx/          — TAS5825M (Esparagus)
  audio-resampler/      — Speex-based polyphase resampler
  board_utils/, boards/ — pin maps, partition tables per target
  display/, u8g2*       — optional OLED status display
  spiffs_storage/       — SPIFFS init/mount helper

data/www/               — web UI assets (HTML/JS/CSS) uploaded to SPIFFS
scripts/                — formatting, lint, SPIFFS upload hook, EQ table gen
sdkconfig.defaults*     — ESP-IDF Kconfig per board
```

## 4. AirPlay audio pipeline (the critical path)

Data flow, network → speakers:

```
RTSP control (TCP/7000)        +— event channel (TCP, per session)
        |                       |
  rtsp_server / rtsp_handlers --+
        | SETUP / RECORD / SETRATEANCHORTIME / FLUSH(BUFFERED) / PAUSE / TEARDOWN
        v
  audio_receiver                          audio_stream_realtime  (UDP "type=96", ALAC, 352 spf)
        |  picks stream type ───────────► audio_stream_buffered  (TCP "type=103", AAC,  1024 spf)
        |
  audio_crypto (ChaCha20-Poly1305, keys via HKDF-SHA512 from SRP shared secret)
        |
  audio_decoder (ALAC or AAC ELD via esp_audio_codec)
        |
  audio_buffer (sorted by RTP timestamp; ~1000 slots × ~1.4 KB in SPIRAM)
        |
  audio_timing (sync to PTP/NTP anchor, drop early/late frames, drift servo
                feed)
        |
  audio_resample (only if source rate != OUTPUT_RATE; 44.1 kHz build skips it)
        |
  audio_servo (linear-interp rate trim, fed by smoothed drift EMA)
        |
  audio_alert mix (chime / alarm overlay)
        |
  apply_volume (software, unless CONFIG_DAC_CONTROLS_VOLUME=y → ES8388 etc.)
        |
  i2s_channel_write → DAC → speakers
```

The playback loop lives in `playback_task` (`audio_output.c`), a regular
FreeRTOS task — **not** a DMA callback. It pulls `FRAME_SAMPLES + 1` samples
from `audio_receiver_read()` per iteration; if zero, it writes
`FRAME_SAMPLES` of silence so I2S never underruns.

### Stream types

| AirPlay path        | type | codec    | transport | spf  | typical sender              |
|---------------------|------|----------|-----------|------|-----------------------------|
| Realtime (`type=96`)| 96   | ALAC     | UDP       | 352  | macOS system audio tray     |
| Buffered (`type=103`)| 103 | AAC-ELD  | TCP       | 1024 | iPhone Music, Safari player |

The buffered/TCP path is by far the better-behaved one. Most known glitches
historically map to the realtime/UDP path (see `AI_HANDOFF.md` §5).

### Clock & timing

* `audio_timing.c` is the timing brain. It owns the *anchor* (RTP timestamp ↔
  network time pair from `SETRATEANCHORTIME`), the *pending frame* buffer
  (one held-back frame while waiting for its wall-clock moment), early/late
  thresholds, and the post-flush bypass state machine.
* Sync modes (`sync_mode_t`):
  * **PTP** — AirPlay 2. Network time from `anchor_network_time_ns` minus
    `ptp_clock_get_offset_ns()`.
  * **NTP** — AirPlay 1. Same shape, with `ntp_clock_get_offset_ns()`.
  * **NONE** — fallback to `anchor_local_time_ns` (no multi-room sync).
* `HARDWARE_OUTPUT_LATENCY_US = 46 ms` accounts for I2S DMA depth (8 desc ×
  256 frames @ 44.1 kHz). Subtracted from target time so frames hit the
  speaker, not the DMA register, on schedule.
* **Drift servo** (`audio_servo.c`) is a linear-interpolation resampler
  bounded to ±2000 ppm in steady state (≈ 3.46 cents, normally inaudible on
  music/speech). It has a short ±10000 ppm post-seek boost to pull phase back
  quickly after scrub. It does *not* change the I2S clock; it stretches/squeezes
  the PCM stream feeding I2S. Smoothing time constant ≈ 1.6 s so listeners
  do not perceive a pitch glide in normal playback.

### Thresholds (audio_timing.c) — these are tuned, do not casually retune

| Macro                              | Value      | Why                                            |
|------------------------------------|------------|------------------------------------------------|
| `DEFAULT_BUFFER_LATENCY_US`        | 200 ms     | startup jitter buffer                          |
| `HARDWARE_OUTPUT_LATENCY_US`       | 46 ms      | I2S DMA depth                                  |
| `TIMING_THRESHOLD_EARLY_US`        | 40 ms      | hold frame as pending; avoids audible A/V drift|
| `TIMING_THRESHOLD_LATE_US`         | 60 ms      | absorbs WiFi-induced i2s_channel_write stalls  |
| `BULK_FLUSH_LATE_THRESHOLD_US`     | 2 s        | flush whole buf on single huge-late frame      |
| `MAX_CONSECUTIVE_EARLY`            | 750 (~6 s) | invalidate "stuck" anchor                      |
| `MAX_CONSECUTIVE_LATE`             | 3          | distinguishes WiFi spike vs stale buffer       |
| `POST_FLUSH_STALE_THRESHOLD_US`    | 10 s       | post-seek: frames > this early are old-track   |
| `POST_FLUSH_LATE_CATCHUP_US`       | 120 ms     | post-seek: late beyond this → drop & catch up  |
| `POST_FLUSH_TIMEOUT_US`            | 5 min      | safety backstop on post-flush bypass           |
| `POST_FLUSH_ONTIME_EXIT_COUNT`     | 10 frames  | converged-on-time count to exit post-flush     |
| `COMPUTE_EARLY_SANITY_LIMIT_US`    | 5 min      | reject nonsense from PTP-master mismatch       |

The DMA loop attempt limit in `audio_timing_read` is **500** — high enough to
drain ~11.6 s of stale/late frames inside one DMA tick (microseconds of CPU
time). This was deliberately raised from 8 as part of the seek fix.

### Decoder transient mute

`audio_stream.c::apply_aac_transient_mute` zeros the first 2 frames of any
*discontinuity* (flush, seek, track change) for AAC streams only. AAC has a
warm-up where the first 1–2 frames are garbage. Don't widen this — anything
larger leaks audible silence after every track skip.

## 5. RTSP / AirPlay 2 control plane

* `rtsp_server.c` accepts on port 7000, hands sockets to `rtsp_conn` workers.
* `rtsp_handlers.c::cmd_handlers[]` dispatches methods:
  `OPTIONS`, `GET`, `POST`, `ANNOUNCE`, `SETUP`, `RECORD`, `SET_PARAMETER`,
  `GET_PARAMETER`, `PAUSE`, `FLUSH`, `FLUSHBUFFERED`, `TEARDOWN`,
  `SETRATEANCHORTIME`, `SETPEERS`, `SETPEERSX`.
* **Pairing:** AirPlay 2 transient pair-setup (skip M1–M6) → derives session
  keys via HKDF-SHA512 over a 64-byte SRP shared secret. Implementation is
  in `main/hap/hap_crypto.c` using `mbedtls_md_hmac_*` primitives (we
  cannot use `mbedtls_hkdf` directly because `CONFIG_MBEDTLS_HKDF_C` is
  not enabled in ESP-IDF defaults). **Do not** swap this back to a
  libsodium-based HKDF — the previous custom implementation broke on
  64-byte IKM and caused immediate decryption failure on the first RTSP
  packet.
* **Frame crypto:** ChaCha20-Poly1305 (`audio_crypto.c`) with per-stream
  Read/Write keys.
* `FLUSH` (AirPlay 1) → `audio_receiver_seek_flush()` (immediate).
* `FLUSHBUFFERED` (AirPlay 2):
  * If body contains `flushFromSeq` / `flushFromTS` and `flushUntilSeq` /
    `flushUntilTS` → deferred: keep playing until the frame whose RTP
    crosses `flushUntilTS`, then bulk-flush. Handled inside
    `audio_timing_read` via `deferred_flush_pending` + signed-32-bit
    timestamp comparison (RTP wraps).
  * Else → immediate `audio_receiver_seek_flush()`.
* `SETRATEANCHORTIME` carries:
  * `rate=0` → pause (`audio_receiver_set_playing(false)`).
  * `rate=1` → resume; reseat anchor (`audio_receiver_set_anchor_time`).
  * Includes `clock_id` (PTP master GUID). Watch the PTP-master-change
    path in `audio_receiver_set_anchor_time` — when an iPhone hands off
    to a Mac mid-session the master changes; we must clear PTP lock and
    invalidate the anchor or `compute_early_us` produces nonsense values
    (observed: 511 452 s early) and post-flush spins.
* `SETPEERS` / `SETPEERSX` → PTP peer list (bplist).

## 6. Seek / flush state machine (recently fixed — read before touching)

This is the source of multiple regressions in this repo. The contract:

1. Sender pauses (`SETRATEANCHORTIME rate=0`) + sends `FLUSHBUFFERED`
   immediate when the user scrubs.
2. `audio_receiver_seek_flush()` clears ring buffer, flushes I2S DMA,
   resets the servo, sets `timing.post_flush = true`, arms an
   "expect new anchor" gate.
3. Sender sends new `SETRATEANCHORTIME rate=1` with the new RTP/position.
4. New audio arrives — but **the TCP socket buffer still contains several
   seconds of pre-flush audio** that the sender queued before the seek.
   This is the root of every post-seek glitch.
5. **`audio_stream_buffered.c` pre-decode skip** (load-bearing): while
   the seek-drain window is open, the buffered TCP task peeks the
   per-packet RTP timestamp BEFORE crypto/decode and discards packets
   whose RTP is outside `(-2 s, +15 s)` of `timing.anchor_rtp_time`.
   This drains the OS TCP socket at network speed (a few hundred ms)
   instead of decoder speed (multiple seconds) and keeps stale audio
   out of the ring buffer entirely.  Without this, the multi-second
   seek gap returns no matter what `audio_timing_read` does downstream.

   The skip window is **time-bounded** via
   `audio_receiver_state_t::seek_drain_until_us` (3 s post-seek), NOT
   tied to `timing.post_flush`.  Reason: post_flush only exits when 10
   consecutive frames land within ±60 ms drift; if the post-seek
   steady-state drift settles outside that band (commonly does — seen
   ~−100 ms after a seek with the servo saturated at +500 ppm),
   post_flush stays true forever.  Combined with a static
   `anchor_rtp + 15 s` window, packet RTPs eventually exceed the window
   as wall clock advances (real-time playback adds ~1 s of "expected
   RTP" per second; pre-buffer adds another ~5 s; after ~10 s of
   playback the window rejects every real-time packet → silence dead
   until the next pause/resume re-anchors). Time-bounding the drain
   window self-clears regardless of post_flush state.

   `seek_drain_until_us` is set by `audio_receiver_seek_flush()` AND by
   `audio_receiver_set_anchor_time()` when its seek heuristic fires
   (covers AirPlay 2 senders that re-anchor without an explicit
   FLUSHBUFFERED).
6. Any stale frames that slipped through (e.g. raced with anchor arrival)
   are handled in `audio_timing_read` post_flush stale / late paths:
   **drop the frame and `continue` inside the 500-attempt loop, never
   `audio_buffer_flush(buffer) + return 0`.** The flush+return pattern
   produced a fatal stutter-then-silence loop: each DMA callback
   flushed, waited 200 ms, the decoder re-queued more late TCP-drain
   frames behind us, repeat for ~5–7 seconds.
7. The **normal-mode** late path (non-post_flush) ALSO uses
   `continue`, not bulk flush — the same flush+return pattern can
   trigger on a WiFi stall mid-track (drift suddenly exceeds 60 ms for
   a few frames, the old code took that as "stale buffer" and flushed,
   producing several seconds of "robotic" stutter every time WiFi
   hiccupped).  The fix is symmetric with the post_flush path.
8. Exit `post_flush` when `POST_FLUSH_ONTIME_EXIT_COUNT` (10) consecutive
   frames land in normal early/late thresholds, OR `POST_FLUSH_TIMEOUT_US`
   (5 min) elapses.

There is also a pre-anchor "seek detection" in
`audio_receiver_set_anchor_time`: if `oldest_rtp` in the buffer is more
than 5 s away from the new anchor RTP, walk the buffer dropping stale
frames until you hit one inside the keep window `(-5 s, +10 s)`. This
handles only what's already in the **ring** buffer at anchor-arrival
time; the TCP socket drain that follows is the pre-decode skip's job.

`audio_buffer_flush()` is destructive — it nukes both stale and valid
frames. Avoid it in any hot path; prefer per-frame `audio_buffer_return`
+ `continue` so the on-time burst behind the stale data survives.

### Buffer target sizing (subtle — easy to break)

`update_timing_targets` computes `target_buffer_frames` as
`latency_samples / slot_capacity`, NOT `/ nominal_frame_samples`.  This
matters because `audio_buffer_queue_decoded` splits AAC frames
(1024 samples) into chunks of `AAC_FRAMES_PER_PACKET` (352) per slot,
and `audio_buffer_get_frame_count` returns the number of slots, not
1024-sample frames.  Dividing by `nominal_frame_samples=1024` yields
target=9 slots = 72 ms (only 36% of the intended 200 ms), causing
startup to begin with a thin buffer that underruns every time the
servo takes a moment to converge.  Always divide by the actual slot
capacity (`min(nominal_frame_samples, AAC_FRAMES_PER_PACKET)`).

## 7. Audio buffer

`audio_buffer.c` is a **sorted-by-RTP-timestamp** ring of fixed slots in
SPIRAM (see telemetry `Sorted buffer created: 1000 slots × 1416 bytes`).
Producers (`audio_stream_*`) call `audio_buffer_queue_decoded`; consumer
(`audio_timing_read`) calls `audio_buffer_take` which returns the
oldest-RTP frame. Sort-on-insert handles out-of-order UDP arrivals on the
realtime path. The buffer also exposes `audio_buffer_oldest_timestamp`
used by the seek detector.

## 8. Telemetry — read the logs like this

`telemetry` (`audio_telemetry.c`) emits one line per second. Field cheat sheet:

```
buf103 rx=N dec=N drop=N gap_evt=N gap_pkt=N maxgap=N nack=N/N retx=N
  jitter=Nsmp dts=N
  | buf min/avg/max=A/B/C ur=N late=N
  | drift min/max=Ams/Bms servo=Nppm
  | i2s w=N avg=Nus max=Nus sil=N
  | ptp lock=0|1 t=NNms
```

* `buf` triplet is *ring-buffer fill in frames* sampled inside the second.
  Healthy steady state on AAC/1024spf is ~30–200 frames.
* `ur` = buffer underruns inside that second (consumer asked, buffer was
  empty). Non-zero = we're starving I2S.
* `late` = frames dropped because they were too late.
* `drift min/max` = `early_us` per frame, in ms. Negative = late. Healthy:
  single-digit ms range, slowly trending toward zero.
* `servo` = applied correction ppm (range ±2000 steady, briefly ±10000 after
  seek). Saturating for several seconds means drift exceeds what the servo can
  fix quickly.
* `i2s sil` = silence writes (consumer returned 0 samples that tick).
  Spikes here indicate timing-gate silencing or buffer empty.
* `ptp lock=1 t=NNms` = PTP locked, offset NN ms.

`set_playing: X -> Y`, `Seek detected:`, `Dropped N stale frames`,
`Frame too early: early=...`, `Bulk flush: ...`, `post_flush ...`,
`Invalidating stuck anchor`, `PTP master changed` are the high-signal lines
when debugging timing.

## 9. BT A2DP coexistence (ESP32 only)

* Wi-Fi + classic BT share the radio. Code in `main.c` aggressively stops
  BT while AirPlay is active (see `start_airplay_services`,
  `on_airplay_client_event`, `stop_bt_services_async`). BT is restarted
  after AirPlay client disconnects + a debounce.
* ESP32-S3 builds have no classic BT — `CONFIG_BT_A2DP_ENABLE` is gated
  via Kconfig.
* If a user reports AirPlay glitches *only* when BT is also up,
  isolate-BT-on-AirPlay is the existing mitigation; see commit
  `66aa347 fix(playback): isolate BT during AirPlay sessions`.

## 10. Web server / control plane

`main/network/web_server.c` registers (port 80):

```
GET  /                       captive portal / SPA shell
GET  /favicon.ico
GET  /logs                   live log page (uses WS /ws/logs)
GET  /ws/logs                WebSocket log stream (see log_stream.c)
GET  /api/wifi/scan
POST /api/wifi/config
*    /api/device/name
POST /api/ota/update         multipart firmware upload → esp_ota
GET  /api/system/info
POST /api/system/restart
GET  /api/alert/list
GET  /api/alert/play?name=chime&volume=N&repeat=N
GET  /api/alert/stop
POST /api/fs/upload
*    /api/fs/delete
GET  /api/fs/list
GET  /hotspot-detect.html    Apple captive portal probe
GET  /library/test/success.html
GET  /generate_204           Android captive portal probe
```

Captive-portal DNS hijack (`dns_server.c`) runs only in AP-provisioning mode
so an unconfigured device shows up as an open Wi-Fi with a setup page.

## 11. Sound alerts

`audio_alert.c` mixes a built-in chime (embedded PCM in `chime_pcm.c`) into
the I2S output. The chime plays *over* AirPlay audio if streaming, or onto
the silence stream if idle (DAC powers up for the duration). Only one
preset (`chime`) is recognized server-side. To add presets: extend
`alert_play_handler` switch + add another embedded PCM array.

## 12. Things that have been tried and rolled back (DO NOT re-introduce)

These are the ghosts in this repo. Re-introducing any of them WITHOUT new
evidence will re-create a known bug. See `AI_HANDOFF.md` §5 for context.

* **APLL-based I2S clocking on ESP32-A1S/ES8388** → severe static / speaker
  boom on connect. Reverted.
* **Pre-decode `discard_before_rtp` / `discard_above_rtp` gates** → 32-bit
  RTP wrap-around eats new tracks' burst packets. Removed entirely as part
  of the seek fix; do not bring back without wrap-safe math AND a way to
  distinguish "stale old track" from "new track with random RTP base".
* **Per-sample slip / drop-or-duplicate in the I2S callback** ("Realtime
  buffer high: frames=..., dropping 1 sample") → static. Reverted.
* **Custom libsodium HKDF-SHA512 in HAP crypto** → wrong keys for 64-byte
  IKM. Replaced with mbedtls HMAC-based HKDF. Keep mbedtls.
* **Advertising both `type=96` and `type=103` to macOS audio tray,
  RAOP TXT `tp=UDP,TCP`** → macOS tray still picks 96. Reverted, neutral.
* **`audio_buffer_flush(buffer)` inside `audio_timing_read` post-flush
  paths** → 5–7 s of stutter-then-silence after every seek. Replaced by
  per-frame `continue` inside the 500-attempt loop (see §6).
* **Touching the PTP anchor to "fix" sync** → breaks multi-room. The PTP
  master clock + offset is the source of truth; we don't get to lie about
  it. Fix the consumer side instead.

## 13. Diagnostic-only / debug code left in place

* Verbose `/info` / SETUP / SETRATEANCHORTIME logging in `rtsp_handlers.c`.
* Format dump in `audio_receiver.c::audio_receiver_set_format`.
* Source-rate change log in `audio_output.c`.
* 44.1 kHz resample-disabled warning in `audio_resample.c`.
* `audio_time` early-frame log promoted to INFO with consecutive count.
* **Known leak:** RTSP/SRP keys are still dumped via `DBG S`, `DBG K`,
  `DBG srp_K`, `DBG enc(Read)`, `DBG dec(Write)` lines somewhere in
  `main/hap/` / `main/rtsp/`. Flagged in `AI_HANDOFF.md` as a cleanup
  item — gate them behind a Kconfig option before any release.

## 14. Coding conventions in this repo

* 2-space indent, K&R braces, snake_case for files and functions, typedef
  structs as `<name>_t`. No CamelCase except for ESP-IDF callbacks.
* `static const char *TAG = "<short>";` at top of every `.c` for ESP_LOG.
* All allocations large enough to matter go through `heap_caps_malloc(...,
  MALLOC_CAP_SPIRAM)` or the helpers in `spiram_task.h`.
* No floats in the audio hot path except the servo's resampler step (and
  even that uses int32 ppm internally). I2S timing is integer-clean.
* Comments explain *why*, not *what*. Most of the timing thresholds carry
  a paragraph-long comment documenting the reasoning + the past
  regression each value is defending against. **Preserve those comments
  when refactoring** — they are load-bearing institutional memory.
* When fixing a non-trivial timing bug, document the reasoning at the call
  site (the existing pattern in `audio_timing.c`) AND in a top-level
  markdown file (`AIRPLAY_SEEK_FIX.md` is the template).
* User communicates in Romanian or English; respond in whichever language
  the user wrote in.

## 15. Companion documents (read these for deeper context)

* `README.md` — user-facing hardware/build/flash guide.
* `AI_HANDOFF.md` — narrative debugging history per area (RTSP crypto,
  SPIFFS, ES8388 volume, alerts, Mac tray glitches, seek fix). The
  authoritative "what we already tried" log.
* `AIRPLAY_SEEK_FIX.md` — detailed write-up of the v3 seek fix
  (removed RTP gates, fast-forward DMA, 200 ms buffer-refill pause).
* `OPUS_LOGS.md`, `OPUS_PROMPT.md` — older debugging session captures,
  occasionally useful for "did anyone hit X before".
