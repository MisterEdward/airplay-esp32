# ESP32-S3 AirPlay receiver: engineering history and hardening record

## 1. Purpose of this file

This document is the durable engineering record for the custom ESP32-S3 +
PCM5102A firmware. It is intentionally more detailed than a normal changelog.
It records:

- the exact repository and device being changed;
- the upstream baseline;
- every custom behaviour change;
- the evidence that motivated each change;
- implementation details and concurrency assumptions;
- build, OTA and validation procedures;
- known limitations;
- ideas that were reviewed and deliberately rejected;
- future work that can be added without rediscovering the entire investigation.

The short Git commit history remains the authoritative record of source
changes. This file explains why those commits exist and how they interact.

## 2. Project identity

### 2.1 Active working copy

The active S3 project is:

```text
/Users/edward/airplay-esp32/.claude/worktrees/airplay-esp32-seek-buffer-69494b
```

The active Git branch is:

```text
claude/audio-mix-transition-fixes-403fbd
```

Earlier sections of this history referenced the retired
`s3-seek-telemetry-v29` worktree; the custom commits listed below were carried
into the current branch unchanged.

The parent repository is:

```text
/Users/edward/airplay-esp32
```

The parent repository currently contains the older ESP32-A1S development
history. The S3 worktree is separate so changes made for PCM5102A do not
silently alter the archived A1S firmware.

### 2.2 Hardware and network target

- MCU: ESP32-S3.
- Audio DAC: PCM5102A over I2S.
- Target IP: `192.168.68.105`.
- Verified target MAC: `A4:CB:8F:F8:2F:14`.
- Firmware version exposed by the API: `0.1.29`.
- PSRAM observed at runtime: approximately 6.8 MiB free during playback.
- Internal RAM observed at stream start: approximately 183 KiB free, with a
  largest contiguous internal block of approximately 90 KiB.

The MAC is checked before OTA so an address reassignment cannot accidentally
flash a different device.

### 2.3 Upstream baseline

The S3 branch starts from upstream tag `v0.1.29`, commit `1c68f12`.

Custom S3 commits before the hardening work in this document:

1. `7a62ae7` — add seek telemetry and browser-visible log streaming.
2. `112cb36` — preserve the selected seek head with a compressed-packet queue
   and split TCP reader/AAC decoder tasks.
3. `80cb059` — add a modular PCM5102A audio profile, copy the ESP32-A1S volume
   curve and add a small bass shelf adjustment.

The source still reports version `0.1.29` because these are local reliability
changes on top of that release, not a new upstream release.

## 3. Operational rule for this project

Every firmware behaviour change is handled independently:

1. make one bounded change;
2. inspect the diff;
3. build the `esp32s3` PlatformIO environment;
4. commit the change;
5. verify IP and MAC;
6. upload the generated firmware by OTA;
7. wait for reboot;
8. verify the system API and relevant boot/runtime log;
9. update this history with the result.

This intentionally costs more time than batching everything into one flash.
It makes regressions bisectable and gives every on-device behaviour a known
commit boundary.

## 4. Current architecture

### 4.1 Buffered AirPlay 2 audio path

The Apple Music path seen in the supplied logs is stream type `103`, AAC at
44.1 kHz with 1024 decoded samples per compressed packet.

The pipeline is:

```text
TCP socket
  -> buffered reader task
  -> 32-slot compressed packet queue in PSRAM
  -> AAC decoder task
  -> sorted decoded PCM buffer in PSRAM
  -> audio timing consumer
  -> software volume and PCM5102A tone profile
  -> I2S DMA
  -> PCM5102A
```

The reader/decoder split is important. TCP reception can continue while the AAC
decoder is busy. During a seek, compressed packets can be retained until the
new anchor arrives instead of losing the first word selected by the user.

### 4.2 Timing clocks

AirPlay 2 uses PTP. Each decoded frame has an RTP timestamp. A timing anchor
maps an RTP timestamp to PTP network time. The consumer computes when each
frame must reach I2S after subtracting the known hardware and pipeline latency.

Frames that are early are held. Frames that are late are discarded until an
on-time frame is found. Immediate seek recovery uses `quick_start` so the
consumer can start with one frame instead of waiting for the normal buffer
target.

### 4.3 Audio profile

The PCM5102A software-volume path uses the modular profile in:

```text
audio_profiles/pcm5102-a1s.json
```

Current values:

- AirPlay midpoint `-15 dB` maps to output `-30 dB`;
- AirPlay maximum `0 dB` maps to output `-6 dB`;
- bass shelf is `-2 dB` at `180 Hz`;
- the profile is regenerated with `scripts/apply_audio_profile.py`.

## 5. Full-log analysis: 2026-07-20

### 5.1 Input capture

The full browser log supplied for this investigation contains 874 text lines
and approximately 143 KiB. It covers roughly 699 seconds of device time, from
about `474.9 s` to `1174.1 s` after boot.

The capture contains:

- one fresh AirPlay setup;
- two manual seek transitions;
- a long stable buffered playback interval;
- two Apple Music Audio Mix / automatic track transitions;
- browser log-client disconnect warnings;
- no panic, abort, watchdog reset, brownout, allocation failure, decoder
  failure or TCP receive stall.

### 5.2 Healthy startup evidence

At `475.466 s`, Apple configures AAC, 44.1 kHz, stream type 103. At `475.486 s`,
the 32-slot compressed packet queue is allocated in PSRAM.

At `475.716 s`, the first timing anchor arrives. PTP is not yet locked at that
instant. Playback begins at `475.956 s`, approximately 240 ms later.

At `476.536 s`, PTP locks with approximately 205 microseconds of reported
deviation. That lock time is normal and is far shorter than the long playback
session that follows.

The runtime memory line reports:

```text
182891 bytes internal free
90112-byte largest internal free block
6861460 bytes SPIRAM free
```

Nothing in this memory state indicates imminent heap exhaustion or dangerous
fragmentation.

### 5.3 Healthy steady-state evidence

During normal buffered playback, a typical one-second telemetry window is:

```text
rx=43 compressed packets
decoded=129 PCM chunks
dropped=0
late=0
underrun=0
decoded buffer approximately 900 frames
```

The repeating `43/129/0` pattern is consistent with one AAC packet being split
into about three 352-sample PCM chunks. It is not triple decoding or a counter
error.

The buffer remains near 900 decoded chunks for many minutes. There is no
evidence that WiFi, TCP, AAC decoding or I2S gradually loses throughput.

### 5.4 Manual seek behaviour in the capture

Seek generation 3:

- immediate FLUSHBUFFERED begins around `509.426 s`;
- anchor arrives about 655 ms after the seek begins;
- stale upper-window packets are rejected;
- quick-start playout begins about 1.156 seconds after seek start;
- the initial recovery window records 43 late-frame discards and 7 underrun
  reads;
- subsequent one-second windows return to zero drops and zero underruns.

Seek generation 4:

- immediate FLUSHBUFFERED begins around `530.596 s`;
- anchor arrives about 574 ms after the seek begins;
- quick-start playout begins about 970 ms after seek start;
- the initial recovery window records 20 late-frame discards and 3 underrun
  reads;
- steady state again returns to zero.

These recovery drops are bounded to the seek. They do not explain later cuts
during automatic Apple Music transitions.

### 5.5 The actual Audio Mix cut: transition one

At approximately `852.846 s`, Apple sends three deferred FLUSHBUFFERED requests
within 170 ms:

```text
seq 6189482..6189687, ts 211574962..211785255
seq 6188039..6189687, ts 210097158..211574962
seq 6189688..6189921, ts 210097158..210336774
```

Important observations:

1. The ranges overlap.
2. Their RTP timestamps are not monotonic with sequence number.
3. The old implementation stored only one `flush_until_ts` value.
4. Each request overwrote the previous request.
5. The last request therefore changed the active boundary to `210336774`.

At `880.266 s`, the PCM consumer reaches that timestamp and executes the old
bulk-flush behaviour. It destroys the entire sorted PCM buffer, including
valid audio already decoded after the requested removal ranges.

The next surviving frame has RTP timestamp `210643974`. Relative to the still
valid anchor, that frame is `6951.9 ms` early. The timing engine correctly holds
an early frame, so the output is silence until `887.226 s`.

Measured audible cut: approximately 6.95 seconds.

This is deterministic receiver behaviour. It is not an ESP32-S3 performance
failure and not a random Apple Music network pause.

### 5.6 The actual Audio Mix cut: transition two

At `1108.946 s`, Apple sends another deferred FLUSHBUFFERED range:

```text
seq 6202383..6202466, ts 222857817..222922247
```

At `1165.656 s`, the old consumer reaches `222922329`, bulk-flushes the decoded
buffer and retains a next frame at `223229529`.

That next frame is `6948.7 ms` early. Playback resumes at `1172.616 s`.

Measured audible cut: again approximately 6.95 seconds.

The nearly identical duration in two independent transitions proves the cut is
the destroyed prebuffer plus anchor hold, not random WiFi jitter.

### 5.7 Harmless warnings and misleading telemetry

`httpd_sock_err: error in recv : 104` is the web log client closing/resetting
its TCP connection. It is unrelated to audio.

Values such as `4294965287` in the first telemetry window after a stream reset
are unsigned counter underflow. The telemetry task subtracts new zeroed stream
counters from an older snapshot. Playback state is not corrupted. The health
telemetry hardening described below resets its baseline whenever counters move
backward.

## 6. Deferred FLUSHBUFFERED correctness fix

### 6.1 Protocol interpretation

A deferred AirPlay 2 FLUSHBUFFERED request describes a packet sequence range to
remove. It is not a command to play until one RTP timestamp and then destroy
all later audio.

Reference behaviour was checked against the current shairport-sync AirPlay 2
implementation. That implementation:

- retains multiple deferred requests;
- activates and terminates requests by 23-bit packet sequence number;
- discards only packets inside each requested range;
- does not bulk-flush every valid packet that follows the range.

The S3 implementation now follows the same central rule while fitting the
smaller embedded architecture.

### 6.2 Data model

The timing state contains eight deferred request slots. Each slot records:

- `from_seq`;
- `until_seq`;
- `from_ts` for diagnostics;
- `until_ts` for diagnostics;
- an expiry time;
- whether the slot is in use.

Eight slots are sufficient for the three-request Audio Mix burst observed in
the log while leaving room for duplicates or a second closely-following
transition.

Requests expire after 120 seconds. The observed request-to-transition delay is
well below that value. Expiry prevents stale sequence ranges surviving until a
distant 23-bit sequence wrap and also recycles slots if a sender abandons a
transition.

### 6.3 Sequence arithmetic

Apple buffered packet sequence numbers use 23-bit modular arithmetic. Incoming
and requested sequence numbers are masked to `0x7fffff`.

Range membership uses signed modular subtraction, assuming compared values are
less than half the 23-bit sequence space apart. A range is half-open:

```text
[fromSeq, untilSeq)
```

The first packet at `untilSeq` is retained. This matches the reference receiver
behaviour and avoids deleting the head of valid audio after the removed region.

### 6.4 Filtering at two pipeline stages

Filtering only before AAC decoding is insufficient because Apple can send a
FLUSHBUFFERED request after relevant packets have already been decoded into the
deep PCM buffer.

Filtering only at the PCM consumer wastes CPU and can require several consumer
iterations to drain a large removal range.

The implementation therefore filters twice:

1. The buffered decoder checks compressed packet sequence before decrypt and
   AAC decode. Future range packets are cheap to discard.
2. Every decoded PCM chunk retains its source packet sequence number. The
   timing consumer checks that sequence, catching frames decoded before the
   request arrived.

This dual-stage design preserves the valid prebuffer and removes both already
decoded and not-yet-decoded portions of every request.

### 6.5 Concurrency

Deferred requests are written by the RTSP client task and read by the buffered
decoder and audio playback tasks. The small request table is protected by a
dedicated FreeRTOS critical-section lock.

The critical section performs only fixed-size integer comparisons and counter
updates. It does not allocate, log, decode, copy PCM or access a socket while
locked.

Duplicate requests refresh the expiry of the existing slot and increment a
diagnostic counter. They do not consume another slot.

### 6.6 Behaviour deliberately removed

The following old sequence is removed:

```text
reach flushUntilTS
  -> return current frame
  -> flush the entire decoded PCM buffer
  -> reset playout_started
  -> enable quick_start
  -> wait for the next surviving frame's anchored time
```

That sequence was the direct source of both measured 6.95-second cuts.

## 7. Hardening package plan

The hardening package is deliberately split into independent commits and OTA
images.

### 7.1 Add-on A: deferred range correctness and idempotency

Status at implementation checkpoint:

- source implementation complete;
- `pio run -e esp32s3` passed;
- firmware size: 1,445,515 bytes;
- static RAM reported by the linker: 53,120 bytes;
- commit and OTA follow this checkpoint.

Completion record:

- commit: `e1d0de4` (`fix(audio): preserve Audio Mix transition buffer`);
- firmware SHA-256:
  `446dcd76d0d8ebcbd2f64db926ba67b3bf4b731ce724e3e44a963f280d7e434c`;
- OTA target identity verified as `A4:CB:8F:F8:2F:14`;
- OTA endpoint reported successful firmware installation and reboot;
- post-reboot system API returned successfully;
- post-reboot free heap reported approximately 7.0 MiB before playback.

Goals:

- fix Audio Mix cuts;
- keep multiple overlapping ranges;
- deduplicate exact repeats;
- filter without bulk-flushing valid future audio;
- preserve seek behaviour outside deferred transitions.

### 7.2 Add-on B: fast buffered TCP stall recovery

Completion record:

- commit: `f6327cd` (`fix(audio): recover buffered TCP stalls sooner`);
- firmware SHA-256:
  `be7122410ad7665c532eb5062bc7df43014000c6fc5bda08196581e88412b061`;
- OTA target identity verified as `A4:CB:8F:F8:2F:14`;
- OTA endpoint accepted the image and rebooted the device;
- the system API returned after reboot at `192.168.68.105`.

Delivered behaviour:

- reduce active-play receive timeout from 30 seconds to 8 seconds;
- continue waiting indefinitely while intentionally paused;
- if the sender is silent while playback is active, close only the buffered
  audio socket;
- keep the listening task alive so the sender can reconnect;
- log the measured stall duration and recovery count;
- avoid rebooting the device for a recoverable socket problem.

Implementation details:

- the socket receive timeout is set independently on every newly accepted
  buffered audio connection;
- failure to set `SO_RCVTIMEO` is logged instead of silently ignored;
- each blocking `recv` measures its own wait duration;
- a timeout while paused remains non-fatal and the reader keeps waiting;
- a timeout while playing increments `buffered_stall_recoveries` and returns
  to the existing close-and-accept loop;
- every successful complete packet updates `buffered_last_packet_us`;
- every accepted buffered connection increments
  `buffered_connections_accepted`;
- these liveness values feed the later telemetry and recovery-ladder add-ons.

Build checkpoint:

- `pio run -e esp32s3` passed;
- firmware size: 1,445,787 bytes;
- static RAM reported by the linker: 53,136 bytes;
- no new allocation is performed per packet;
- the normal packet path adds only one timestamp write.

Why 8 seconds:

- the A1S archive tested 30 seconds and found it too slow to recover;
- 3 seconds produced false positives around legitimate track transitions;
- 8 seconds survived normal transitions while recovering before a long silent
  failure became permanent.

### 7.3 Add-on C: general health telemetry

Implementation status:

- source implementation complete;
- `pio run -e esp32s3` passed;
- firmware size: 1,447,223 bytes;
- static RAM reported by the linker: 53,136 bytes;
- the telemetry task stack is 4,096 bytes;
- commit and OTA follow this checkpoint.

Delivered behaviour:

- retain useful seek timing data without logging stale generations forever;
- make counter deltas safe across stream counter resets;
- report packet receive/decode/drop deltas;
- report late and underrun deltas;
- report decoded-buffer depth;
- report PTP lock status;
- report internal heap free, largest internal block and PSRAM free;
- report deferred flush request/drop/expiry/overflow counters;
- expose recovery-ladder counters;
- reduce repetitive healthy logging so the log stream itself does not become
  a scheduling load.

Sampling and output policy:

- the task samples the health snapshot once per second;
- a full `HEALTH` record is emitted only once every ten seconds;
- full health records are suppressed while no audio stream is running;
- seek transitions remain event driven and produce only `SEEK begin` and
  `SEEK ready` records;
- this replaces the old one-line-per-second `S3TRACE` stream, reducing log
  traffic by roughly ten times during ordinary playback;
- failure to create the telemetry task is now reported explicitly rather than
  returning an unexplained generic error.

Counter correctness:

- receive, decode, drop, late, underrun and decrypt-error counters are compared
  with their previous snapshot;
- if any source counter moves backwards, the stream statistics were reset and
  every delta for that report is forced to zero;
- this removes the misleading `429496...` values previously produced by
  unsigned subtraction after a stream replacement;
- the log includes `reset=1` on that report so the discontinuity remains
  visible without being mistaken for billions of dropped packets.

Memory observations:

- internal 8-bit-capable free heap is measured separately from PSRAM;
- the largest contiguous internal block is reported because task creation can
  fail from fragmentation even when total free memory still looks healthy;
- free PSRAM is reported independently;
- a report is marked `WARN` below 64 KiB internal free heap or below a 24 KiB
  largest internal block;
- these are warning thresholds only; telemetry never modifies playback or
  restarts a service.

Task observations:

- remaining stack high-water marks are reported for the PCM output task,
  buffered TCP reader, AAC decoder and telemetry task;
- zero means that task is not presently running and is not interpreted as a
  low-stack event by telemetry;
- a live task below 256 FreeRTOS stack words marks the report `WARN`;
- the accessors are read-only and do not suspend the observed tasks.

Network and protocol observations:

- the line reports whether a buffered TCP client is connected;
- packet age is calculated from the last fully received packet, not from a
  partial socket read;
- cumulative connection and active-play stall-recovery counts are shown;
- deferred FLUSHBUFFERED telemetry exposes active request slots, total armed
  requests, exact duplicates, selectively dropped packets, expired requests
  and slot-overflow events;
- reading deferred-flush statistics takes the same short mutex used by updates,
  expires stale slots first, copies scalar counters and releases immediately;
- packet-range matching and all normal audio behaviour are unchanged.

Timing observations:

- current PTP lock state and filtered clock offset in microseconds are shown;
- decoded-buffer current and target depths are shown together;
- seek readiness retains anchor, first-received and first-queued timing relative
  to the exact seek generation;
- a stale seek generation is no longer printed indefinitely after playback has
  stopped.

Warning semantics:

- `WARN` is emitted for memory or stack pressure, an underrun delta, a decrypt
  error delta or a saturated deferred-flush table;
- packet drops and late frames remain visible but do not alone label the system
  unhealthy because bounded drops are expected during legitimate seek timing
  correction;
- PTP acquisition does not trigger a warning or recovery action;
- this add-on is intentionally diagnostic only and cannot close sockets, stop
  services or reboot the ESP.

### 7.4 Add-on D: recovery ladder

Implementation status:

- source implementation complete in `main/audio/audio_recovery.c`;
- `pio run -e esp32s3` passed;
- firmware size: 1,439,575 bytes;
- static RAM reported by the linker: 53,168 bytes;
- the supervisor task stack is 4,096 bytes at priority 2.

Completion record:

- commit: `bbc4a56` (`feat(recovery): add conservative audio recovery ladder`);
- firmware SHA-256:
  `48efb1a5dddfdc0c25b4317bb69760c2dd79743254bf5761df41bb16eb4f6a09`;
- OTA target identity verified as `A4:CB:8F:F8:2F:14` before upload;
- OTA endpoint reported successful firmware installation and reboot;
- post-reboot system API returned with the same IP, MAC and approximately
  7.0 MiB free heap;
- the boot backlog read over `/ws/logs` shows, in order,
  `audio_health: Health telemetry started`,
  `audio_recover: Recovery ladder started: stuck>30s, 3 stream
  restarts/service, 3 service restarts/reboot` and `main: AirPlay ready`,
  confirming the new supervisor is running on the device.

Escalation order as delivered:

1. rung 1 — recover the local buffered audio socket when it stalls. This is
   the existing 8-second receive timeout from add-on B, inside
   `audio_stream_buffered.c`. The supervisor does not drive it; it only reads
   its counters.
2. rung 2 — restart the active buffered stream via the new
   `audio_receiver_restart_buffered_stream()`, reusing the TCP port Apple
   already learned from SETUP so the sender can reconnect without a new
   handshake.
3. rung 3 — after three rung-2 restarts without recovery, restart the RTSP
   service (`rtsp_server_stop`, `audio_receiver_stop`, `rtsp_server_start`).
   mDNS advertising and the web server are left untouched.
4. rung 4 — after three rung-3 restarts without recovery, reboot the ESP.

Trigger conditions (sampled once per second from the same read-only snapshot
the telemetry task uses):

- task loss: the buffered stream reports running but the TCP reader or AAC
  decoder task handle is gone, for five consecutive samples. The
  five-sample filter absorbs the sub-second window during task creation.
- stuck stream: a buffered TCP client is connected, playback is nominally
  active, and no complete packet has been read for more than 30 seconds, for
  five consecutive samples. This covers the wedge rung 1 cannot fix: the
  8-second socket timeout only fires while the reader blocks in `recv`, not
  while it waits for a packet slot behind a wedged decoder or a consumer
  that stopped draining.

Deliberate non-triggers:

- late frames, packet drops, underruns and decrypt errors never trigger
  recovery; they remain telemetry-only signals;
- seeks and PTP acquisition are never consulted;
- a paused stream is always left alone (`playing` is required);
- a sender that simply vanished does not climb the ladder: rung 1 already
  closed its socket, the idle listener accepts reconnects, and the stuck
  condition requires a connected client;
- the five-sample stuck filter also absorbs RESUME after a long pause, where
  the packet age is momentarily large while `playing` just turned true;
- on BT builds, a trigger while an A2DP client is connected is ignored
  because the AirPlay teardown is intentional (not compiled on this S3).

Cooldowns and reset:

- 60 seconds of grace after a stream restart and 120 seconds after a service
  restart before the ladder re-evaluates;
- 120 consecutive seconds of a running stream with advancing receive
  counters clears all escalation state;
- an idle listener takes no action but does not count as proof of recovery;
- rung 4 additionally requires at least 15 minutes of uptime; every
  escalation path already needs more than ten minutes of repeated failures,
  so a reboot loop is structurally impossible;
- before rebooting, the supervisor logs all ladder counters and waits 250 ms
  so the WebSocket log stream can flush.

Concurrency:

- stream start/stop/switch operations in `audio_receiver.c`
  (`start`, `start_buffered`, `start_stream`, `stop`, `stop_buffered_only`,
  `set_stream_type` and the new restart) are now serialized by a recursive
  FreeRTOS mutex. RTSP client tasks and the supervisor could otherwise race
  a recovery restart against SETUP/TEARDOWN. The mutex is recursive because
  these entry points call each other; it is held at most for the bounded
  task-shutdown wait (about one second).

Telemetry integration:

- the `HEALTH` line now ends with `recover(lvl/stream/svc)=` showing the
  highest rung reached since the last full-health reset plus cumulative
  stream and service restart counts;
- separate task-loss and stuck-stream event counters are kept for the
  reboot log line;
- the boot log announces the ladder thresholds with
  `audio_recover: Recovery ladder started`.

## 8. Existing protections already present on S3

The generated S3 configuration already enables:

- brownout detection;
- panic print followed by reboot;
- FreeRTOS stack-overflow canaries;
- the ESP task watchdog with a five-second timeout;
- watchdog monitoring for both CPU idle tasks.

The task watchdog is not configured to panic automatically. The recovery
ladder is preferred for known audio failures because it can restart a smaller
component before considering a full-device reboot.

The OTA writer already uses small chunks to avoid starving the watchdog while
writing flash.

## 9. Ideas reviewed but not copied from A1S

### 9.1 Static RTSP task control blocks

Rejected for direct porting.

Upstream v0.1.29 deliberately uses dynamically allocated restartable RTSP task
control blocks because reusing static FreeRTOS task memory before idle-task
deletion completes caused a known crash. The A1S static-task pattern includes
wait loops, but copying it verbatim would weaken the newer lifecycle fix.

### 9.2 Larger TCP receive window

Not enabled without evidence.

A larger window can improve burst throughput, but it also permits more stale
audio to accumulate before a seek. The current S3 has a dedicated compressed
queue, stable throughput and no normal-play packet loss in the supplied log.

### 9.3 Deeper I2S DMA

Not enabled without evidence.

The A1S needed deeper DMA because WiFi/BT contention produced measured 50–90 ms
scheduling gaps. The S3 log shows zero steady-state underruns. Increasing DMA
would change hardware latency and could degrade seek precision.

### 9.4 A1S servo and PTP changes

Not ported wholesale.

The S3 is using the newer v0.1.29 timing stack plus local seek fixes. Old servo
and anchor experiments were tuned for ESP32-A1S resource pressure and included
several reverted approaches. Replacing the stable S3 timing system would be a
large regression risk.

### 9.5 ES8388 DAC mutex

Not needed for the present PCM5102A.

The A1S mutex prevents concurrent I2C register writes to ES8388. PCM5102A has
no firmware volume-register traffic; volume and tone are performed in PCM.
The mutex becomes relevant only if a future board adds an I2C-controlled codec
or amplifier.

### 9.6 Bluetooth lifecycle hardening

Not applicable.

ESP32-S3 does not provide the Classic Bluetooth A2DP path used by the original
ESP32-A1S build. Keeping Bluedroid alive, AVRCP deduplication and BT/AirPlay
handoff guards would add dead code.

## 10. Build and OTA runbook

### 10.1 Build

From the active worktree:

```sh
cd /Users/edward/airplay-esp32/.claude/worktrees/s3-seek-telemetry-v29
pio run -e esp32s3
```

Expected image:

```text
.pio/build/esp32s3/firmware.bin
```

### 10.2 Verify target before OTA

```sh
curl --fail --silent --show-error \
  http://192.168.68.105/api/system/info
```

Do not upload unless the returned MAC is `A4:CB:8F:F8:2F:14`.

### 10.3 OTA upload

```sh
curl --fail --silent --show-error --max-time 45 \
  -H 'Content-Type: application/octet-stream' \
  --data-binary @.pio/build/esp32s3/firmware.bin \
  http://192.168.68.105/api/ota/update
```

### 10.4 Post-OTA checks

1. Wait for `/api/system/info` to respond again.
2. Verify IP, MAC, WiFi and free heap.
3. Open `/logs` or connect to `/ws/logs`.
4. Confirm the expected new subsystem log line.
5. Run at least one normal play, seek and automatic track transition.

### 10.5 Audio-profile migration after upstream update

Edit:

```text
audio_profiles/pcm5102-a1s.json
```

Regenerate:

```sh
python3 scripts/apply_audio_profile.py audio_profiles/pcm5102-a1s.json
```

Keep the profile JSON, generator and `audio_output_profile` module when
rebasing onto a newer firmware tag.

## 11. Validation matrix

Every future release should cover:

1. Cold boot and WiFi association.
2. Initial Apple Music connection.
3. At least five forward and backward seeks.
4. Pause longer than 30 seconds, then resume.
5. Manual next-track and previous-track.
6. At least two automatic track transitions with Audio Mix enabled.
7. Audio Mix burst with multiple overlapping FLUSHBUFFERED requests.
8. Browser log page connect and disconnect during playback.
9. WiFi interruption shorter than 8 seconds.
10. WiFi/TCP interruption longer than 8 seconds, confirming socket recovery.
11. Repeated AirPlay client reconnects.
12. Thirty-minute continuous playback while watching heap and buffer depth.
13. OTA followed by immediate playback.
14. Maximum and midpoint volume comparison against the A1S profile.

## 12. Future improvements that remain safe candidates

### 12.1 Persist last recovery reason

Store a compact reason code and count in RTC memory or NVS before a deliberate
restart. Expose it through `/api/system/info`. This distinguishes power loss,
brownout, panic, watchdog, stream restart and supervisor restart.

### 12.2 Task stack high-water telemetry

Record the minimum remaining stack for:

- RTSP server and client;
- buffered reader;
- AAC decoder;
- audio playback;
- telemetry;
- network monitor.

Only warn below a conservative threshold. Do not poll every task at audio rate.

### 12.3 Heap low-water telemetry

Track both total internal heap and largest internal block. Total free memory can
look healthy while fragmentation prevents an 8 KiB RTSP task stack allocation.

### 12.4 Allocation and task-creation fault injection

Add a development-only mode that fails selected allocations and verifies that
the stream closes cleanly rather than leaving a running flag with no task.

### 12.5 Long-duration soak automation

Use the WebSocket log endpoint to capture 8–24 hours of:

- heap minima;
- PTP lock changes;
- socket recoveries;
- stream restarts;
- underruns;
- deferred flush statistics.

The script should mark transitions and produce a small summary instead of
storing every healthy one-second line indefinitely.

### 12.6 Firmware build identity

Expose Git commit, build date and selected audio profile in the system API. The
numeric upstream version alone cannot distinguish multiple local OTA builds
that all identify themselves as `0.1.29`.

## 13. Current conclusion

The supplied full log does not show a weak ESP32-S3, an overloaded AAC decoder
or unstable WiFi. It shows a stable pipeline and one reproducible protocol
handling error: deferred FLUSHBUFFERED was implemented as a destructive global
PCM flush instead of selective packet-range suppression.

The immediate priority is therefore protocol correctness, followed by bounded
self-recovery and trustworthy health telemetry. Broad timing, DMA and network
tuning is intentionally avoided while the normal path is stable.
