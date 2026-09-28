# Fable — working guide for future sessions

Read this before touching the audio path.  It records what this firmware
does, why the seek fix of 2026-09-23 works, how to measure instead of guess,
and what is still broken.  `docs/FABLE.md` covers the feature set (USB
speaker, PC wake, diagnostics pages); this file covers the AirPlay 2
buffered path and the way of working that finally fixed it.

Reply to Edward in Romanian.  He tests with his ears and his iPhone; you
read the journal.

---

## 1. Where things stand

**Update 2026-09-28: everything below is on `main` (4fbd9f8) and on the
board.**  Stress-test report: https://claude.ai/artifact/RTLNSYFthtMgFKNcWSFdwe.

| Commit | What changed | Verified |
|---|---|---|
| 36eaced | No click on play after a plain pause: resampler history reset when the pause fade ends, silence forced while the envelope is shut | mic: HF peak at play 27-38 dB -> 2-6 dB, 0/8 pops; Edward on the iPhone |
| 81d7d12 | PTP accepts a grandmaster relayed by the source it tracks (§7) | group runs Mac+TV: `relayed by tracked source … keeping it`, every anchor `domain=ptp` |
| 4835c3c | Transient pair-setup ~0.15 s instead of ~1.6 s (SRP: 256-bit private exponent, Barrett reduction on the MPI unit, precomputed M1 key pool) | host KAT tests; iPhone pairing M1 71 us, M3 141 ms; click->first sample 3.61 -> 1.73 s (TCL TV: 2.23 s) |
| 32c6402 | WiFi: clean deauth before every restart; watchdog (no IP 180 s -> restart, again -> deep sleep 1 s) | rejoined alone after every later OTA |
| 5274cf2 | PTP multicast re-join after 5 s of silence in a session (`ptp.rejoins`) | after a 2.4 GHz collapse PTP came back without reboot |
| db2f0ac, 34da543, 0838870, 227943e, 4fbd9f8 | USB capture of what the DAC plays + DAC mute (debug; env `esp32s3-capture` only) | 4 h soak recorded on the wire |
| 73ab243 | Stress harness, capture recorder, report generator (`scripts/fable/stress/`) | used for all numbers here |

Measured: 4 h soak on the wire (Mac Music, a seek a minute, a storm every
15 min): 0 resets, 0 gaps, 0 underruns, every anchor PTP, |err| p50 126 us,
seek median 0.68 s, internal heap stable (~95-107 KB free while streaming,
largest block ~31 KB).  Still open: see §7 "Fade cut at seek" and "Short
dropouts with the iPhone".

Board on 2026-09-28: on a plain 5 V USB charger (not the PS5), running the
capture build of 4fbd9f8 with capture and mute off.  Before it goes back
behind the PS5, OTA the normal build (`pio run -e esp32s3`).  Ping on the
charger: 0 % loss, but a regular 75-90 ms spike every 15 s while an iPhone
session is active (4-9 ms between spikes); not investigated.

Earlier findings that still hold:
- The "phantom session" (Music shows playing, no sound) is Music.app's own
  idle-timeout disconnect (§7).  Not firmware.
- The 2.4 GHz link collapses at times when the board is powered from the PS5
  (ping ~85 ms, 20 % loss, RSSI unchanged); on the Mac's or a charger's power
  it stays fine.

`main` is the only branch.  It is `55cfa44` (the build whose multiroom sync
Edward confirmed as instant) plus the fixes below.  Everything older is kept
as tags, not branches:

| Tag | What it was |
|---|---|
| `archive/main-0.1.20` | upstream 0.1.20, the old `main` of this fork |
| `archive/fable-5.1` | the first Fable line; its later commits broke sync (see §8) |
| `archive/baseline-55cfa44` | 55cfa44 + the 2026-09-07 attempts, up to `b198d85` |
| `archive/codex-*`, `archive/feat-dual-airplay-v1-v2` | older experiments |

Measured on the iPhone 12 mini with Apple Music, Bedroom Speakers + the
MacBook as a multiroom group, after the fix:

- seek: sound 0.41-0.91 s after FLUSHBUFFERED, 0 frames lost (resumes at
  the chosen verse), first sample 6-38 µs from the anchor schedule;
- pause/resume: ring kept, sound when the sender schedules it (~0.6 s);
- whole session: `gaps=0 under=0`, no slow RTSP handler.

Definition of done, in Edward's words: sync as impressive as 55cfa44, seek
under a second, resume at the verse, no silent "playing" dead time, no
pops — "behave like an Apple product".

### Handover — start the next session here (as of 2026-09-28)

1. **The board runs `main`** (capture build, see above).  The old
   `wip/ptp-grandmaster` tag is merged as 81d7d12.
2. **Next audio work, in order**: the fade cut at seek (§7), then the
   iPhone-only short dropouts (§7, needs Edward and the phone), then the
   session-start follow-ups on branch `perf/session-start-rtsp-path`
   (event port 100 ms, keep packet pool, accept in 10 ms, warm PTP re-lock;
   the last one touches sync, so ear test).  Measure with plan `sstart`.
3. **Hardware plans discussed** (not started): Seeed XIAO ESP32-S3 Plus with
   an external antenna (same code, new pin map in a
   `config/sdkconfig.user.*`); longer term, wired Ethernet (W5500 on the S3
   is already supported) or 5 GHz.  The CPU was never the bottleneck: the
   decoder uses ~9 % of a core; the link was.  Options weighed:
   ESP32-C5 (5 GHz, but single core and no USB OTG, so no PC speaker),
   ESP32-P4 + C5/C6 companion (everything, most complex), ESP32-S31
   (dual RISC-V 320 MHz, Gigabit Ethernet MAC, USB HS, BT Classic back,
   but 2.4 GHz only; new silicon: needs a recent ESP-IDF and a
   `esp_audio_codec` AAC build for it — check both before buying).

---

## 2. The board

- ESP32-S3 + PCM5102A, "Bedroom Speakers", **192.168.68.104**.  `.103` is
  a smart-home device: never touch it.
- Build: `~/.platformio/penv/bin/pio run -e esp32s3`.  In a fresh worktree
  run `git submodule update --init --recursive` first (u8g2), and delete
  `sdkconfig.esp32s3` after changing any `sdkconfig.defaults*` file.
- OTA (the normal path):
  `curl -X POST --data-binary @.pio/build/esp32s3/firmware.bin http://192.168.68.104/api/ota/update`
  then poll `/api/status` until it answers.
- The bootloader on the board is currently **Sol's** (flashed by cable on
  2026-09-07), which has no rollback: `ota_state` shows `new`/`undefined`
  and a broken image will not roll back.  A full cable flash of this repo
  (`pio run -e esp32s3 -t upload` + `-t uploadfs`) restores Fable's
  bootloader and web pages; the SPIFFS pages on the board are also Sol's.
- Cable flashing: native USB port only; hold BOOT, tap RESET, release BOOT.
  If GPIO0 stays low it re-enters download mode on every reset: ask Edward
  to release BOOT and press RESET.  No serial console by design.
- Power: a sleeping PC's USB port browned it out (2026-09-07).  It now runs
  from the PS5's USB and boots reliably.  If anything behaves inexplicably,
  check `reset_reason` in `/api/system/info` first.
- RF: its 2.4 GHz link is weak where it sits (behind/under the TV stand with
  the PS5).  Ping to it measured 38 ms quiet and 0.1-2 s while a PS5 game
  ran, while other 2.4 GHz devices stayed at ~40 ms.  Check
  `ping 192.168.68.104` before trusting any timing measurement.  Edward
  plans to move to a Seeed ESP32-S3 (16 MB flash / 8 MB PSRAM) with an
  external antenna; its pin map will need a `config/sdkconfig.user.*`.

Logs: `curl http://192.168.68.104/api/logs/download` (the PSRAM journal,
~190 KB).  Per-tag level: `POST /api/logs/level {"tag":"audio_time","level":"debug"}`.
Never set `*` to debug.

---

## 3. How the AirPlay 2 buffered path works

```
sender ──TCP──▶ reader task (prio 5)          audio_stream_buffered.c
                  │ parse seq/rtp; drop old backlog at the socket
                  ▼
               900 compressed slots in PSRAM (~20 s)
                  ▼
               decoder task (prio 4, core 1)   hold until anchor, RTP gates,
                  │                             decrypt, AAC decode
                  ▼
               PCM ring (~7 s, "buffered=900")  audio_buffer.c
                  ▼
               timing engine                    audio_timing.c: anchor →
                  │                             exact silence/trim, servo
                  ▼
               render task (prio 9, core 1)     audio_output.c: envelope,
                                                resample 44.1→48 k, I2S
```

The RTSP side (`rtsp_handlers.c`) drives it:

- **SETUP** advertises `audioBufferSize`.  The sender keeps that many bytes
  of lead in flight.
- **SETRATEANCHORTIME** rate=1 carries the anchor: "rtp X plays at PTP time
  T" on timeline `networkTimeTimelineID`.  rate=0 is pause (fade out).
- **FLUSHBUFFERED immediate** (no `flushFrom*`): a seek or skip.  It always
  carries `flushUntilSeq`/`flushUntilTS`; the first packet of the new
  position has `seq == flushUntilSeq` (a 36-byte odd one), real audio
  starts at `until+1`.
- **FLUSHBUFFERED deferred** (`flushFrom*` + `flushUntil*`): "discard the
  packets in [fromSeq, untilSeq)".  Sent on pause (and alongside the
  immediate flush of a seek).  Measured on the iPhone: the range starts
  exactly at the end of the current track (from - playhead = the time left
  in it) and covers packets the phone **never sends** — it just skips those
  sequence numbers, and the next track arrives at `untilSeq` with
  `rtp == flushFromTS`, contiguous.  After a resume it may send one or two
  more tiny ranges with the same `from`.  Applied by sequence number (§4,
  fix 5).  All raw seq values from RTSP carry bit 23: mask with `0x7FFFFF`.

After an immediate flush: `audio_receiver_seek_flush_until()` bumps the slot
generation (queued packets become stale), resets timing, and arms
`discard_all_until_anchor` + `flush_until_seq`.  The reader drops packets
below `flushUntilSeq` and hands the rest to the decoder, which holds them
until the anchor arrives, then releases them through the RTP gates.  The
timing engine places the first sample exactly on the anchor's schedule
(`Acquired: err=…`).

---

## 4. The seek fix, and why it works

### What was wrong

For weeks the symptom was: after a seek the speaker stayed silent for
seconds (sometimes tens of seconds), then came back late or out of sync.
Holding post-seek packets "made the anchor late"; dropping them "fixed" it
but started ~0.5-1 s past the chosen point and popped.  That trade-off was
a symptom.  The cause, measured on 2026-09-23:

**We advertised `audioBufferSize` = 1 MiB (~32 s of AAC) but can hold only
~28 s.**  So the reader back-pressured forever, the TCP window stayed
closed, and the surplus waited in the *sender's* socket.  Polling the Mac's
`netstat -anp tcp` Send-Q towards the speaker's data port showed
129-131 KB queued for the whole session.  After every seek those ~4 s of
old audio had to cross our tiny TCP window (5760 bytes) before a single
packet of the new position could arrive.  With the Mac as sender: 135-183
old packets first, the first new one 1.5-2.1 s after the flush, sound at
2.5 s, 1.5 s of the new position skipped to stay in sync.

On the phone this was most likely worse, because the phone writes its
post-flush burst before it sends SETRATEANCHORTIME, so the anchor itself
waits for the drain.  That fits the old "anchor 10-12 s late,
lead=-12001 ms" captures (not re-measured at 1 MiB on the phone, but gone
on the phone with the fix).  None of the earlier suspects held up: the hold
queue's rate or size, the recycle delay, a deaf sender, the RTP gate.

### The fixes

1. **`fix(airplay): seek resumes at the chosen point in under a second`**
   - `audioBufferSize` 512 KiB (~16 s, fits): the sender's Send-Q stays 0.
   - Immediate flush passes `flushUntilSeq` down; the reader drops only
     packets below it (old backlog), at the socket, and **holds** the new
     position until the anchor (seek hold mode 1, the default).  Resume is
     at the exact chosen sample and the sender's lead is kept, so the ring
     refills to 900 in ~4 s with no sawtooth and no 23 ms holes.
   - The decoder's hold loop used `vTaskDelay(pdMS_TO_TICKS(2))`, which is
     0 ticks at 100 Hz: a busy spin at priority 6.  Now ≥ 1 tick.
   - Diagnostics: the seek trace log lines and `/api/debug/seek_mode`.
2. **`perf(net): 24 KiB TCP window; decoder yields to RTSP while refilling`**
   - `CONFIG_LWIP_TCP_WND_DEFAULT=24576`, `TCP_RECVMBOX_SIZE=24`,
     `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=48` (S3 defaults only).  Throughput is
     window / RTT; at 0.5-0.9 s RTT the old window gave 12-21 KB/s, less
     than the music needs (~32 KB/s).  A second seek within the post-seek
     burst used to find 160-190 old packets; now none.
   - The faster pipe made the decoder decode ~900 packets back to back at
     priority 6, starving the RTSP task (a SETRATEANCHORTIME handler stalled
     1.35 s mid-log line).  Decoder now at priority 4: it needs ~9 % of a
     core and the ring holds 7 s.
3. **`fix(airplay): resume after a long pause no longer throws the buffer away`**
   - Pause ends on the render task's fade path, `audio_receiver_pause()`,
     which never took the pause snapshot.  On resume, "Path B" estimated
     the pause position from the wall clock *including the pause*, so any
     pause > 5 s looked like a seek and flushed 7 s of audio the sender
     never resends: 7.6 s of silence.  Now routed through
     `audio_receiver_set_playing(false)`.

4. **`fix(buffered): the flushUntilSeq gate never outlives its flush`**
   - Fix 1's gate stayed armed when a session ended right after a seek; the
     next session's packets all compared "below" it and were dropped: a
     reconnect played nothing.  Cleared on every flush and stream start;
     anything more than 16384 below `until` disarms it.
5. **`fix(airplay): deferred FLUSHBUFFERED drops its sequence range, nothing else`**
   - The timing engine used to empty the whole ring when a frame reached
     `flushUntilTS`: ~7 s of silence plus a click at the start of the next
     song after a pause near a track's end.  Now up to four ranges, dropped
     by sequence number in the reader (and the decoder for queued ones), as
     shairport-sync does; an immediate flush cancels them.  Verified on the
     iPhone: pause 44 s before the end, three ranges, `dropped 0 packets`,
     clean transition.
6. **`fix(ptp): follow a master timescale step instead of rejecting it forever`**
   - **While paused, the iPhone's PTP timescale does not advance normally:
     it jumps back in steps of ~2-6 s every ~10-12 s** (16 steps in a
     4.5-minute pause).  The 50 ms outlier gate rejected every sample after
     the first jump and kept the old offset, so after resume every anchor
     looked 11.4 s in the past: ~14 s dropped as late, stuttering, while the
     Mac followed.  A run of ≥ 12 samples over ≥ 1.5 s, ≥ 1 s away and
     agreeing within 30 ms, is now followed.  Host test in
     `tests/host/test_ptp.c`; `ptp.steps` in `/api/status`.  Verified: after
     a 4.5-minute pause the resume anchor had `lead=-17 ms`, `err=+0 us`.

The timing engine's alignment, envelope, servo and anchor judging are
unchanged.  That is deliberate: those are what make 55cfa44's sync instant.

---

## 5. How to measure (do this, not theories)

**Be your own sender.**  The Mac's Music app is scriptable, including
AirPlay device selection, so you can reproduce without Edward:

```bash
osascript -e 'tell application "Music" to get name of every AirPlay device'
scripts/fable/seektest.sh mytag          # 4 seeks, 11 s apart
scripts/fable/seektest.sh mytag rapid    # pairs of seeks 3 s apart
python3 scripts/fable/seeksummary.py /tmp/fable-tests/mytag.log
```

`seektest.sh` also records the Mac's Send-Q (`sendq-mytag.txt`); healthy is
0 throughout.  The iPhone is the final test; ask Edward for it and send a
push notification when you need him.

**Runtime A/B without reflashing:**
`GET /api/debug/seek_mode?m=0|1&kb=N`.  `m=0` drops every pre-anchor
packet (the old b198d85 behaviour), `m=1` holds (default).  `kb` is the
advertised `audioBufferSize`; it applies from the next SETUP (deselect and
reselect the speaker).  Knobs reset to the defaults on reboot.

**Raw pipe:** `curl -X POST --data-binary @1mb.bin http://192.168.68.104/api/speedtest/upload -w '%{speed_upload}'`.

**Stress harness and capture (2026-09-27):** `scripts/fable/stress/`
(README there).  Mic-based or, better, on the wire: the capture firmware makes
the board a USB input ("Bedroom Speakers Audio") carrying exactly what goes to
the DAC, and can mute the DAC (`GET /api/debug/capture?on=1&mute=1`, off at
boot).  Mac-side AirPlay logs: `/usr/bin/log stream --level debug` on
AirPlayXPCHelper (debug lines are not persisted, stream them live).
Plugging the board into the Mac once left it in ROM download mode
(303a:1001, `boot:0x0`): press RESET.

**Log lines that matter** (tags `audio_buf`, `audio_recv`, `audio_time`,
`rtsp_handlers`):

| Line | Healthy |
|---|---|
| `FLUSHBUFFERED immediate untilSeq=… untilTS=…` | always has until |
| `Seek: first new-position packet +N ms … before it K old packets` | N < 250, K ≈ 0 |
| `Seek trace: anchor +N ms after flush, mode=1 …` | N 150-700 |
| `Anchor: … lead=-L ms domain=ptp` | small negative L, **domain=ptp** |
| `Acquired: … err=E us silence=… dropped=D … start=quick` | \|E\| < 100, D = 0 |
| `Playout: err=… buffered=… gaps=G under=U` | buffered→900, G = U = 0 |
| `SETRATEANCHORTIME slow: anchor … state+events … reply …` | never |
| `rtsp_events: listener N took …` | never |
| `Anchor change detected` right after a plain pause | never (that was bug 3) |
| `PTP not locked to … local timeline latched` | never; means unsynced |
| `Master timescale stepped by … following it` | only while/after a pause |
| `Deferred flush done: dropped N packets of [a, b)` | N = 0 is normal |
| `ptp_gap` in Playout, `ptp.steps` in `/api/status` | gap within ±10 ms |

---

## 6. Rules of engagement (learned the hard way)

- **One change at a time**, flashed and listened to before the next.  Six
  changes flashed together on 2026-09-07 lost the sync and cost a day.
- **Multiroom sync must not regress.**  Anything touching `audio_timing.c`,
  the envelope, the anchor or PTP needs Edward's ear immediately.
- **Measure the sender too.**  The breakthrough came from looking at the
  Mac's Send-Q, i.e. from outside the firmware.  The firmware's logs alone
  had supported five wrong theories.
- Check the network (ping) and the power (`reset_reason`) before blaming
  code.
- Keep A/B knobs for risky behaviour so the comparison needs no reflash.
- Don't shorten the anchor fallback to "fix" silence: starting unscheduled
  is by definition out of sync with the other speaker.
- A phone that went through a deadlocked session can stay wonky until
  airplane mode on/off; ask for that before blaming a new build.

---

## 7. Known bugs, not fixed yet

### PTP: the anchor names a grandmaster that only reaches us relayed

**Fixed in 81d7d12** (on `main`, exercised in the Mac+TV group runs: the
journal shows `PTP timeline … is relayed by tracked source …: keeping it`).
Kept here for the analysis.  Seen twice: with the Mac as sender and the iPhone around, and with the
iPhone as sender after an airplane-mode reset made another device
(`cc6146fffefc2f15`) the group's grandmaster; the phone kept sending Sync,
now carrying that clock's time (a 43 h timescale jump the step detector
followed), the anchors named `cc6146…`, and every seek played
`domain=local`, 1.3-5 s to sound.  The original analysis:


The anchor names the phone's clock (`a8817e25…`) as timeline, but on the
wire only the Mac (`3c06307f…`) sends Sync, relaying the phone's time as a
boundary clock.  `ptp_clock_set_master_clock_id()` switches to the named
clock, never gets samples from it, and after 1.5 s the timing engine
latches the local timeline: `PTP not locked … local timeline latched`.
`ptp_clock.c` counts Announce messages but ignores `grandmasterIdentity`
(offset 53 in the Announce body).  Fix: remember each source's announced
grandmaster and treat "anchor names X, tracked source announces X as GM"
as a lock.  iPhone-as-sender is unaffected (the phone sends its own Sync).
This touches sync: listening test mandatory.

### Fade cut at seek

17 of 21 Mac seeks recorded on the wire (capture, post-envelope) keep full
level until the last ~10 ms and then drop to zero: no 100 ms fade-out.  A
plain pause fades perfectly.  On the wire the sender sends rate=0 at +15 ms and
`FLUSHBUFFERED immediate` at +65..145 ms.  Lead: `rtsp_handlers.c` ~1848 ->
`audio_receiver_seek_flush_until` -> `audio_receiver_flush()` empties the PCM
ring from the RTSP task; the render loop then has no media and cuts the
envelope.  But even the 50-130 ms between rate=0 and the flush shows no ramp,
so check first whether the pause fade starts at all on a seek.  Fix idea: let
the fade finish on audio already held before emptying the ring.  Touches the
envelope: ear test.

### Short dropouts with the iPhone

60-70 ms holes of digital silence between audible audio, 3 in 2.5 min with the
iPhone as sender (capture run `phone2`), 0 in 20 Mac seeks.  One came ~110 ms
after a seek's `Acquired`, one ~1.2 s after a re-acquire.  Suspects: the AAC
decoder reset on the 36-byte packet at `untilSeq` (every seek logs one decode
error), or a quick start with only 1-2 frames buffered starving.  To capture:
board on the Mac's USB, `CAPTURE=1 python3 run.py phoneN esp listen30` while
Edward plays from the phone, then `python3 holes.py $FABLE_RUNS/phoneN`.

### Music's idle-timeout disconnect (sender side)

After some scripted seeks Music arms an idle timer that is not cancelled and
~73 s later disconnects the AirPlay device while playback continues; Music
then plays on the Mac's own speakers or shows "playing" with no session.
The AirPlay side of a fatal seek is identical to a good one.  Workaround in
tests: `scripts/fable/stress/guard.sh` reselects the speaker.

### WiFi 2.4 GHz collapse and PTP

See §1.  Firmware only mitigates: clean deauth before restarts, a watchdog
(no IP for 180 s -> restart, then deep sleep 1 s), and a PTP multicast re-join
after 5 s of silence during a session (`ptp.rejoins`).

### Step detection lag at resume

The PTP step detector needs ~1.5 s.  In the verified run the phone's last
step landed ~2.1 s before its resume anchor, so it was caught in time.  If
a phone ever anchors less than 1.5 s after its last step, that first anchor
is judged on the old offset and the next re-acquire corrects it (an audible
jump).  Watch for `Master timescale stepped` right *after* an `Anchor:`.

### Smaller

- Found by the 2026-09-26/27 stress campaign, all inaudible so far: the
  `gaps` counter can be bumped by a deferred-flush/seek race; every seek logs
  one AAC decode error on the 36-byte packet at `untilSeq` (may relate to the
  iPhone dropouts); a grandmaster change mid-play once gave `Anchor 12242 ms in
  the future`; the first acquisition after a rapid reconnect can be off by up
  to -7.7 ms (corrected by the next re-acquire); each disconnect blocks ~1 s on
  the event port (fix on `perf/session-start-rtsp-path`).
- A small click on play after a pause was reported once; the resume there
  re-acquired with `trimmed=116 dropped=2` (anchor 55 ms in the past).  The
  PCM5102A's own zero-data auto-mute is the other suspect.  Not isolated.
- One isolated ~8 ms render stall (three re-acquires within 110 ms) was
  seen once in a long session.  Not reproduced.

- On a bad 2.4 GHz link an RTSP reply can still sit in `send()` for
  seconds (`SETRATEANCHORTIME slow: … reply 4664 ms`); the next request
  waits behind it.  Network, but the RTSP task could be made to not block.
- `archive/fable-5.1` has later work that is **not** on `main`: TPDF
  dither and a constant-gain fast path, the O(1) PCM ring, the retained
  crash log and reset tally, task CPU stats, cache tuning (see its
  `docs/repairs/PROGRESS.md`).  Port them one at a time if wanted; §8 says
  which were suspected in the 2026-09-07 sync regression.

---

## 8. History, briefly

- `v0.2.0` upstream → Fable 5.1 (`archive/fable-5.1`): render task, exact
  alignment, tiered servo, PTP tracking, compressed hold queue, USB speaker.
- `55cfa44`: the build with instant multiroom sync; seek could go mute.
- After it, `c8fd852` (anchor fallback 10 s → 1.5 s), `de65dde` (anchor
  judged "projected to now"), `d7900d2`/`5e10db7` (dither, PCM ring) were
  flashed together and sync was lost; never isolated.  Suspects in that
  order.
- 2026-09-07 (`archive/baseline-55cfa44`): ten attempts at the seek on top
  of 55cfa44, ending with "drop pre-anchor packets at the socket" — seek
  worked but late and popping.  The brownout was found the same day.
- 2026-09-23: root cause measured from the sender's side; the six fixes
  of §4, each verified on the iPhone.
