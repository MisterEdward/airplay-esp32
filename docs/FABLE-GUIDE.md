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
  packets in [fromSeq, untilSeq)".  Sent at track transitions and on pause.
  **Handled wrongly today**, see §7.

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

### The three commits

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

Nothing in the timing engine, envelope, servo or anchor judging changed.
That is deliberate: those are what make 55cfa44's sync instant.

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

### Deferred FLUSHBUFFERED applied as "flush everything"

`audio_timing_read()` treats a deferred flush as: play until a frame with
rtp ≥ `flushUntilTS`, then empty the whole ring.  The protocol means
"discard the packets with seq in [fromSeq, untilSeq)".  Measured after a
pause on the iPhone: the ring was emptied at the boundary, 7 s of silence
and a click (`Deferred flush at ts=…` followed by `early=6958 ms`).  Also
the cause of the old "next with crossfade stalls".

Do it the way shairport-sync does (`ap2_buffered_audio_processor.c`): by
**sequence number**.  The phone re-sends the same rtp range with new
sequence numbers after a pause, so any timestamp-based skip (including
`archive/fable-5.1`'s 169bb56) also throws away the resent audio.  Packets
still in the compressed slots carry `seq_no`; frames already in the PCM
ring do not, so either add the sequence number to `audio_frame_header_t`
or keep the deferred range as a skip list applied at playout by seq.
Instrument first: log seq/rtp of packets around each deferred flush, like
the seek trace.

### PTP: Mac as sender with the iPhone around → unsynced

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

### Smaller

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
- 2026-09-23: root cause measured from the sender's side, the three
  commits above.
