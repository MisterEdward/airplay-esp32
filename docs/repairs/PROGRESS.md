# Repair progress — 2026-09-06

Original code: `078b719`, branch `fable-5.1` (verified synchronized with origin before edits).
The supplied plan is preserved alongside this report. Stages 2–6 are not started.

## Stage 1

- 1.1: failed OTA returns HTTP 500 and reboots the intact current firmware.
  This deliberately uses the plan's reboot alternative: RTSP service flags and
  client tasks are rebuilt together, without attempting partial reinitialization.
- 1.2: render scratch uses internal 8-bit RAM (13,164 bytes). Float conversion
  scratch also uses internal RAM (up to 17,264 bytes for 1025 input / 1133 output
  frames). Combined maximum: 30,428 bytes. Failed growth preserves the previous
  allocation and returns silence instead of dereferencing NULL or overrunning it.
  Optional decoder scratch moves were not needed for this change.
- 1.3: 32/64 KiB cache build compiled but the first combined OTA failed
  before network readiness and rolled back to the old `ota_0` image. The
  next boot reported `other_wdt` (7). With only the caches reverted to the
  original 16/32 KiB, the second OTA booted `ota_1` and reached `valid`.
  Cache expansion is deferred until boot diagnostics can explain the failure;
  the other stage 1 fixes remain enabled.
  Correction to the plan: ESP-IDF Kconfig explicitly says the smaller
  instruction cache returns 16 KiB to the heap and the smaller data cache
  returns 32 KiB. Increasing both caches therefore costs 48 KiB of RAM.
  This is confirmed in the installed SDK at
  `components/esp_system/port/soc/esp32s3/Kconfig.cache`.
- 1.4: one formatting pass per log line instead of two when both consoles are
  disabled. The normal console path and printf return count are preserved.
- 1.5: shared 352-frame block limit; USB scratch 4104 → 1412 bytes, saving
  2692 bytes. The original plan's larger savings estimate was incorrect.
- 1.6: non-Bluetooth HTTP client slots 3 → 5; Bluetooth settings unchanged.
- 1.7: CPU statistics use deltas between task snapshots, with task number
  matching to handle reused task handles. The first sample reports null
  percentages. A busy core is 100%; idle tasks are included. ESP-timer-backed
  64-bit counters avoid the 32-bit microsecond wrap after about 71 minutes.
  Final build passes; hardware verification is in progress.
- Added free internal heap and largest internal block to system info so
  allocation headroom can be checked during playback.

## Verification and measurement limitations

The original firmware repeatedly rebooted while the user tried AirPlay playback.
`/api/system/info` reported `reset_reason: int_wdt`; journal boot messages agreed
(reset reason 5). No repaired firmware had been uploaded at that point.

Before the first reset, internal free heap was 150,991 bytes, largest block
77,824 bytes. Later baseline boots ranged from 150,431 to 150,623 bytes free,
with the same largest block. See `before-playout.log` for retained measurements.
Only four playout reports were captured across resets. These are insufficient
for the planned uninterrupted two-minute jitter comparison. No performance
improvement is claimed from that incomplete baseline.

Host tests run with AddressSanitizer and UndefinedBehaviorSanitizer. They cover
existing core/timing behavior plus real-resampler initialization/growth failures
at either allocation, no output writes on failure, and successful retry.
Each completed logical change has a successful ESP32-S3 build and its own commit.
`format.sh` was run with clang-format 22.1.4; pre-existing formatting changes in
unrelated files were restored to avoid unrelated edits.

Full raw diagnostic captures and build/config backup are kept locally under
`/Users/edward/Documents/ChatGPT/airplay-repair-evidence/`.

Hardware OTA, invalid-upload recovery, HTTP concurrency, and idle CPU
percentages pass. Post-change playback/heap checks are in progress. The next gate is 15 minutes of
listening before proceeding to stage 2.

## First successful repaired boot

- `ota_1` reached `valid`; the journal confirms the validity marker.
- Boot internal heap: 131,875 bytes; largest block: 59,392 bytes.
- First system-info read: 130,183 internal bytes free; largest block 57,344.
- First CPU sample: null percentages, as intended. Second sample after 5 s:
  200.00094% summed across both cores, including idle.
- HTTP concurrency passes: one WebSocket remained connected while three
  persistent HTTP clients fetched `/logs`, `/`, and `/api/status` 20 times
  each (60 successful requests). A subsequent WebSocket ping received pong.
- Invalid-image test: uploading README returned HTTP 500
  (`ESP_ERR_INVALID_STATE`); the device rebooted the repaired `ota_1` image
  and again reached `valid`. Reset reason was `software`, and a fresh RTSP
  OPTIONS request returned `RTSP/1.0 200 OK`.
- After recovery, CPU sum was 199.98770%; internal RAM 130,527 bytes free,
  largest block 57,344. User playback/listening validation is pending.

## 2026-09-07, overnight — root cause found, and stage 2/3/4

### The crashes were the power supply, not the firmware

Playback died with `int_wdt` after eight to ten seconds, on both AirPlay
paths, and `cannot connect` from the phone was the same failure seen from
the other end.  `/api/system/info` then reported `reset_reason: brownout`,
and a firmware OTA failed mid-upload with `ESP_ERR_INVALID_STATE` — a flash
write is the largest current peak in the firmware, so it was the first thing
to fall over.  The board was drawing from a sleeping PC's USB port.  Moved to
a charger, playback ran with `under=0`, `gaps=0` and a playout error around
+100 to +300 us.

Two corrections to earlier conclusions.  The invalid-image OTA test recorded
above did not prove graceful rejection; the board was resetting during the
write.  And the `int_wdt` resets attributed to the original firmware were the
same supply problem, so no code in `078b719` is implicated.

The journal is in PSRAM and dies with the reset, so every crash erased its
own evidence.  The only way to see anything was to stream `/ws/logs` to a
laptop and read what arrived before the board went quiet.  That is what
stage 2 is for, and it is why it moved to the front of the queue.

### Done

- **Retained diagnostics** (`main/network/crash_log.c`).  The last 3 KiB of
  the journal is mirrored into RTC memory, which survives a core reset, and
  replayed into the new journal at boot under a `previous boot` banner.
  Reset reasons are tallied across boots and published as `boots` in
  `/api/system/info`, so `brownout: 7` states the problem instead of a
  single reason that describes only the last restart.  RTC memory does not
  survive a power cut; `boots` back at 1 is itself the signal that it
  happened.
- **Stage 4.1 and 4.2** (`audio_envelope.c`).  Constant-gain fast path, with
  a true bypass at unity — the old code ran every sample through
  `(x * 32768) / 32768`.  Rounding plus TPDF dither on the attenuating path,
  which matters here because the PCM5102A has no hardware volume.  Dither is
  confined to constant gain: during a fade the gain already moves every
  sample, and the existing tests require fades to stay monotonic and
  channel-symmetric.  Digital silence passes through untouched.
- **Host coverage for `audio_buffer`**, which had none, followed by
  **stage 3's O(1) consume**.  `sorted[]` is now a ring with a head, so
  taking a frame no longer memmoves up to 2 KB under the spinlock 136 times
  a second.  Insertion shifts the shorter side.  Tests: ordering, RTP
  wraparound, overflow eviction, flush, slot accounting, contiguous-run
  detection, plus 60k randomised operations under ASan/UBSan asserting the
  invariants after every step.

### Deliberately not done

- Nothing was flashed.  The envelope and ring changes need a listening test
  and the user was asleep; the board still runs the stage 1 image.
- The counting semaphore is still there.  Replacing it with a task
  notification changes the consumer's blocking contract and deserves its own
  change with hardware behind it.
- Stage 2.1 (coredump partition) and 2.4 (real flash size) both need a
  serial flash in download mode, which needs the user.
- Stage 2.3 (task watchdog panic) is pointless before coredump exists.
- Stages 5 and 6 are untouched, as planned.

### Next session

1. Flash and listen.  `pio run -e esp32s3` then OTA; confirm `ota_state`
   reaches `valid`, then play for fifteen minutes.  What to listen for: the
   noise floor on quiet passages at low volume should be cleaner, fades in
   and out unchanged, no clicks at track changes.  Watch `under=0` and
   `gaps=0` in the playout line — the ring rewrite would show up there first.
2. Confirm the supply fix holds: `boots.by_reason` must gain no new
   `brownout` after a long session.
3. Then the coredump partition, which needs the serial flash anyway.
