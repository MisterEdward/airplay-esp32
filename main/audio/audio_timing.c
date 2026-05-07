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
#define HARDWARE_OUTPUT_LATENCY_US    46000  // ~46ms I2S DMA latency
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
// infinite bypass if the anchor never settles.  At MAX_CORRECTION_PPM=500 the
// servo converges ~130 ms of structural post-seek drift in ~2.5 minutes;
// 5 minutes gives comfortable headroom.
#define POST_FLUSH_TIMEOUT_US 300000000LL // 5 min
// Number of consecutive on-time frames (within the normal TIMING_THRESHOLD
// window) before exiting post_flush.  Prevents exiting on a single lucky
// frame amidst jitter.
#define POST_FLUSH_ONTIME_EXIT_COUNT 10
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
  uint32_t target_frames =
      (uint32_t)((latency_samples + timing->nominal_frame_samples - 1) /
                 timing->nominal_frame_samples);
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
  timing->deferred_flush_pending = false;
  timing->flush_until_ts = 0;
  timing->drift_min_us = INT32_MAX;
  timing->drift_max_us = INT32_MIN;
  timing->drift_samples = 0;
  timing->smoothed_drift_us = 0;
  timing->smoothed_drift_valid = false;
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
    if (buffered_frames < (int)timing->target_buffer_frames) {
      return 0;
    }
    // Wait for anchor before playing.
    // Normal startup: allow a 1-second fallback so a stream with no anchor
    // (e.g. AirPlay 1 without NTP) can still start.
    if (!timing->anchor_valid) {
      int64_t now_us = esp_timer_get_time();
      if (timing->ready_time_us == 0) {
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

  for (int attempt = 0; attempt < 8; attempt++) {
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
        timing->playout_started = false;
        timing->ready_time_us = 0;
        timing->consecutive_early_frames = 0;
        timing->consecutive_late_frames = 0;
        // post_flush = true so the first frame of the next track plays
        // immediately rather than waiting out the phone's pre-buffer window.
        timing->post_flush = true;
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
          // Exception: frames that are MORE than POST_FLUSH_STALE_THRESHOLD_US
          // early are old-position data still draining from the TCP kernel
          // buffer (e.g. frames from 2:30 after a seek back to 0:00).  Discard
          // those so the user never hears audio from the wrong position.
          if (early_us > POST_FLUSH_STALE_THRESHOLD_US) {
            // This frame is from the wrong seek position — still draining the
            // TCP kernel buffer from before the flush.  Bulk-flush the entire
            // ring buffer so all remaining stale (and any already-queued new)
            // frames are cleared in one shot.  Draining one-by-one takes
            // hundreds of DMA callbacks (8 frames/callback × hundreds of stale
            // frames) causing seconds of silent lag that compounds each seek.
            ESP_LOGW(
                TAG, "post_flush: bulk flush %d stale frames (%lld s early)",
                audio_buffer_get_frame_count(buffer), early_us / 1000000LL);
            if (from_pending) {
              timing->pending_valid = false;
              timing->pending_frame_len = 0;
            } else {
              audio_buffer_return(buffer, item);
            }
            audio_buffer_flush(buffer);
            timing->playout_started = false;
            timing->ready_time_us = 0;
            timing->consecutive_early_frames = 0;
            timing->consecutive_late_frames = 0;
            // Keep post_flush=true so new-position frames that refill the
            // buffer will play immediately rather than waiting out the anchor.
            return 0;
          }
          // Within pre-buffer depth — play and check if we should exit.
          // Exit post_flush only when frames are within the NORMAL timing
          // thresholds (±60ms/40ms).  The servo runs during post_flush
          // (EMA updates are not suppressed), converging the structural
          // ~130 ms post-seek drift at up to MAX_CORRECTION_PPM.  At
          // 500 ppm this takes ~2.5 minutes — during which all frames
          // play unconditionally with zero silence.
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
            ESP_LOGI(TAG,
                     "post_flush done: early=%lld ms, elapsed=%lld ms, "
                     "ontime=%d",
                     early_us / 1000LL, flush_elapsed / 1000LL,
                     timing->post_flush_ontime_count);
            timing->post_flush = false;
            timing->post_flush_start_us = 0;
            timing->post_flush_ontime_count = 0;
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
        } else if (early_us < -TIMING_THRESHOLD_LATE_US) {
          // Reset consecutive early counter on late/normal frames
          timing->consecutive_early_frames = 0;
          timing->consecutive_late_frames++;

          if (-early_us > BULK_FLUSH_LATE_THRESHOLD_US ||
              timing->consecutive_late_frames > MAX_CONSECUTIVE_LATE) {
            // Bulk flush the stale buffer.  Two triggers:
            //  1. A single frame is massively late (> 2 s): e.g. after resume
            //     from a long pause where the phone advanced the anchor past
            //     its pre-buffer window.
            //  2. Many consecutive individually-late frames (e.g. after a
            //     track skip where the anchor's network_time has already
            //     passed): the individual-drop path would drain hundreds of
            //     frames one-by-one over several seconds; bulk flush instead.
            ESP_LOGW(TAG,
                     "Bulk flush: frame %lld ms late, consecutive_late=%d, "
                     "flushing %d stale frames",
                     -early_us / 1000LL, timing->consecutive_late_frames,
                     audio_buffer_get_frame_count(buffer));
            if (from_pending) {
              timing->pending_valid = false;
              timing->pending_frame_len = 0;
            } else {
              audio_buffer_return(buffer, item);
            }
            audio_buffer_flush(buffer);
            timing->playout_started = false;
            timing->ready_time_us = 0;
            timing->consecutive_late_frames = 0;
            if (stats) {
              stats->late_frames++;
            }
            return 0;
          }

          // Too late but within normal range: drop this single frame.
          // Rate-limit the log — spamming LOGW on every frame adds
          // UART-blocking latency that makes the drain period even longer.
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
