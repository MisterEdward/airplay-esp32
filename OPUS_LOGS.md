# Logs for Opus

These are curated snippets from the user tests.
They show the difference between the bad macOS system tray path and the good Safari/player/phone path.

## 1. macOS System Audio Tray: Bad Path

Behavior:

* selected from macOS system audio tray,
* audible jitter / drift / sample skip feeling,
* often bad on YouTube / HBO / system audio.

Important lines:

```text
I (...) rtsp_handlers: /info: audioFormats=[type96], latency96=0us, latency103=246000us
I (...) rtsp_handlers: RECORD received - starting playback, stream_paused was 0
I (...) rtsp_handlers: SETPEERS: got bplist
I (...) rtsp_handlers: SETPEERS: PTP peers changed, clock will re-lock
I (...) rtsp_handlers: SETUP: has_streams=1, stream_count=1
I (...) rtsp_handlers: Configured codec: ALAC (ct=2, sr=44100, spf=352)
I (...) rtsp_handlers: SETUP stream[0]: type=96 ct=2 sr=44100 spf=352 ekey=0 eiv=0 shk=32
I (...) audio_recv: Audio format: codec=ALAC sr=44100 ch=2 bits=16 frame=352 maxspf=352
I (...) rtsp_handlers: SETUP response: type=96 dataPort=64242 controlPort=64243
I (...) audio_recv: NACK retransmission enabled, client control port 53084
I (...) rtsp_handlers: FLUSH received
I (...) audio_recv: RTP gates armed on anchor: discard_before=1845050216 discard_above=1845491216
I (...) audio_time: post_flush done: early=571 ms, elapsed=509 ms
I (...) audio_time: Frame too early: early=573 ms consecutive=1 buffered=135 pending=0
I (...) audio_time: Frame too early: early=46 ms consecutive=1 buffered=215 pending=0
I (...) audio_time: Frame too early: early=77 ms consecutive=1 buffered=200 pending=0
I (...) audio_time: Frame too early: early=120 ms consecutive=1 buffered=202 pending=0
```

Interpretation:

* macOS tray chooses realtime UDP `type=96`.
* It is ALAC, 44.1 kHz, 352 samples/frame.
* Repeated early-frame logs show timing/anchor/buffer tension.

## 2. Safari / YouTube Player AirPlay Picker: Good Path

Behavior:

* selected from the YouTube/Safari player AirPlay control,
* user says this sounds clean,
* video appears to pause briefly and then resumes synced.

Important lines:

```text
I (...) rtsp_handlers: /info: audioFormats=[type96], latency96=0us, latency103=246000us
I (...) rtsp_handlers: RECORD received - starting playback, stream_paused was 0
I (...) rtsp_handlers: SETPEERS: got bplist
I (...) rtsp_handlers: SETPEERS: PTP peers changed, clock will re-lock
I (...) rtsp_handlers: SETUP: has_streams=1, stream_count=1
I (...) rtsp_handlers: Configured codec: AAC (ct=4, sr=44100, spf=1024)
I (...) rtsp_handlers: SETUP stream[0]: type=103 ct=4 sr=44100 spf=1024 ekey=0 eiv=0 shk=32
I (...) audio_recv: Audio format: codec=AAC sr=44100 ch=2 bits=16 frame=1024 maxspf=1024
I (...) rtsp_handlers: SETUP response: type=103 dataPort=49399 controlPort=64246
I (...) rtsp_handlers: SETRATEANCHORTIME: secs=13802, rtp=4059316363, rate=1.000000
I (...) rtsp_handlers: SETRATEANCHORTIME: rate=1.000000 -> RESUMING (was_paused=0)
I (...) audio_time: Frame too early: early=246 ms consecutive=1 buffered=44 pending=0
I (...) audio_time: Frame too early: early=183 ms consecutive=12 buffered=56 pending=1
```

Interpretation:

* Safari/player chooses buffered `type=103`.
* It is AAC, 44.1 kHz, 1024 samples/frame.
* This path generally sounds good despite some early-frame logs.

## 3. iPhone / Phone Path: Generally Good

Behavior:

* phone playback is basically perfect,
* examples include Apple Music and video/media.

Important lines:

```text
I (...) rtsp_server: Client IP: 192.168.1.5
I (...) rtsp_handlers: /info: audioFormats=[type96], latency96=0us, latency103=246000us
I (...) rtsp_handlers: SETUP: has_streams=1, stream_count=1
I (...) rtsp_handlers: Configured codec: AAC (ct=4, sr=44100, spf=1024)
I (...) rtsp_handlers: SETUP stream[0]: type=103 ct=4 sr=44100 spf=1024 ekey=0 eiv=0 shk=32
I (...) audio_recv: Audio format: codec=AAC sr=44100 ch=2 bits=16 frame=1024 maxspf=1024
I (...) rtsp_handlers: SETUP response: type=103 dataPort=55990 controlPort=64895
I (...) rtsp_handlers: SETRATEANCHORTIME: secs=493668, rtp=3235771530, rate=1.000000
I (...) rtsp_handlers: Received DMAP metadata
I (...) rtsp_handlers:   Album  = MAYHEM
I (...) rtsp_handlers:   Artist = Lady Gaga
I (...) rtsp_handlers:   Title  = Vanish Into You
```

Interpretation:

* Phone also uses buffered `type=103` in this captured example.
* This supports the theory that ES8388/I2S/basic decoder output can be clean.

## 4. Failed Experiment: One-Sample Slip Caused Static

Behavior:

* after a patch that dropped/duplicated one sample based on realtime buffer depth,
* user reported only static,
* patch was reverted.

Important lines:

```text
I (...) rtsp_handlers: Configured codec: ALAC (ct=2, sr=44100, spf=352)
I (...) rtsp_handlers: SETUP stream[0]: type=96 ct=2 sr=44100 spf=352 ekey=0 eiv=0 shk=32
I (...) audio_recv: Audio format: codec=ALAC sr=44100 ch=2 bits=16 frame=352 maxspf=352
I (...) audio_recv: RTP gates armed on anchor: discard_before=371312769 discard_above=371753769
I (...) audio_output: Realtime buffer high: frames=178, dropping 1 sample
I (...) audio_output: Realtime buffer high: frames=174, dropping 1 sample
I (...) audio_output: Realtime buffer high: frames=163, dropping 1 sample
I (...) audio_output: Realtime buffer high: frames=156, dropping 1 sample
I (...) audio_output: Realtime buffer high: frames=157, dropping 1 sample
I (...) audio_output: Realtime buffer high: frames=159, dropping 1 sample
I (...) audio_output: Realtime buffer high: frames=155, dropping 1 sample
```

User report after this:

```text
Se aude doar static, am deconectat si reconectat wifi doar
```

Interpretation:

* Do not use this approach again.
* Crude sample dropping in the output callback is unsafe here.

## 5. Failed Experiment: Advertising Buffered Type 103

Behavior:

* `/info` advertised both `type96` and `type103`,
* RAOP TXT temporarily advertised `UDP,TCP`,
* macOS tray still chose `type=96`.

Important lines:

```text
I (...) rtsp_handlers: /info: audioFormats=[type96,type103], latency96=0us, latency103=246000us
I (...) rtsp_handlers: Configured codec: ALAC (ct=2, sr=44100, spf=352)
I (...) rtsp_handlers: SETUP stream[0]: type=96 ct=2 sr=44100 spf=352 ekey=0 eiv=0 shk=32
I (...) audio_recv: Audio format: codec=ALAC sr=44100 ch=2 bits=16 frame=352 maxspf=352
```

Interpretation:

* Advertising `type=103` did not convince macOS system tray to use buffered TCP.
* This was reverted.

## 6. AAC Decode Error Seen In Some Buffered Tests

This appeared occasionally:

```text
E (...) ESP_AAC_DEC: Failed to decode aac frame, error:20.
W (...) audio_dec: AAC decode error -1 — resetting decoder
W (...) audio_dec: AAC decoder reset OK
I (...) rtsp_handlers: FLUSHBUFFERED immediate (missing from/until fields)
```

Interpretation:

* Worth keeping in mind.
* But the main user complaint is still the macOS tray `type=96` realtime path.

## 7. Noisy / Unsafe Debug Logs

Logs also contain key material dumps:

```text
W (...) srp: DBG S_len=384
W (...) DBG S head: ...
W (...) DBG K: ...
W (...) DBG srp_K: ...
W (...) DBG enc(Read): ...
W (...) DBG dec(Write): ...
```

Interpretation:

* These should be removed or gated.
* They leak secrets and make audio logs harder to read.
