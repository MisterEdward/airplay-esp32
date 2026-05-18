#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio_timing.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "ntp_clock.h"
#include "ptp_clock.h"

#define DEFAULT_BUFFER_LATENCY_US     200000 // 200ms startup jitter buffer
// I2S DMA latency compensation: frames are scheduled this many µs earlier so
// they hit the speaker at the correct wall-clock moment, not when they enter
// the DMA register.  Must equal dma_desc_num × dma_frame_num / OUTPUT_RATE:
//   Round 1: 8  × 256 / 44100 ≈ 46 ms (audio_output.c dma_desc_num=8)
//   Round 2: 16 × 256 / 44100 ≈ 93 ms (audio_output.c dma_desc_num=16)
// If you change dma_desc_num in audio_output.c, update this in lockstep or
// post-seek lipsync drifts by exactly the delta.
#define HARDWARE_OUTPUT_LATENCY_US    93000
#define MIN_STARTUP_FRAMES            4
#define DRIFT_ADJUST_THRESHOLD_FRAMES 2
#define TIMING_THRESHOLD_US           40000 // 40ms early threshold (legacy)
// Early threshold: how far in the future a frame can be before we hold it
// pending and emit silence. Kept tight (40ms) because relaxing it would let
// frames play ahead of their anchor, introducing audible A/V drift.
#define TIMING_THRESHOLD_EARLY_US 40000
// Late threshold: how far past its anchor a frame can be and still play.
// Wider than the early threshold because WiFi-induced i2s_channel_write stalls
// (observed 30-50ms in telemetry on the realtime UDP path) shift the read
// point past the anchor temporarily — dropping those frames produces audible
// silence.  But the threshold can't be too wide either: once drift accumulates
// it persists indefinitely (the I2S clock is the master, we can't speed up
// playback), creating a perceptible A/V delay on video sources.  60ms absorbs
// the typical stall while keeping steady-state drift bounded enough to stay
// imperceptible against video.
#define TIMING_THRESHOLD_LATE_US  60000
// Wider late threshold for the realtime UDP path (type=96, ~200 ms jitter
// buffer).  NACK retransmits on WiFi routinely return frames 60-100 ms late,
// and with a 200 ms jitter budget those frames are still within the sender's
// intended playback window.  Dropping them produces audible stutter while
// playing them slightly behind the anchor is inaudible on the realtime path
// where the sender does not enforce A/V lipsync at sample accuracy.
// The buffered AAC/TCP path keeps the tight 60 ms gate because TCP retransmits
// arrive in sequence with predictable timing and a ~600 ms+ jitter budget.
//
// Round 1: 150 ms.  Telemetry showed drift spikes to -180/-200 ms when an
// i2s_channel_write blocked 90 ms on top of a steady -100 ms baseline.
// Round 2: raised to 200 ms.  With the DMA depth doubled to 16×256 (Task 1),
// worst-case scheduling jitter should halve to ~45 ms, so 200 ms gives
// comfortable headroom above the expected -100 ms baseline + 45 ms spike.
#define TIMING_THRESHOLD_LATE_REALTIME_US 200000
// If a frame is late by more than this, flush the whole buffer at once
// instead of draining one frame per DMA callback (which would cause seconds
// of silence while thousands of stale frames are individually dropped).
// Kept independent of DEFAULT_BUFFER_LATENCY_US so reducing the startup
// buffer doesn't also reduce the late-detection threshold.
#define BULK_FLUSH_LATE_THRESHOLD_US 2000000 // 2 seconds
// MAX_CONSECUTIVE_EARLY: number of consecutive early frames before we conclude
// the anchor is genuinely stuck/wrong.  At ~8 ms per frame this is ~6 seconds
// of silence, which is well beyond any normal pre-buffer depth.
#define MAX_CONSECUTIVE_EARLY 750
// MAX_CONSECUTIVE_LATE: number of consecutive individually-late frames before
// we conclude the whole buffer is stale and do a bulk flush.  At ~8 ms/frame
// this is ~24 ms — just enough to distinguish a genuine stale-buffer from a
// one-off WiFi jitter spike, without the 20-frame drain+log storm.
#define MAX_CONSECUTIVE_LATE 3

// POST_FLUSH_STALE_THRESHOLD_US: in post_flush mode the bypass plays frames
// unconditionally to avoid silence during the phone's pre-buffer window
// (typically 2–4 s).  Frames that are MORE than this many µs early are from
// the wrong seek position (old audio still draining through the TCP pipeline)
// and must be discarded rather than played.  10 s is well above the deepest
// observed AirPlay 2 pre-buffer depth and well below any real seek delta.
#define POST_FLUSH_STALE_THRESHOLD_US 10000000LL // 10 seconds
// POST_FLUSH_TIMEOUT_US: safety backstop for the post_flush bypass.  The
// primary exit is the on-time convergence check below; this timeout prevents
// infinite bypass if the anchor never settles.  The steady-state servo still
// converges post-seek drift slowly, so 5 minutes gives comfortable headroom.
#define POST_FLUSH_TIMEOUT_US 300000000LL // 5 min
// Number of consecutive on-time frames (within the normal TIMING_THRESHOLD
// window) before exiting post_flush.  Prevents exiting on a single lucky
// frame amidst jitter.
#define POST_FLUSH_ONTIME_EXIT_COUNT 10
// During seek/track-skip recovery, drop frames that are more than this far
// behind the sender timeline.  Must match TIMING_THRESHOLD_LATE_US so the
// 500-attempt fast-forward loop drops ALL late frames (the "past portion" of
// the phone's pre-buffer burst) and only plays once it reaches genuinely
// on-time data.  A wider threshold (e.g. 700 ms) causes permanent desync:
// the steady-state servo cannot recover hundreds of ms of offset quickly, so
// playback stays permanently behind the sender's timeline.
#define POST_FLUSH_LATE_CATCHUP_US 60000LL
// After post_flush exits, keep a short late-frame grace window.  The first
// second after a seek is buffer-starved; dropping 60–90 ms late frames there
// sounds worse than playing them and letting the servo pull back gently.
#define POST_FLUSH_LATE_GRACE_US   1500000LL
#define POST_FLUSH_GRACE_LATE_US   100000LL
// COMPUTE_EARLY_SANITY_LIMIT_US: if computed early_us exceeds this magnitude
// (5 minutes), the math is wrong — typically caused by anchor_network_time_ns
// from a different PTP master combined with a stale ptp offset.  Treat the
// result as "not computable" so audio_timing_read falls through to the
// post_flush bypass / no-anchor path instead of triggering a bulk-flush loop.
// 5 minutes is far above any legitimate buffer depth (~21 s for buf103) and
// far below the magnitudes seen on a master mismatch (>500_000 s).
#define COMPUTE_EARLY_SANITY_LIMIT_US (300LL * 1000000LL)

static const char *TAG = "audio_time";
// consecutive_early_frames is now a field in audio_timing_t so it resets
// automatically whenever a new anchor is set.

static uint32_t frame_samples_from_format(const audio_format_t *format) {
  if (format->frame_size > 0) {
    return (uint32_t)format->frame_size;
  }
  if (format->max_samples_per_frame > 0) {
    return format->max_samples_per_frame;
  }
  return AAC_FRAMES_PER_PACKET;
}

static void update_timing_targets(audio_timing_t *timing,
                                  const audio_format_t *format) {
  timing->nominal_frame_samples = frame_samples_from_format(format);

  if (format->sample_rate <= 0 || timing->nominal_frame_samples == 0) {
    timing->target_buffer_frames = MIN_STARTUP_FRAMES;
    return;
  }

  uint64_t latency_samples =
      ((uint64_t)timing->output_latency_us * (uint64_t)format->sample_rate) /
      1000000ULL;
  // The ring buffer stores frames as chunks of at most AAC_FRAMES_PER_PACKET
  // (352) samples per slot — see audio_buffer_queue_decoded, which splits a
  // 1024-sample AAC frame into three 352-sample chunks.  audio_buffer_get_frame_count
  // returns the number of slots, not the declared frame_size.  So when we
  // want "200 ms of buffered audio before playout starts", we must divide
  // latency_samples by the actual per-slot capacity, not by
  // nominal_frame_samples (which for AAC is 1024 and yields a target that is
  // ~3× too small — 72 ms instead of 200 ms, producing underruns every time
  // the servo takes a moment to converge after an anchor change).
  uint32_t slot_capacity = timing->nominal_frame_samples;
  if (slot_capacity > AAC_FRAMES_PER_PACKET) {
    slot_capacity = AAC_FRAMES_PER_PACKET;
  }
  uint32_t target_frames =
      (uint32_t)((latency_samples + slot_capacity - 1) / slot_capacity);
  if (target_frames < MIN_STARTUP_FRAMES) {
    target_frames = MIN_STARTUP_FRAMES;
  }
  timing->target_buffer_frames = target_frames;
}

typedef enum {
  SYNC_MODE_NONE, // No clock sync, use local anchor time
  SYNC_MODE_PTP,  // AirPlay 2 PTP sync
  SYNC_MODE_NTP,  // AirPlay 1 NTP sync
} sync_mode_t;

// Compute how early (positive) or late (negative) a frame is in microseconds
static bool compute_early_us(const audio_timing_t *timing,
                             const audio_format_t *format,
                             uint32_t rtp_timestamp, sync_mode_t sync_mode,
                             int64_t *early_us) {
  if (!timing->anchor_valid || format->sample_rate <= 0) {
    return false;
  }

  int32_t rtp_delta = (int32_t)(rtp_timestamp - timing->anchor_rtp_time);
  int64_t frame_offset_ns =
      ((int64_t)rtp_delta * 1000000000LL) / format->sample_rate;

  int64_t target_ns;
  switch (sync_mode) {
  case SYNC_MODE_PTP:
    // AirPlay 2: use network time with PTP offset for multi-room sync
    target_ns = (int64_t)timing->anchor_network_time_ns -
                ptp_clock_get_offset_ns() + frame_offset_ns;
    break;
  case SYNC_MODE_NTP:
    // AirPlay 1: use network time with NTP offset for multi-room sync
    // offset = remote_time - local_time, so local = remote - offset
    target_ns = (int64_t)timing->anchor_network_time_ns -
                ntp_clock_get_offset_ns() + frame_offset_ns;
    break;
  default:
    // Fallback: use local anchor time (no multi-room sync)
    target_ns = timing->anchor_local_time_ns + frame_offset_ns;
    break;
  }

  // Subtract hardware latency to account for I2S DMA delay
  target_ns -= (int64_t)HARDWARE_OUTPUT_LATENCY_US * 1000LL;

  int64_t now_ns = (int64_t)esp_timer_get_time() * 1000LL;
  *early_us = (target_ns - now_ns) / 1000LL;

  // Sanity: a result wildly outside any plausible buffer depth means the
  // anchor's clock universe doesn't match ours (typically: PTP master changed
  // but ptp_offset is still locked to the old master).  Refuse to answer so
  // audio_timing_read doesn't bulk-flush in a loop.
  if (*early_us > COMPUTE_EARLY_SANITY_LIMIT_US ||
      *early_us < -COMPUTE_EARLY_SANITY_LIMIT_US) {
    return false;
  }

  return true;
}

void audio_timing_init(audio_timing_t *timing, size_t pending_capacity) {
  if (!timing) {
    return;
  }

  memset(timing, 0, sizeof(*timing));
  timing->output_latency_us = DEFAULT_BUFFER_LATENCY_US;
  timing->playing = true;
  timing->drift_min_us = INT32_MAX;
  timing->drift_max_us = INT32_MIN;

  if (pending_capacity > 0) {
    timing->pending_frame = (uint8_t *)malloc(pending_capacity);
    if (timing->pending_frame) {
      timing->pending_frame_capacity = pending_capacity;
    }
  }
}

void audio_timing_reset(audio_timing_t *timing) {
  if (!timing) {
    return;
  }

  timing->playout_started = false;
  timing->anchor_valid = false;
  timing->pending_valid = false;
  timing->pending_frame_len = 0;
  timing->ready_time_us = 0;
  timing->consecutive_early_frames = 0;
  timing->consecutive_late_frames = 0;
  timing->post_flush = false;
  timing->post_flush_start_us = 0;
  timing->post_flush_ontime_count = 0;
  timing->post_flush_late_grace_until_us = 0;
  timing->deferred_flush_pending = false;
  timing->flush_until_ts = 0;
  timing->drift_min_us = INT32_MAX;
  timing->drift_max_us = INT32_MIN;
  timing->drift_samples = 0;
  timing->smoothed_drift_us = 0;
  timing->smoothed_drift_valid = false;
  timing->local_anchor_adjusted = false;
}

int32_t audio_timing_get_smoothed_drift_us(const audio_timing_t *timing) {
  if (!timing || !timing->smoothed_drift_valid) {
    return 0;
  }
  return timing->smoothed_drift_us;
}

void audio_timing_drain_drift(audio_timing_t *timing, int32_t *min_us,
                              int32_t *max_us, uint32_t *samples) {
  if (!timing) {
    if (min_us) *min_us = 0;
    if (max_us) *max_us = 0;
    if (samples) *samples = 0;
    return;
  }
  uint32_t s = timing->drift_samples;
  int32_t mn = (s > 0) ? timing->drift_min_us : 0;
  int32_t mx = (s > 0) ? timing->drift_max_us : 0;
  timing->drift_min_us = INT32_MAX;
  timing->drift_max_us = INT32_MIN;
  timing->drift_samples = 0;
  if (min_us) *min_us = mn;
  if (max_us) *max_us = mx;
  if (samples) *samples = s;
}

void audio_timing_set_format(audio_timing_t *timing,
                             const audio_format_t *format) {
  if (!timing || !format) {
    return;
  }

  update_timing_targets(timing, format);
}

void audio_timing_set_output_latency(audio_timing_t *timing,
                                     const audio_format_t *format,
                                     uint32_t latency_us) {
  if (!timing || !format) {
    return;
  }

  timing->output_latency_us = latency_us;
  update_timing_targets(timing, format);
}

uint32_t audio_timing_get_output_latency(const audio_timing_t *timing) {
  if (!timing) {
    return 0;
  }

  return timing->output_latency_us;
}

uint32_t audio_timing_get_hardware_latency(void) {
  return HARDWARE_OUTPUT_LATENCY_US;
}

void audio_timing_set_anchor(audio_timing_t *timing,
                             const audio_format_t *format, uint64_t clock_id,
                             uint64_t network_time_ns, uint32_t rtp_time) {
  if (!timing || !format) {
    return;
  }

  (void)clock_id;

  int64_t now_ns = (int64_t)esp_timer_get_time() * 1000LL;

  timing->anchor_rtp_time = rtp_time;
  timing->anchor_network_time_ns = network_time_ns;
  timing->anchor_local_time_ns = now_ns;
  timing->ptp_locked = ptp_clock_is_locked();
  timing->anchor_valid = true;
  // Reset frame counters so pre-buffered audio after a pause/resume or
  // track skip does not accumulate into the new anchor's counts.
  timing->consecutive_early_frames = 0;
  timing->consecutive_late_frames = 0;
  // Clear the per-anchor local-time adjustment flag so the pre-buffer
  // compensation (Task 1 / round-3) fires exactly once on the first huge-early
  // frame after this anchor, not repeatedly.
  timing->local_anchor_adjusted = false;
}

void audio_timing_set_playing(audio_timing_t *timing, bool playing) {
  if (!timing) {
    return;
  }

  ESP_LOGI(TAG, "set_playing: %s -> %s", timing->playing ? "playing" : "paused",
           playing ? "playing" : "paused");

  timing->playing = playing;
  if (!playing) {
    // Discard any partially-pending frame so resume starts cleanly from
    // the oldest frame in the sorted buffer.
    timing->pending_valid = false;
    timing->pending_frame_len = 0;
  }
}

size_t audio_timing_read(audio_timing_t *timing, audio_buffer_t *buffer,
                         const audio_stream_t *stream, audio_stats_t *stats,
                         int16_t *out, size_t samples) {
  if (!timing || !buffer || !stream || !out || samples == 0) {
    return 0;
  }

  if (!timing->playing) {
    return 0;
  }

  const audio_format_t *format = &stream->format;
  audio_buffer_sample_depth(buffer);
  int buffered_frames = audio_buffer_get_frame_count(buffer);

  // Wait for enough buffer before starting
  if (!timing->playout_started && !timing->pending_valid) {
    // After a seek/track-skip (post_flush), start playback as soon as we have
    // a few frames — the sender's pre-buffer burst will refill the ring
    // quickly, and waiting the full target_buffer_frames here just adds to the
    // user-perceived seek latency.  Normal startup still uses the full target
    // so a cold-start stream builds proper jitter margin before playing.
    int startup_target = timing->post_flush
                             ? MIN_STARTUP_FRAMES
                             : (int)timing->target_buffer_frames;
    if (buffered_frames < startup_target) {
      return 0;
    }
    
    // If we just bulk flushed due to late TCP packets, wait a bit before playing
    // to let the buffer refill and settle, giving a clean 200ms pause instead of stutter
    if (timing->ready_time_us != 0) {
      int64_t now_us = esp_timer_get_time();
      if (now_us - timing->ready_time_us < 200000) {
        return 0; // Wait 200ms
      }
    }

    // Wait for anchor before playing.
    // Normal startup: allow a 1-second fallback so a stream with no anchor
    // (e.g. AirPlay 1 without NTP) can still start.
    if (!timing->anchor_valid) {
      int64_t now_us = esp_timer_get_time();
      // Only set ready_time_us for startup if it wasn't already set by a bulk flush
      if (timing->ready_time_us == 0 || (now_us - timing->ready_time_us > 1000000)) {
        timing->ready_time_us = now_us;
      }
      if (now_us - timing->ready_time_us < 1000000) {
        return 0; // Still waiting for anchor
      }
      // Waited 1 second, no anchor - proceed without sync
    }
  }

  // Determine sync mode: PTP (AirPlay 2), NTP (AirPlay 1), or local fallback
  sync_mode_t sync_mode = SYNC_MODE_NONE;
  if (ptp_clock_is_locked()) {
    sync_mode = SYNC_MODE_PTP;
  } else if (ntp_clock_is_locked()) {
    sync_mode = SYNC_MODE_NTP;
  }

  for (int attempt = 0; attempt < 500; attempt++) {
    size_t item_size = 0;
    void *item = NULL;
    bool from_pending = false;

    // Get frame from pending or buffer
    if (timing->pending_valid) {
      item_size = timing->pending_frame_len;
      if (item_size < sizeof(audio_frame_header_t)) {
        timing->pending_valid = false;
        timing->pending_frame_len = 0;
        continue;
      }
      item = timing->pending_frame;
      from_pending = true;
    } else {
      if (!audio_buffer_take(buffer, &item, &item_size, 0)) {
        if (stats) {
          stats->buffer_underruns++;
        }
        return 0;
      }
      buffered_frames = audio_buffer_get_frame_count(buffer);

      if (item_size < sizeof(audio_frame_header_t)) {
        audio_buffer_return(buffer, item);
        continue;
      }
    }

    audio_frame_header_t *hdr = (audio_frame_header_t *)item;
    size_t frame_samples = hdr->samples_per_channel;
    size_t channels = hdr->channels ? hdr->channels : format->channels;
    int16_t *pcm = (int16_t *)(hdr + 1);

    // Validate frame
    if (frame_samples == 0 || channels == 0) {
      if (from_pending) {
        timing->pending_valid = false;
        timing->pending_frame_len = 0;
      } else {
        audio_buffer_return(buffer, item);
      }
      continue;
    }

    size_t expected_bytes =
        sizeof(*hdr) + frame_samples * channels * sizeof(int16_t);
    if (item_size < expected_bytes) {
      if (from_pending) {
        timing->pending_valid = false;
        timing->pending_frame_len = 0;
      } else {
        audio_buffer_return(buffer, item);
      }
      continue;
    }

    if (frame_samples > samples) {
      frame_samples = samples;
    }

    // Deferred flush check (AirPlay 2 FLUSHBUFFERED with flushFromSeq):
    // keep playing until the frame whose RTP timestamp reaches flush_until_ts,
    // then bulk-flush the remainder of the buffer and start fresh.
    // Signed 32-bit subtraction handles RTP wraparound correctly.
    if (timing->deferred_flush_pending) {
      if ((int32_t)(hdr->rtp_timestamp - timing->flush_until_ts) >= 0) {
        ESP_LOGI(TAG,
                 "Deferred flush triggered at ts=%" PRIu32 " (until_ts=%" PRIu32
                 ")",
                 hdr->rtp_timestamp, timing->flush_until_ts);
        if (from_pending) {
          timing->pending_valid = false;
          timing->pending_frame_len = 0;
        } else {
          audio_buffer_return(buffer, item);
        }
        audio_buffer_flush(buffer);
        timing->deferred_flush_pending = false;
        timing->anchor_valid = false;
        timing->playout_started = false;
        timing->ready_time_us = 0;
        timing->consecutive_early_frames = 0;
        timing->consecutive_late_frames = 0;
        timing->smoothed_drift_us = 0;
        timing->smoothed_drift_valid = false;
        // The old anchor can belong to the pre-flush track.  Invalidate it so
        // next-track frames do not look seconds early and poison telemetry.
        timing->post_flush = false;
        timing->post_flush_start_us = 0;
        timing->post_flush_ontime_count = 0;
        return 0;
      }
    }

    // Handle early/late frames based on anchor timing.
    //
    // post_flush bypasses ALL timing checks (early and late) and plays every
    // frame unconditionally.  This mirrors shairport-sync's
    // first_packet_timestamp==0 path: after a seek or flush, the phone's anchor
    // may be stale by hundreds of ms (startup buffer fill delay + pre-buffer
    // depth), so frames appear early or late through no fault of the stream.
    // Enforcing timing here causes silence or cascading re-flushes.
    // post_flush clears only when a frame is genuinely on-time, at which point
    // the anchor has settled and normal timing can re-engage.
    if (timing->anchor_valid && format->sample_rate > 0) {
      int64_t early_us = 0;
      if (compute_early_us(timing, format, hdr->rtp_timestamp, sync_mode,
                           &early_us)) {
        // Drift telemetry: track per-window min/max of early_us so the
        // telemetry task can show actual A/V skew.  Saturate to int32 range.
        int32_t early_clamped = (early_us > INT32_MAX) ? INT32_MAX
                              : (early_us < INT32_MIN) ? INT32_MIN
                                                       : (int32_t)early_us;
        if (early_clamped < timing->drift_min_us) {
          timing->drift_min_us = early_clamped;
        }
        if (early_clamped > timing->drift_max_us) {
          timing->drift_max_us = early_clamped;
        }
        timing->drift_samples++;
        // EMA for the rate servo's setpoint.  alpha = 1/128 -> ~1 s time
        // constant at the ~125 Hz frame cadence.  Skip the update for absurd
        // values that compute_early_us already filters; this is just a
        // belt-and-braces guard against poisoning the integrator if the
        // sanity gate is ever loosened.
        if (early_us > -1000000LL && early_us < 1000000LL) {
          if (!timing->smoothed_drift_valid) {
            timing->smoothed_drift_us = (int32_t)early_us;
            timing->smoothed_drift_valid = true;
          } else {
            int32_t diff = (int32_t)early_us - timing->smoothed_drift_us;
            timing->smoothed_drift_us += diff / 128;
          }
        }
        if (timing->post_flush) {
          // Bypass: play regardless of early/late — the phone pre-buffers
          // several seconds ahead of the anchor's current position after a
          // seek, so frames appear early through no fault of the stream.
          //
          // Track the start time so we can exit post_flush after a timeout
          // rather than requiring early to reach ±TIMING_THRESHOLD_US (which
          // may never happen if the pre-buffer depth exceeds the threshold).
          if (timing->post_flush_start_us == 0) {
            timing->post_flush_start_us = esp_timer_get_time();
          }
          int64_t flush_elapsed =
              esp_timer_get_time() - timing->post_flush_start_us;

          // --- Round-3 fix: pre-buffer compensation when PTP is not locked ---
          // When PTP is NOT locked the consumer falls back to anchor_local_time_ns
          // as the reference.  But anchor_local_time_ns is the CPU instant we
          // received SETRATEANCHORTIME — it does NOT include the sender's
          // pre-buffer (3-5 s of audio queued ahead of the anchor for multi-room
          // sync).  Forward-buffered frames appear "5 s in the future", the
          // consumer holds them pending, and the user hears 4-5 s of silence.
          //
          // Guard: only when (1) PTP is not locked — PTP-locked path already
          // uses anchor_network_time_ns - ptp_offset which is correct, so we
          // must NEVER shift anchor_local_time_ns there (would break multi-room
          // sync, see CLAUDE.md §12).  (2) inside post_flush — only fires after
          // a seek, not on regular packet flow.  (3) first frame only per anchor
          // (local_anchor_adjusted flag) — prevents cumulative drift.
          // (4) drift > +500 ms — far outside any legitimate jitter budget.
          //
          // Action: slide anchor_local_time_ns forward by (early_us - 200 ms)
          // so the oldest buffered frame's new computed target is ~now + 200 ms,
          // matching the normal jitter buffer depth.  The 200 ms head-start lets
          // DMA fill cleanly before the first frame is due.  anchor_network_time_ns
          // is left untouched — it is sacred for multi-room sync.
          if (!timing->local_anchor_adjusted &&
              !ptp_clock_is_locked() &&
              early_us > 500000LL) {
            int64_t adjust_ns = (early_us - 200000LL) * 1000LL;
            int64_t adjust_ms = (early_us - 200000LL) / 1000LL;
            timing->anchor_local_time_ns += adjust_ns;
            timing->local_anchor_adjusted = true;
            ESP_LOGI(TAG,
                     "Re-anchored local time by %lld ms "
                     "(PTP not locked, pre-buffer compensation)",
                     adjust_ms);
            // Recompute early_us with the adjusted anchor so the frame
            // evaluation below sees the corrected value immediately.
            compute_early_us(timing, format, hdr->rtp_timestamp, sync_mode,
                             &early_us);
          }
          // --- end pre-buffer compensation ---

          // Exception: frames that are MORE than POST_FLUSH_STALE_THRESHOLD_US
          // early are old-position data still draining from the TCP kernel
          // buffer (e.g. frames from 2:30 after a seek back to 0:00).  Discard
          // those so the user never hears audio from the wrong position.
          if (early_us > POST_FLUSH_STALE_THRESHOLD_US) {
            // This frame is from the wrong seek position — still draining the
            // TCP kernel buffer from before the flush.  Drop it and continue
            // to the next frame inside this same DMA callback.  The 500-attempt
            // loop drains up to ~11.6 s of late/stale audio in microseconds,
            // so we hit on-time frames in the same tick instead of emitting
            // silence and waiting 200 ms (which produces a stutter-then-silence
            // pattern as the decoder keeps queueing late frames behind us).
            if (timing->consecutive_late_frames == 0) {
              ESP_LOGW(TAG, "post_flush: dropping stale frames (%lld s early)",
                       early_us / 1000000LL);
            }
            timing->consecutive_late_frames++;
            if (from_pending) {
              timing->pending_valid = false;
              timing->pending_frame_len = 0;
            } else {
              audio_buffer_return(buffer, item);
            }
            continue;
          }
          // Late after a seek means we are about to play old-position audio
          // behind the sender's timeline.  Drop frames quickly until we catch
          // up; this gives a short clean mute instead of choppy desync.
          if (early_us < -POST_FLUSH_LATE_CATCHUP_US) {
            if (stats) {
              stats->late_frames++;
            }
            if (timing->consecutive_late_frames == 0) {
              ESP_LOGW(TAG, "post_flush catch-up: dropping late frames starting at %lld ms",
                       -early_us / 1000LL);
            }
            timing->consecutive_late_frames++;
            if (from_pending) {
              timing->pending_valid = false;
              timing->pending_frame_len = 0;
            } else {
              audio_buffer_return(buffer, item);
            }
            // Drop one frame and continue inside the 500-attempt loop.  This
            // drains the TCP backlog at decoder speed (microseconds per
            // frame) within a single DMA callback, instead of bulk-flushing
            // the ring buffer and waiting 200 ms (which lets the decoder
            // re-queue more late frames behind us, producing a stutter-then-
            // silence pattern for several seconds).
            continue;
          }

          // Within the acceptable post-seek window — play and check if we
          // should exit.  Exit only when frames are within normal thresholds.
          bool frame_ontime =
              (early_us >= -TIMING_THRESHOLD_LATE_US &&
               early_us <= TIMING_THRESHOLD_EARLY_US);
          if (frame_ontime) {
            timing->post_flush_ontime_count++;
          } else {
            timing->post_flush_ontime_count = 0;
          }
          if (timing->post_flush_ontime_count >= POST_FLUSH_ONTIME_EXIT_COUNT ||
              flush_elapsed >= POST_FLUSH_TIMEOUT_US) {
            int64_t now_us = esp_timer_get_time();
            ESP_LOGI(TAG,
                     "post_flush done: early=%lld ms, elapsed=%lld ms, "
                     "ontime=%d",
                     early_us / 1000LL, flush_elapsed / 1000LL,
                     timing->post_flush_ontime_count);
            timing->post_flush = false;
            timing->post_flush_start_us = 0;
            timing->post_flush_ontime_count = 0;
            timing->post_flush_late_grace_until_us =
                now_us + POST_FLUSH_LATE_GRACE_US;
          }
          timing->consecutive_early_frames = 0;
          timing->consecutive_late_frames = 0;
          // Fall through to play the frame.
        } else if (early_us > TIMING_THRESHOLD_EARLY_US) {
          timing->consecutive_early_frames++;

          // If we have had an implausibly long run of early frames the anchor
          // is probably stuck or wrong — give up on it so playback can
          // continue.  This threshold is high enough (~6 s) that it never
          // fires during normal pre-buffered-audio scenarios.
          if (timing->consecutive_early_frames > MAX_CONSECUTIVE_EARLY) {
            ESP_LOGW(TAG,
                     "Invalidating stuck anchor: consecutive=%d, early=%lld ms",
                     timing->consecutive_early_frames, early_us / 1000LL);
            timing->anchor_valid = false;
            timing->consecutive_early_frames = 0;
            // Fall through to play the frame normally
          } else {
            // Frame is early — store it as pending and output silence.
            // The pending frame is re-checked on every subsequent call;
            // once wall-clock catches up it will be played on time.
            // This is the normal path for pre-buffered audio after a pause.
            static int early_count = 0;
            early_count++;
            if (timing->consecutive_early_frames == 1 ||
                early_count % 100 == 1) {
              ESP_LOGI(TAG,
                       "Frame too early: early=%lld ms consecutive=%d "
                       "buffered=%d pending=%d",
                       early_us / 1000LL, timing->consecutive_early_frames,
                       buffered_frames, timing->pending_valid ? 1 : 0);
            }
            if (!from_pending && timing->pending_frame &&
                item_size <= timing->pending_frame_capacity) {
              memcpy(timing->pending_frame, item, item_size);
              timing->pending_frame_len = item_size;
              timing->pending_valid = true;
              audio_buffer_return(buffer, item);
            }
            memset(out, 0, samples * channels * sizeof(int16_t));
            return samples;
          }
        } else {
          // Realtime UDP streams (output_latency_us <= 300 ms) need a wider
          // late window because NACK retransmits arrive 60-100 ms late
          // after WiFi interference; dropping them produces audible stutter.
          // Buffered TCP streams keep the tight 60 ms gate.
          int64_t late_threshold_us =
              (timing->output_latency_us <= 300000)
                  ? TIMING_THRESHOLD_LATE_REALTIME_US
                  : TIMING_THRESHOLD_LATE_US;
          int64_t now_us = esp_timer_get_time();
          if (timing->post_flush_late_grace_until_us > now_us) {
            late_threshold_us = POST_FLUSH_GRACE_LATE_US;
          } else {
            timing->post_flush_late_grace_until_us = 0;
          }

          if (early_us < -late_threshold_us) {
            // Reset consecutive early counter on late/normal frames
            timing->consecutive_early_frames = 0;
            timing->consecutive_late_frames++;

            // Late frame — drop and continue inside the 500-attempt loop.
            // Earlier versions of this code did `audio_buffer_flush + return 0`
            // on a single very-late frame or after MAX_CONSECUTIVE_LATE
            // consecutive late frames.  That produced a stutter-then-silence
            // loop on every WiFi stall: flush → 200 ms wait → producer
            // re-queues more late frames → flush → ... for several seconds.
            // Per-frame `continue` instead drains stale frames at CPU speed
            // (the 500-attempt loop covers ~11.6 s of audio in microseconds)
            // and stops cleanly when the loop hits an on-time frame, which
            // then plays in the same DMA tick.  The producer-side
            // pre-decode skip in audio_stream_buffered.c is responsible for
            // not letting massive stale backlogs into the ring buffer in the
            // first place after a seek.
            if (timing->consecutive_late_frames == 1) {
              ESP_LOGW(TAG, "Dropping late frame(s): %lld ms",
                       -early_us / 1000LL);
            }
            if (stats) {
              stats->late_frames++;
            }
            if (from_pending) {
              timing->pending_valid = false;
              timing->pending_frame_len = 0;
            } else {
              audio_buffer_return(buffer, item);
            }
            continue;
          }
        }
      }
    }

    // Frame is on time (or anchor-invalid) — reset counters.
    timing->consecutive_early_frames = 0;
    timing->consecutive_late_frames = 0;

    // Copy PCM data to output
    memcpy(out, pcm, frame_samples * channels * sizeof(int16_t));

    // Cleanup
    if (from_pending) {
      timing->pending_valid = false;
      timing->pending_frame_len = 0;
    } else {
      audio_buffer_return(buffer, item);
    }

    if (!timing->playout_started) {
      timing->playout_started = true;
    }

    return frame_samples;
  }

  return 0;
}
