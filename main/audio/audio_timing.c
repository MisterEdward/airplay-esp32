#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "audio_timing.h"

#include "audio_align.h"
#include "audio_output.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ntp_clock.h"
#include "ptp_clock.h"

#define DEFAULT_BUFFER_LATENCY_US 2000 // 2ms startup jitter buffer
// Additional pipeline latency to account for task scheduling, I2S write
// blocking, and resampler processing.  Without this, frames pass the
// timing check "on time" but actually exit the speaker several ms later.
// With the render task's one-block look-ahead now included in the measured
// pipeline, this covers only scheduling jitter and the resampler's group
// delay (taps/2 samples ≈ 0.3 ms).
#define PIPELINE_LATENCY_US 1500
#define MIN_STARTUP_FRAMES  4

// ---------------------------------------------------------------------------
// Two regimes, deliberately kept apart:
//
//  ACQUISITION — the first frame after start / seek / track change / a gap or
//  a drop run.  Here the error is corrected EXACTLY, once: if the frame is
//  early by E µs we emit ceil(E) frames of silence first; if it is late by L µs
//  we discard the first floor(L) frames of the block.  The first audible sample
//  therefore lands within one sample period of its scheduled instant.  Nothing
//  is slewed.  This is what makes a seek line up with the other speakers in a
//  group immediately instead of "drifting into place" over 20 s.
//
//  TRACKING — everything after that.  The only remaining error sources are
//  crystal drift between the sender's clock and ours (10-40 ppm) and slow bias
//  in the pipeline measurement.  A slow servo trims or duplicates single
//  samples, always at the frame's quietest point, so the correction is
//  inaudible.  Its authority is tiered by the size of the filtered error, so a
//  disturbance is removed in seconds while steady state stays at a gentle
//  710 ppm.
// ---------------------------------------------------------------------------

// Position servo (tracking regime).
//
// Control law: filter the per-frame error with a clamped-innovation IIR,
// engage outside a hysteresis band, trim one sample every `interval` frames
// until back inside.  No credit term — a trim changes the real playout
// position and the filter simply tracks it.  The innovation clamp survives
// the one-sided "late read" noise described below.
#define POS_SERVO_FILTER_DIV   16   // IIR divisor, ~0.13 s time constant
#define POS_SERVO_ENGAGE_US    2500 // engage when |filtered err| exceeds this
#define POS_SERVO_DISENGAGE_US 800  // disengage when it falls below this
// Tiered trim intervals (frames per one-sample trim), 352-frame chunks:
//   interval 4 → 710 ppm  (inaudible; steady state)
//   interval 2 → 1420 ppm (still below the ~2000 ppm pitch JND)
//   interval 1 → 2841 ppm (used only for errors > POS_SERVO_TIER2_US, i.e.
//                          a real disturbance; shairport-sync's default
//                          stuffing rate is the same 1 sample/frame)
#define POS_SERVO_TIER1_US       6000 // > this → interval 2
#define POS_SERVO_TIER2_US       15000 // > this → interval 1
#define POS_SERVO_INTERVAL_SLOW  4
#define POS_SERVO_INTERVAL_MED   2
#define POS_SERVO_INTERVAL_FAST  1
// Innovation clamp: cap how far one frame's measurement can move the filter.
// The per-frame error measurement is NOISY in a one-sided way: when the
// render task is briefly starved (WiFi, metadata bursts) the READ happens
// late and the measurement says "late" — but the audio on the wire never
// moved; the DMA ring absorbed the delay.  These artifacts are always
// negative, so an unclamped average is dragged below the true position and
// the servo chases offsets that do not exist.  With the clamp a -22 ms spike
// moves the filter by at most CLAMP/DIV ≈ 94 µs, while a REAL standing offset
// (present on every frame) still walks the filter to engagement in ~0.5 s.
#define POS_SERVO_INNOV_CLAMP_US 1500

// Every audio output backend allocates its read buffer as interleaved
// stereo, and the output stage is stereo regardless of what an incoming
// frame header claims.  Writes into the caller's buffer must therefore be
// bounded by this constant, never by hdr->channels.
#define AUDIO_OUT_CHANNELS 2

// Early/late threshold in the TRACKING regime: how far a contiguous frame
// may be early (held) or late (dropped) before the engine re-acquires.
// Buffered AirPlay 2 streams have a deep jitter buffer so a tight threshold
// keeps sync without drop-outs; unbuffered realtime streams (ALAC/UDP) need
// a looser one to survive scheduling hiccups.  Both are Kconfig-tunable.
#ifdef CONFIG_AIRPLAY_TIMING_THRESHOLD_MS
#define TIMING_THRESHOLD_US (CONFIG_AIRPLAY_TIMING_THRESHOLD_MS * 1000)
#else
#define TIMING_THRESHOLD_US 25000
#endif

#ifdef CONFIG_AIRPLAY_RT_TIMING_THRESHOLD_MS
#define RT_TIMING_THRESHOLD_US (CONFIG_AIRPLAY_RT_TIMING_THRESHOLD_MS * 1000)
#else
#define RT_TIMING_THRESHOLD_US 50000
#endif

// Safety valve: consecutive NEW frames found too early.  Each corresponds to
// a buffer read (~8 ms of output), so 400 ≈ 3+ s of "everything is early"
// — long enough for any legitimate pre-buffer, short enough to catch a
// stuck or absurd anchor.  Pending re-checks of the same frame do not count.
#define MAX_CONSECUTIVE_EARLY 400

// How long to wait for the PTP filter to lock to the clock an AirPlay 2
// anchor names before falling back to the local timeline for that anchor.
// PTP normally locks in <500 ms; 1.5 s covers a slow first SYNC without
// making a cold start feel sluggish.
#define PTP_LOCK_WAIT_US 1500000LL

static const char *TAG = "audio_time";

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

// The clock domain is decided per anchor and held (see anchor_clock_id).
static sync_mode_t current_sync_mode(const audio_timing_t *timing) {
  if (timing->ptp_locked) {
    // Once latched, stay on the PTP timeline even if the lock flag drops
    // (a 5 s SYNC gap): ptp_clock_get_offset_ns() keeps the last filtered
    // offset, which is far closer to the truth than a jump to local time.
    return SYNC_MODE_PTP;
  }
  if (timing->anchor_clock_id == 0 && ntp_clock_is_locked()) {
    return SYNC_MODE_NTP;
  }
  return SYNC_MODE_NONE;
}

const char *audio_timing_sync_mode_name(const audio_timing_t *timing) {
  if (!timing) {
    return "?";
  }
  switch (current_sync_mode(timing)) {
  case SYNC_MODE_PTP:
    return "ptp";
  case SYNC_MODE_NTP:
    return "ntp";
  default:
    return "local";
  }
}

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

  // Stream playout latency.  For AirPlay 2 REALTIME streams (type 96) the
  // anchor maps an RTP timestamp onto the sender's source timeline, and the
  // receiver is expected to emit that frame `latencyMin` samples LATER —
  // 11025 samples (250 ms) unless SETUP negotiates otherwise.  Every
  // reference receiver applies this delay; playing at the anchor instant
  // directly makes this device lead the whole group by exactly 250 ms.
  // Buffered streams (type 103) schedule playout with the anchor directly
  // and keep this at 0.
  int64_t latency_ns =
      ((int64_t)timing->playout_latency_samples * 1000000000LL) /
      format->sample_rate;

  int64_t target_ns;
  switch (sync_mode) {
  case SYNC_MODE_PTP:
    target_ns = (int64_t)timing->anchor_network_time_ns -
                ptp_clock_get_offset_ns() + frame_offset_ns + latency_ns;
    break;
  case SYNC_MODE_NTP:
    target_ns = (int64_t)timing->anchor_network_time_ns -
                ntp_clock_get_offset_ns() + frame_offset_ns + latency_ns;
    break;
  default:
    target_ns = timing->anchor_local_time_ns + frame_offset_ns + latency_ns;
    break;
  }

  // Subtract the output pipeline delay: the time between this call handing a
  // sample to the backend and that sample leaving the DAC.  Prefer the LIVE
  // measurement (DMA queue depth + the render task's held block); the
  // modelled constant is only a fallback for backends without a cursor.
  int64_t now_us = 0;
  uint32_t pipeline_us = 0;
  if (!audio_output_get_pipeline_us(&now_us, &pipeline_us)) {
    now_us = esp_timer_get_time();
    pipeline_us = audio_output_get_hardware_latency_us();
  }
  target_ns -= (int64_t)(pipeline_us + PIPELINE_LATENCY_US) * 1000LL;

  *early_us = (target_ns / 1000LL) - now_us;

  return true;
}

// Index of the quietest sample in a frame (smallest summed magnitude across
// the output channels).  Servo trims drop or duplicate exactly one sample;
// doing that at the frame's quietest point makes the waveform seam
// inaudible even on loud tonal content.
static size_t quietest_sample_index(const int16_t *pcm, size_t frame_samples,
                                    size_t channels, size_t out_ch) {
  size_t best = frame_samples - 1;
  int32_t best_mag = INT32_MAX;
  for (size_t i = 0; i < frame_samples; i++) {
    int32_t mag = 0;
    for (size_t ch = 0; ch < out_ch; ch++) {
      int32_t s = pcm[i * channels + ch];
      mag += s < 0 ? -s : s;
    }
    if (mag < best_mag) {
      best_mag = mag;
      best = i;
    }
  }
  return best;
}

void audio_timing_init(audio_timing_t *timing, size_t pending_capacity) {
  if (!timing) {
    return;
  }

  memset(timing, 0, sizeof(*timing));
  timing->output_latency_us = DEFAULT_BUFFER_LATENCY_US;
  timing->playing = true;
  timing->servo_interval = POS_SERVO_INTERVAL_SLOW;

  if (pending_capacity > 0) {
    timing->pending_frame = (uint8_t *)malloc(pending_capacity);
    if (timing->pending_frame) {
      timing->pending_frame_capacity = pending_capacity;
    }
  }
}

void audio_timing_reset_continuity(audio_timing_t *timing) {
  if (!timing) {
    return;
  }
  timing->expected_rtp_valid = false;
  timing->pos_err_filtered_us = 0;
  timing->servo_engaged = false;
  timing->servo_phase = 0;
  timing->servo_interval = POS_SERVO_INTERVAL_SLOW;
}

void audio_timing_reset(audio_timing_t *timing) {
  if (!timing) {
    return;
  }

  timing->playout_started = false;
  timing->anchor_valid = false;
  timing->anchor_clock_id = 0;
  timing->ptp_locked = false;
  timing->ptp_wait_expired = false;
  timing->pending_valid = false;
  timing->pending_frame_len = 0;
  timing->ready_time_us = 0;
  timing->consecutive_early_frames = 0;
  timing->quick_start = false;
  timing->deferred_flush_pending = false;
  timing->flush_until_ts = 0;
  timing->late_drop_count = 0;
  timing->late_drop_active = false;
  timing->servo_trims = 0;
  timing->acquired = false;
  timing->acquire_err_us = 0;
  timing->align_silence = 0;
  timing->align_trimmed = 0;
  timing->read_has_media = false;
  audio_timing_reset_continuity(timing);
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
  return audio_output_get_hardware_latency_us();
}

uint32_t audio_timing_get_advertised_latency(const audio_timing_t *timing) {
  uint32_t base =
      timing ? timing->output_latency_us : DEFAULT_BUFFER_LATENCY_US;
  return base + audio_output_get_hardware_latency_us() + PIPELINE_LATENCY_US;
}

void audio_timing_set_playout_latency(audio_timing_t *timing,
                                      uint32_t latency_samples) {
  if (!timing) {
    return;
  }
  timing->playout_latency_samples = latency_samples;
}

void audio_timing_set_anchor(audio_timing_t *timing,
                             const audio_format_t *format, uint64_t clock_id,
                             uint64_t network_time_ns, uint32_t rtp_time) {
  if (!timing || !format) {
    return;
  }

  int64_t now_ns = (int64_t)esp_timer_get_time() * 1000LL;

  timing->anchor_rtp_time = rtp_time;
  timing->anchor_network_time_ns = network_time_ns;
  timing->anchor_local_time_ns = now_ns;
  timing->anchor_clock_id = clock_id;
  // Latch the clock domain for this anchor.  AirPlay 2 anchors name the
  // PTP master; only schedule on the PTP timeline if OUR filter is locked to
  // THAT clock.  Otherwise audio_timing_read() waits briefly for the lock.
  timing->ptp_locked =
      clock_id != 0 ? ptp_clock_is_locked_to(clock_id) : ptp_clock_is_locked();
  timing->ptp_wait_expired = false;
  timing->anchor_valid = true;
  timing->consecutive_early_frames = 0;
  // A new anchor is a new schedule: the first frame under it is acquired
  // exactly, whatever was playing before.
  timing->acquired = false;
  timing->acquire_err_us = 0;
  timing->align_silence = 0;
  timing->align_trimmed = 0;

  // Lead time: how far in the future the anchor's network timestamp is.
  // Negative means already in the past (normal: the phone anchors ~10 ms
  // before the first frame it wants heard).
  int64_t lead_ms = 0;
  if (timing->ptp_locked) {
    lead_ms = ((int64_t)network_time_ns -
               (int64_t)(ptp_clock_get_offset_ns() + now_ns)) /
              1000000LL;
  }
  ESP_LOGI(TAG,
           "Anchor: rtp=%" PRIu32 " clock=%016llx lead=%lld ms domain=%s "
           "quick_start=%d",
           rtp_time, (unsigned long long)clock_id, (long long)lead_ms,
           audio_timing_sync_mode_name(timing), timing->quick_start);
}

void audio_timing_set_playing(audio_timing_t *timing, bool playing) {
  if (!timing) {
    return;
  }

  if (timing->playing != playing) {
    ESP_LOGI(TAG, "set_playing: %s -> %s",
             timing->playing ? "playing" : "paused",
             playing ? "playing" : "paused");
  }

  timing->playing = playing;
  if (!playing) {
    // Discard any partially-pending frame so resume starts cleanly from
    // the oldest frame in the sorted buffer.
    timing->pending_valid = false;
    timing->pending_frame_len = 0;
  }
}

// Release a frame slot back to wherever it came from.
static void release_item(audio_timing_t *timing, audio_buffer_t *buffer,
                         void *item, bool from_pending) {
  if (from_pending) {
    timing->pending_valid = false;
    timing->pending_frame_len = 0;
  } else {
    audio_buffer_return(buffer, item);
  }
}

// Emit `frames` frames of silence and report it as non-media.
static size_t emit_silence(audio_timing_t *timing, int16_t *out, size_t frames) {
  memset(out, 0, frames * AUDIO_OUT_CHANNELS * sizeof(int16_t));
  timing->read_has_media = false;
  return frames;
}

size_t audio_timing_read(audio_timing_t *timing, audio_buffer_t *buffer,
                         const audio_stream_t *stream, audio_stats_t *stats,
                         int16_t *out, size_t samples) {
  if (!timing || !buffer || !stream || !out || samples == 0) {
    return 0;
  }
  timing->read_has_media = false;

  if (!timing->playing) {
    return 0;
  }

  const audio_format_t *format = &stream->format;
  int buffered_frames = audio_buffer_get_frame_count(buffer);

  const int64_t timing_threshold_us = audio_stream_uses_buffer(stream->type)
                                          ? TIMING_THRESHOLD_US
                                          : RT_TIMING_THRESHOLD_US;

  // Startup gate.  In quick_start mode (after a seek/skip) start as soon as
  // one frame is available; a normal start waits for target_buffer_frames.
  if (!timing->playout_started && !timing->pending_valid) {
    int required = timing->quick_start ? 1 : (int)timing->target_buffer_frames;
    if (buffered_frames < required) {
      return 0;
    }
    if (!timing->anchor_valid) {
      // Allow a 1-second fallback so a stream with no anchor (AirPlay 1
      // without NTP) can still start.
      int64_t now_us = esp_timer_get_time();
      if (timing->ready_time_us == 0) {
        timing->ready_time_us = now_us;
      }
      if (now_us - timing->ready_time_us < 1000000) {
        return 0;
      }
    }
  }

  // PTP lock wait.  The anchor named a clock we are not locked to yet: hold
  // playout briefly, latch PTP the moment the filter locks, or give up and
  // latch the local timeline for this anchor.  Never switch domains after
  // playout under this anchor has begun.
  if (timing->anchor_valid && timing->anchor_clock_id != 0 &&
      !timing->ptp_locked && !timing->ptp_wait_expired) {
    if (ptp_clock_is_locked_to(timing->anchor_clock_id)) {
      timing->ptp_locked = true;
      ESP_LOGI(TAG, "PTP locked to %016llx before playout; network timeline "
                    "latched",
               (unsigned long long)timing->anchor_clock_id);
    } else {
      int64_t waited_us =
          esp_timer_get_time() - timing->anchor_local_time_ns / 1000LL;
      if (waited_us < PTP_LOCK_WAIT_US && !timing->playout_started) {
        return 0;
      }
      timing->ptp_wait_expired = true;
      ESP_LOGW(TAG,
               "PTP not locked to %016llx after %lld ms (tracking %016llx); "
               "local timeline latched for this anchor",
               (unsigned long long)timing->anchor_clock_id,
               (long long)(waited_us / 1000LL),
               (unsigned long long)ptp_clock_get_tracked_clock_id());
    }
  }

  sync_mode_t sync_mode = current_sync_mode(timing);

  // Bounded drain: stale frames are discarded in batches, but a single call
  // spends at most ~1.5 ms here.
  enum { MAX_DRAIN_ATTEMPTS = 64 };
  const int64_t drain_deadline_us = esp_timer_get_time() + 1500;
  bool dropped_late = timing->late_drop_active;
  int start_skips = 0;
  for (int attempt = 0; attempt < MAX_DRAIN_ATTEMPTS; attempt++) {
    if ((attempt & 7) == 0 && attempt != 0 &&
        esp_timer_get_time() >= drain_deadline_us) {
      break;
    }
    size_t item_size = 0;
    void *item = NULL;
    bool from_pending = false;

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

    if (frame_samples == 0 || channels == 0) {
      release_item(timing, buffer, item, from_pending);
      continue;
    }
    size_t expected_bytes =
        sizeof(*hdr) + frame_samples * channels * sizeof(int16_t);
    if (item_size < expected_bytes) {
      release_item(timing, buffer, item, from_pending);
      continue;
    }
    if (frame_samples > samples) {
      frame_samples = samples;
    }

    // Deferred flush (AirPlay 2 FLUSHBUFFERED with flushFromSeq): play up to
    // the boundary, then discard the rest and start the next track fresh.
    if (timing->deferred_flush_pending) {
      if ((int32_t)(hdr->rtp_timestamp - timing->flush_until_ts) >= 0) {
        ESP_LOGI(TAG,
                 "Deferred flush at ts=%" PRIu32 " (until_ts=%" PRIu32 ")",
                 hdr->rtp_timestamp, timing->flush_until_ts);
        release_item(timing, buffer, item, from_pending);
        audio_buffer_flush(buffer);
        timing->deferred_flush_pending = false;
        audio_timing_reset_continuity(timing);
        timing->playout_started = false;
        timing->ready_time_us = 0;
        timing->consecutive_early_frames = 0;
        timing->quick_start = true;
        timing->acquired = false;
        return 0;
      }
    }

    // Stale start-island rejection: before playout, skip frames stranded
    // more than ~100 ms below the contiguous run that ends at the newest
    // frame (late retransmissions from before a flush).  Playing them would
    // be a ~100 ms blip, then silence, then the track.
    if (!timing->playout_started && format->sample_rate > 0) {
      uint32_t bulk_rtp = 0;
      if (audio_buffer_bulk_start_rtp(buffer, &bulk_rtp)) {
        int32_t behind = (int32_t)(bulk_rtp - hdr->rtp_timestamp);
        if (behind > (int32_t)(format->sample_rate / 10)) {
          start_skips++;
          release_item(timing, buffer, item, from_pending);
          continue;
        }
      }
      if (start_skips > 0) {
        ESP_LOGI(TAG,
                 "Skipped %d stale start frame(s); contiguous stream begins "
                 "at rtp=%" PRIu32,
                 start_skips, hdr->rtp_timestamp);
        start_skips = 0;
      }
    }

    // RTP continuity vs the frame just played.  A fresh frame ABOVE
    // expected_rtp means packets were lost and not recovered: conceal the
    // hole by re-acquiring (exact silence for exactly the gap length).
    bool gap = false;
    if (!from_pending && timing->playout_started &&
        timing->expected_rtp_valid) {
      int32_t cont_delta = (int32_t)(hdr->rtp_timestamp - timing->expected_rtp);
      if (cont_delta > 0) {
        gap = true;
        timing->gaps++;
        int64_t now_us = esp_timer_get_time();
        if (now_us - timing->last_gap_log_us > 250000) {
          ESP_LOGW(TAG,
                   "Gap: %ld samples (%ld ms) missing before rtp=%" PRIu32
                   " — concealing with silence (gaps=%" PRIu32 ", +%" PRIu32
                   " unlogged)",
                   (long)cont_delta,
                   (long)(cont_delta * 1000L / format->sample_rate),
                   hdr->rtp_timestamp, timing->gaps, timing->gaps_suppressed);
          timing->last_gap_log_us = now_us;
          timing->gaps_suppressed = 0;
        } else {
          timing->gaps_suppressed++;
        }
      }
    }

    size_t prefix_trim = 0;
    if (timing->anchor_valid && format->sample_rate > 0) {
      int64_t early_us = 0;
      if (compute_early_us(timing, format, hdr->rtp_timestamp, sync_mode,
                           &early_us)) {
        int64_t frame_period_us =
            ((int64_t)frame_samples * 1000000LL) / format->sample_rate;
        // Acquisition regime: first frame under this anchor, a pending
        // re-check, the frame after a drop run, or the frame after a gap.
        bool acquiring = !timing->acquired || from_pending || dropped_late ||
                         gap;

        if (acquiring) {
          // ---- exact alignment -------------------------------------
          // An anchor more than 10 s in the future is not an AirPlay
          // schedule (senders anchor at most ~2-3 s ahead); it is a clock
          // domain mismatch.  Play unscheduled rather than sit in silence.
          if (early_us > 10000000LL) {
            ESP_LOGW(TAG,
                     "Anchor %lld ms in the future — implausible, playing "
                     "unscheduled (domain=%s)",
                     (long long)(early_us / 1000LL),
                     audio_timing_sync_mode_name(timing));
            timing->anchor_valid = false;
            goto play_frame;
          }
          if (early_us > 0) {
            // Too early.  Shelve the frame and emit exactly the silence
            // that separates now from its play time (bounded by the
            // caller's capacity; long waits take several calls, each
            // of which shortens the remaining error via the pipeline
            // measurement).
            if (!from_pending) {
              timing->consecutive_early_frames++;
              if (timing->consecutive_early_frames == 1) {
                ESP_LOGI(TAG,
                         "First early frame: rtp=%" PRIu32 " early=%.1f ms "
                         "quick_start=%d buffered=%d domain=%s",
                         hdr->rtp_timestamp, (float)early_us / 1000.0f,
                         timing->quick_start, buffered_frames,
                         audio_timing_sync_mode_name(timing));
              }
              if (timing->consecutive_early_frames > MAX_CONSECUTIVE_EARLY) {
                ESP_LOGW(TAG,
                         "Invalidating stuck anchor: consecutive=%d early=%lld "
                         "ms",
                         timing->consecutive_early_frames,
                         (long long)(early_us / 1000LL));
                timing->anchor_valid = false;
                timing->consecutive_early_frames = 0;
                goto play_frame; // fall through: play without schedule
              }
              if (timing->pending_frame &&
                  item_size <= timing->pending_frame_capacity) {
                memcpy(timing->pending_frame, item, item_size);
                timing->pending_frame_len = item_size;
                timing->pending_valid = true;
                audio_buffer_return(buffer, item);
              } else {
                // Cannot shelve (should never happen): play it now.
                goto play_frame;
              }
            }
            size_t silence = audio_align_silence_frames(
                early_us, (uint32_t)format->sample_rate, samples);
            if (silence == 0) {
              silence = 1;
            }
            timing->align_silence += (uint32_t)silence;
            return emit_silence(timing, out, silence);
          }

          // On time or late.  Late by less than the block: trim the expired
          // leading samples.  Late by a whole block or more: it is a stale
          // frame, drop it and keep draining.
          if (early_us < 0) {
            size_t trim = audio_align_trim_frames(
                -early_us, (uint32_t)format->sample_rate, frame_samples);
            if (trim >= frame_samples) {
              dropped_late = true;
              timing->late_drop_count++;
              timing->late_drop_active = true;
              uint32_t drop_next =
                  hdr->rtp_timestamp + hdr->samples_per_channel;
              if (!timing->expected_rtp_valid ||
                  (int32_t)(drop_next - timing->expected_rtp) > 0) {
                timing->expected_rtp = drop_next;
                timing->expected_rtp_valid = true;
              }
              if (stats) {
                stats->late_frames++;
              }
              release_item(timing, buffer, item, from_pending);
              continue;
            }
            prefix_trim = trim;
            timing->align_trimmed += (uint32_t)trim;
          }
          timing->acquire_err_us = early_us;
          timing->consecutive_early_frames = 0;
        } else {
          // ---- tracking regime ------------------------------------
          if (early_us > timing_threshold_us) {
            // A contiguous frame far too early means the anchor jumped
            // forward (or our position ran ahead).  Shelve it; the pending
            // re-check re-acquires exactly.
            if (timing->pending_frame &&
                item_size <= timing->pending_frame_capacity) {
              memcpy(timing->pending_frame, item, item_size);
              timing->pending_frame_len = item_size;
              timing->pending_valid = true;
              audio_buffer_return(buffer, item);
              ESP_LOGW(TAG,
                       "Tracking: frame %.1f ms early (> %lld ms); "
                       "re-acquiring",
                       (float)early_us / 1000.0f,
                       (long long)(timing_threshold_us / 1000LL));
              size_t silence = audio_align_silence_frames(
                  early_us, (uint32_t)format->sample_rate, samples);
              return emit_silence(timing, out, silence ? silence : 1);
            }
          } else if (early_us < -timing_threshold_us) {
            // Far too late: start a drop run.  Frames keep being dropped
            // until one is within a block of its schedule; that frame is
            // then acquired exactly (trimmed) by the branch above.
            dropped_late = true;
            timing->late_drop_count++;
            timing->late_drop_active = true;
            uint32_t drop_next = hdr->rtp_timestamp + hdr->samples_per_channel;
            if (!timing->expected_rtp_valid ||
                (int32_t)(drop_next - timing->expected_rtp) > 0) {
              timing->expected_rtp = drop_next;
              timing->expected_rtp_valid = true;
            }
            if (stats) {
              stats->late_frames++;
            }
            release_item(timing, buffer, item, from_pending);
            continue;
          }
          timing->consecutive_early_frames = 0;
        }
        (void)frame_period_us;
      }
    }

  play_frame:;
    // Snapshot metadata before the pool slot is returned below.
    uint32_t played_rtp_timestamp = hdr->rtp_timestamp + (uint32_t)prefix_trim;
    uint32_t played_samples = hdr->samples_per_channel - (uint16_t)prefix_trim;
    pcm += prefix_trim * channels;
    frame_samples -= prefix_trim;

    // Position servo (tracking only) and periodic playout report.
    int sample_adjust = 0;
    if (timing->anchor_valid && sync_mode != SYNC_MODE_NONE &&
        timing->playout_started && timing->acquired) {
      int64_t on_time_err_us = 0;
      if (compute_early_us(timing, format, played_rtp_timestamp, sync_mode,
                           &on_time_err_us)) {
        if (timing->playout_reports++ % 125 == 0) {
          uint32_t newest_rtp = 0;
          int64_t depth_ms = -1;
          if (audio_buffer_peek_newest_rtp(buffer, &newest_rtp)) {
            depth_ms = ((int64_t)(int32_t)(newest_rtp - played_rtp_timestamp) *
                        1000LL) /
                       format->sample_rate;
          }
          ptp_stats_t ps;
          ptp_clock_get_stats(&ps);
          int64_t ptp_gap_us =
              (ps.last_offset_ns - ps.filtered_offset_ns) / 1000LL;
          ESP_LOGI(TAG,
                   "Playout: err=%+lld us filt=%+lld us servo=%s/%u trims=%" PRIu32
                   " buffered=%d depth=%lld ms ptp_gap=%lld us outliers=%" PRIu32
                   " gaps=%" PRIu32 " under=%" PRIu32 " domain=%s rtp=%" PRIu32,
                   (long long)on_time_err_us,
                   (long long)timing->pos_err_filtered_us,
                   timing->servo_engaged ? "on" : "off", timing->servo_interval,
                   timing->servo_trims, buffered_frames, (long long)depth_ms,
                   (long long)ptp_gap_us, ps.outlier_count, timing->gaps,
                   audio_output_get_underruns(),
                   audio_timing_sync_mode_name(timing), played_rtp_timestamp);
        }

        int64_t innovation = on_time_err_us - timing->pos_err_filtered_us;
        if (innovation > POS_SERVO_INNOV_CLAMP_US) {
          innovation = POS_SERVO_INNOV_CLAMP_US;
        } else if (innovation < -POS_SERVO_INNOV_CLAMP_US) {
          innovation = -POS_SERVO_INNOV_CLAMP_US;
        }
        timing->pos_err_filtered_us += innovation / POS_SERVO_FILTER_DIV;

        int64_t abs_err = timing->pos_err_filtered_us < 0
                              ? -timing->pos_err_filtered_us
                              : timing->pos_err_filtered_us;
        if (!timing->servo_engaged && abs_err > POS_SERVO_ENGAGE_US) {
          timing->servo_engaged = true;
          timing->servo_phase = 0;
          ESP_LOGI(TAG, "Servo engaged: err=%+lld us",
                   (long long)timing->pos_err_filtered_us);
        } else if (timing->servo_engaged && abs_err < POS_SERVO_DISENGAGE_US) {
          timing->servo_engaged = false;
          ESP_LOGI(TAG, "Servo disengaged: err=%+lld us trims=%" PRIu32,
                   (long long)timing->pos_err_filtered_us, timing->servo_trims);
        }
        // Tier the authority by the size of the remaining error.
        uint8_t interval = abs_err > POS_SERVO_TIER2_US ? POS_SERVO_INTERVAL_FAST
                           : abs_err > POS_SERVO_TIER1_US
                               ? POS_SERVO_INTERVAL_MED
                               : POS_SERVO_INTERVAL_SLOW;
        if (interval != timing->servo_interval) {
          timing->servo_interval = interval;
          if (timing->servo_engaged) {
            ESP_LOGI(TAG, "Servo tier: 1 trim / %u frames (err=%+lld us)",
                     interval, (long long)timing->pos_err_filtered_us);
          }
        }

        if (timing->servo_engaged &&
            ++timing->servo_phase >= timing->servo_interval) {
          timing->servo_phase = 0;
          // Positive error = playing early = stretch (emit one extra sample)
          // so playout slows; negative = late = shrink to catch up.
          sample_adjust = timing->pos_err_filtered_us > 0 ? 1 : -1;
          timing->servo_trims++;
        }
      }
    }

    size_t out_samples = frame_samples;
    if (sample_adjust < 0 && out_samples > 1) {
      out_samples--;
    } else if (sample_adjust > 0 && out_samples + 1 <= samples) {
      out_samples++;
    }

    // Copy PCM to the interleaved stereo output.  `channels` steps through
    // the SOURCE frame (length-validated above); the output is always
    // AUDIO_OUT_CHANNELS wide.  Mono sources are duplicated to both sides.
    size_t out_ch = AUDIO_OUT_CHANNELS;
    if (out_samples == frame_samples && channels == AUDIO_OUT_CHANNELS) {
      memcpy(out, pcm, frame_samples * out_ch * sizeof(int16_t));
    } else {
      size_t m = out_samples == frame_samples
                     ? SIZE_MAX
                     : quietest_sample_index(pcm, frame_samples, channels,
                                             channels < out_ch ? channels
                                                               : out_ch);
      size_t o = 0;
      for (size_t i = 0; i < frame_samples; i++) {
        if (out_samples < frame_samples && i == m) {
          continue; // shrink: skip the quietest sample
        }
        for (size_t ch = 0; ch < out_ch; ch++) {
          out[o * out_ch + ch] = pcm[i * channels + (channels == 1 ? 0 : ch)];
        }
        o++;
        if (out_samples > frame_samples && i == m) {
          for (size_t ch = 0; ch < out_ch; ch++) {
            out[o * out_ch + ch] = pcm[i * channels + (channels == 1 ? 0 : ch)];
          }
          o++;
        }
      }
    }

    // The next contiguous frame starts where this one's SOURCE data ends.
    uint32_t play_next = played_rtp_timestamp + played_samples;
    if (!timing->expected_rtp_valid ||
        (int32_t)(play_next - timing->expected_rtp) > 0) {
      timing->expected_rtp = play_next;
      timing->expected_rtp_valid = true;
    }

    release_item(timing, buffer, item, from_pending);

    if (!timing->acquired) {
      timing->acquired = true;
      ESP_LOGI(TAG,
               "Acquired: rtp=%" PRIu32 " err=%+lld us silence=%" PRIu32
               " trimmed=%" PRIu32 " domain=%s%s",
               played_rtp_timestamp, (long long)timing->acquire_err_us,
               timing->align_silence, timing->align_trimmed,
               audio_timing_sync_mode_name(timing),
               timing->anchor_valid ? "" : " (no anchor)");
    }
    if (!timing->playout_started) {
      timing->playout_started = true;
      bool was_quick = timing->quick_start;
      timing->quick_start = false;
      ESP_LOGI(TAG, "Playout started%s: rtp=%" PRIu32,
               was_quick ? " (quick_start)" : "", played_rtp_timestamp);
    }
    if (timing->late_drop_active) {
      ESP_LOGW(TAG, "Late-frame drain complete: dropped=%" PRIu32,
               timing->late_drop_count);
      timing->late_drop_count = 0;
      timing->late_drop_active = false;
    }

    timing->read_has_media = true;
    return out_samples;
  }

  return 0;
}
