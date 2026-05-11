# AirPlay Seek Synchronization Fix (v3)

## Root Cause Analysis
The issue with audio delaying and massive desynchronization after starting a track from 0 (or skipping) was traced to two compounding factors in the AirPlay 2 pipeline:

### 1. Flawed Pre-Decode RTP Gates
The previous architecture used `discard_before_rtp` and `discard_above_rtp` gates before the decoder to throw away stale packets. However, for track skips, AirPlay 2 establishes a new random RTP timestamp baseline. The 32-bit wrap-around math caused the new valid track's packets to incorrectly evaluate as "stale", leading to them being dropped *before* decoding. This caused the buffer to starve completely, creating 15 seconds of silence and destroying the pre-buffer burst entirely.

### 2. TCP Buffer Drain Delay
For AirPlay 2 buffered streams (which use TCP), the OS TCP socket buffer can hold several megabytes (~15 seconds) of ALAC/AAC audio data from the old track. When a new track starts, the ESP32 takes a few seconds to drain and discard this old data. By the time the *new* track's data is finally read from the socket, decoded, and enters our ring buffer, it is already "late" relative to the AirPlay timeline.

The `post_flush` late catch-up mechanism in `audio_timing_read` correctly identified that these frames were late. However, because it ran inside the I2S DMA callback and only dropped frames one-by-one with a `continue` (which was rate-limited to 8 frames per 10ms callback), dropping 5 seconds of stale TCP data took 15 seconds of real-time stuttering silence. 

The previous bulk-flush fix attempted to solve this by instantly deleting the ring buffer. However, this was too aggressive: it deleted the *entire* ring buffer, throwing away the valid on-time burst packets that sat right behind the stale packets. This resulted in the 2-3 second lyric skip.

## What Was Modified
**Files:** `main/audio/audio_stream.c`, `main/audio/audio_receiver.c`, `main/audio/audio_timing.c`

## How It Was Modified
1. **Removed RTP Gates:** The `discard_before_rtp` and `discard_above_rtp` logic was completely removed. This prevents the new track's burst from being falsely identified as stale before decoding.
2. **Fast-Forward DMA:** Instead of bulk-flushing the entire ring buffer (which destroyed valid packets), the DMA callback's loop limit in `audio_timing_read` was increased from `8` to `500`. The catch-up logic was reverted to use `continue`. 
3. **Buffer Refill Pause:** When `playout_started` is reset to false due to fast-forwarding, the system enforces a strict 200ms wait period using `ready_time_us` before it begins pulling frames again.

## Why This Modification Works
This perfectly harmonizes the fast decoding speed of the ESP32 with the precision of the ring buffer. When a late frame from the old track's TCP backlog is detected, the DMA callback now instantly "fast-forwards" through up to 500 late frames in a single call (taking only microseconds). It drops *only* the truly late frames and stops exactly when it reaches the new track's on-time burst packets. 

The 200ms `ready_time_us` pause acts as a clean lock, blocking the I2S DMA pipeline momentarily so the audio doesn't stutter while this lightning-fast catch-up occurs.

## Expected Effect
- **Start from 0 / Track skip:** The 15-second gap of silence is eliminated.
- **No Lost Lyrics:** The song no longer skips the first few lyrics because the valid burst is preserved.
- **Clean Transitions:** Track skips feature a clean, brief (~200ms) pause while the device instantly fast-forwards through network latency and syncs perfectly.