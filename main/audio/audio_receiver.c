#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "audio_receiver.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "audio_buffer.h"
#include "audio_decoder.h"
#include "audio_output.h"
#include "audio_receiver_internal.h"
#include "audio_servo.h"
#include "audio_stream.h"
#include "audio_timing.h"
#include "ptp_clock.h"

#define DEFAULT_SAMPLE_RATE     44100
#define DEFAULT_CHANNELS        2
#define DEFAULT_BITS_PER_SAMPLE 16
#define DEFAULT_FRAME_SIZE      352
#define DECRYPT_BUFFER_SIZE     8192

static const char *TAG = "audio_recv";

static audio_receiver_state_t receiver = {0};

static void audio_receiver_reset_stats(void) {
  memset(&receiver.stats, 0, sizeof(receiver.stats));
  // Reset per-receiver jitter tracking too so the first packet of a new
  // session doesn't compute a multi-second arrival delta against the previous
  // session's last_arrival_local_us (showing as a huge fake jitter spike).
  receiver.last_arrival_local_us = 0;
  receiver.last_arrival_rtp_ts = 0;
  receiver.jitter_initialised = false;
  // Forget the last seen PTP master so the first anchor of a new session
  // doesn't trigger a false "master changed" event.
  receiver.last_anchor_clock_id = 0;
}

static void audio_receiver_reset_blocks(void) {
  receiver.blocks_read = 0;
  receiver.blocks_read_in_sequence = 0;
}

static void audio_receiver_copy_stream_state(audio_stream_t *dst,
                                             const audio_stream_t *src) {
  if (!dst || !src) {
    return;
  }

  dst->format = src->format;
  dst->encrypt = src->encrypt;
}

esp_err_t audio_receiver_init(void) {
  if (receiver.buffer.pool) {
    return ESP_OK;
  }

  receiver.realtime_stream = audio_stream_create_realtime();
  if (receiver.realtime_stream) {
    receiver.realtime_stream->ctx = &receiver;
  }
  receiver.buffered_stream = audio_stream_create_buffered();
  if (receiver.buffered_stream) {
    receiver.buffered_stream->ctx = &receiver;
  }
  if (!receiver.realtime_stream || !receiver.buffered_stream) {
    ESP_LOGE(TAG, "Failed to allocate audio streams");
    audio_stream_destroy(receiver.realtime_stream);
    audio_stream_destroy(receiver.buffered_stream);
    receiver.realtime_stream = NULL;
    receiver.buffered_stream = NULL;
    return ESP_ERR_NO_MEM;
  }

  receiver.stream = receiver.realtime_stream;

  audio_format_t default_format = {0};
  strcpy(default_format.codec, "AppleLossless");
  default_format.sample_rate = DEFAULT_SAMPLE_RATE;
  default_format.channels = DEFAULT_CHANNELS;
  default_format.bits_per_sample = DEFAULT_BITS_PER_SAMPLE;
  default_format.frame_size = DEFAULT_FRAME_SIZE;

  receiver.realtime_stream->format = default_format;
  receiver.buffered_stream->format = default_format;

  esp_err_t err = audio_buffer_init(&receiver.buffer);
  if (err != ESP_OK) {
    audio_stream_destroy(receiver.realtime_stream);
    audio_stream_destroy(receiver.buffered_stream);
    receiver.realtime_stream = NULL;
    receiver.buffered_stream = NULL;
    return err;
  }

  receiver.decrypt_buffer_size = DECRYPT_BUFFER_SIZE;
#ifdef CONFIG_SPIRAM
  receiver.decrypt_buffer = heap_caps_malloc(
      receiver.decrypt_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (!receiver.decrypt_buffer) {
    receiver.decrypt_buffer = malloc(receiver.decrypt_buffer_size);
  }
  if (!receiver.decrypt_buffer) {
    ESP_LOGE(TAG, "Failed to allocate decrypt buffer");
    audio_buffer_deinit(&receiver.buffer);
    audio_stream_destroy(receiver.realtime_stream);
    audio_stream_destroy(receiver.buffered_stream);
    receiver.realtime_stream = NULL;
    receiver.buffered_stream = NULL;
    return ESP_ERR_NO_MEM;
  }

  size_t pending_capacity =
      sizeof(audio_frame_header_t) +
      ((size_t)MAX_SAMPLES_PER_FRAME * AUDIO_MAX_CHANNELS * sizeof(int16_t));
  audio_timing_init(&receiver.timing, pending_capacity);
  audio_timing_set_format(&receiver.timing, &receiver.stream->format);

  receiver.buffered_listen_socket = -1;
  receiver.buffered_client_socket = -1;

  audio_receiver_reset_blocks();

  return ESP_OK;
}

void audio_receiver_set_format(const audio_format_t *format) {
  if (!format) {
    return;
  }
  if (!receiver.realtime_stream || !receiver.buffered_stream) {
    return;
  }

  receiver.realtime_stream->format = *format;
  receiver.buffered_stream->format = *format;

  ESP_LOGI(TAG,
           "Audio format: codec=%s sr=%d ch=%d bits=%d frame=%d maxspf=%lu",
           format->codec, format->sample_rate, format->channels,
           format->bits_per_sample, format->frame_size,
           (unsigned long)format->max_samples_per_frame);

  audio_decoder_destroy(receiver.decoder);
  receiver.decoder = NULL;

  audio_decoder_config_t cfg = {.format = *format};
  receiver.decoder = audio_decoder_create(&cfg);
  if (!receiver.decoder) {
    ESP_LOGW(TAG, "Decoder not initialized for codec: %s", format->codec);
  }

  audio_timing_set_format(&receiver.timing, format);
  audio_output_set_source_rate(format->sample_rate);
}

void audio_receiver_set_encryption(const audio_encrypt_t *encrypt) {
  if (!receiver.realtime_stream || !receiver.buffered_stream) {
    return;
  }
  if (encrypt) {
    receiver.realtime_stream->encrypt = *encrypt;
    receiver.buffered_stream->encrypt = *encrypt;
  } else {
    memset(&receiver.realtime_stream->encrypt, 0,
           sizeof(receiver.realtime_stream->encrypt));
    memset(&receiver.buffered_stream->encrypt, 0,
           sizeof(receiver.buffered_stream->encrypt));
  }
}

void audio_receiver_set_output_latency_us(uint32_t latency_us) {
  if (!receiver.stream) {
    return;
  }
  audio_timing_set_output_latency(&receiver.timing, &receiver.stream->format,
                                  latency_us);
}

uint32_t audio_receiver_get_output_latency_us(void) {
  return audio_timing_get_output_latency(&receiver.timing);
}

uint32_t audio_receiver_get_hardware_latency_us(void) {
  return audio_timing_get_hardware_latency();
}

void audio_receiver_set_anchor_time(uint64_t clock_id, uint64_t network_time_ns,
                                    uint32_t rtp_time) {
  if (!receiver.stream) {
    return;
  }

  // Detect a PTP master change.  AirPlay 2 senders include a non-zero
  // networkTimeTimelineID (clock_id) in SETRATEANCHORTIME / 0xD7 timing
  // packets.  When the sender switches (e.g. iPhone disconnects and Mac
  // takes over, or iPhone-mirroring-to-Mac creates a chained AirPlay session
  // with a new master) the clock_id changes.  Without resetting PTP here,
  // ptp_clock_get_offset_ns() still reflects the lock to the previous master
  // while anchor_network_time_ns now references the new master's PTP timeline
  // — compute_early_us then produces nonsense (observed: 511_452 seconds
  // early), which on the post_flush path triggers a runaway bulk-flush loop
  // and audio cuts out within half a second.
  if (clock_id != 0 && receiver.last_anchor_clock_id != 0 &&
      clock_id != receiver.last_anchor_clock_id) {
    ESP_LOGW(TAG,
             "PTP master changed (clock_id %llu -> %llu) — clearing PTP lock "
             "and invalidating anchor; falling back to local timing until "
             "re-lock",
             (unsigned long long)receiver.last_anchor_clock_id,
             (unsigned long long)clock_id);
    ptp_clock_clear();
    audio_buffer_flush(&receiver.buffer);
    audio_timing_reset(&receiver.timing);
    receiver.timing.post_flush = true;
    receiver.timing.post_flush_start_us = 0;
    receiver.arm_gate_on_next_anchor = false;
  }
  receiver.last_anchor_clock_id = clock_id;

  // Detect a seek where the buffer content is far displaced from the new
  // anchor position.  Threshold is 5 seconds of samples — large enough to
  // clear normal pre-buffer depth after a pause (typically 1-3 s), but
  // small enough to catch any real seek (which displaces by the full delta
  // from the current song position).  Both directions are checked:
  //   rtp_ahead > threshold  → backward seek (buffer ahead of new anchor)
  //   rtp_ahead < -threshold → forward seek (buffer behind new anchor)
  // Long-pause resume where the anchor advances over the pre-buffer is
  // handled by the bulk-flush path in audio_timing_read, but catching it
  // here avoids even the first DMA callback of silence.
  // Window size for the upper RTP gate: 10 s of samples.  Large enough that
  // a normal 2-4 s pre-buffer passes, but small enough to reject stale frames
  // left in the TCP socket buffer after a backward seek (which are 60+ s ahead
  // of the new anchor when seeking back to near the start of a track).
  int sample_rate = receiver.stream->format.sample_rate;
  if (sample_rate <= 0) {
    sample_rate = 44100;
  }
  const uint32_t gate_window = (uint32_t)(10 * sample_rate);

  uint32_t oldest_rtp = 0;
  if (audio_buffer_oldest_timestamp(&receiver.buffer, &oldest_rtp)) {
    int32_t rtp_ahead = (int32_t)(oldest_rtp - rtp_time);
    int32_t flush_threshold = 5 * sample_rate; // 5 seconds of samples
    int32_t abs_ahead = rtp_ahead < 0 ? -rtp_ahead : rtp_ahead;
    if (abs_ahead > flush_threshold) {
      ESP_LOGI(TAG,
               "Seek detected: oldest_rtp=%lu, new anchor rtp=%lu, "
               "delta=%ld samples (%.1f s) — cleaning stale packets",
               (unsigned long)oldest_rtp, (unsigned long)rtp_time,
               (long)rtp_ahead, (float)rtp_ahead / sample_rate);

      // Arm the pre-decode TCP-stale-skip window for any seek detected at
      // anchor-arrival time (covers the case where seek_flush wasn't
      // called — e.g. AirPlay 2 sometimes re-anchors without an explicit
      // FLUSHBUFFERED).  See seek_drain_until_us in
      // audio_receiver_internal.h for full rationale.
      receiver.seek_drain_until_us = esp_timer_get_time() + 3000000LL;
      audio_servo_start_seek_boost();

      int dropped = 0;
      uint32_t current_oldest = 0;
      while (audio_buffer_oldest_timestamp(&receiver.buffer, &current_oldest)) {
        int32_t diff = (int32_t)(current_oldest - rtp_time);
        // Keep packets that are close to the anchor (new track)
        if (diff > -flush_threshold && diff < (int32_t)gate_window) {
          break; // Hit the new packets, stop dropping
        }
        
        void *item = NULL;
        size_t item_size = 0;
        if (audio_buffer_take(&receiver.buffer, &item, &item_size, 0)) {
          audio_buffer_return(&receiver.buffer, item);
          dropped++;
        } else {
          break;
        }
      }
      
      if (dropped > 0) {
        ESP_LOGI(TAG, "Dropped %d stale frames from buffer", dropped);
      }
      
      // We don't want to reset playout state if we kept new frames.
      // But if we dropped everything, it's essentially a full flush.
      if (audio_buffer_get_frame_count(&receiver.buffer) == 0) {
        receiver.timing.playout_started = false;
        receiver.timing.pending_valid = false;
        receiver.timing.pending_frame_len = 0;
        receiver.timing.ready_time_us = 0;
        receiver.blocks_read_in_sequence = 0;
      }
      
      receiver.arm_gate_on_next_anchor = false; // already handled
    }
  }

  // Forward-seek path: seek_flush empties the buffer before the anchor
  // arrives, so the oldest_rtp check above never fires.
  if (receiver.arm_gate_on_next_anchor) {
    receiver.arm_gate_on_next_anchor = false;
  }

  audio_timing_set_anchor(&receiver.timing, &receiver.stream->format, clock_id,
                          network_time_ns, rtp_time);
}

void audio_receiver_set_playing(bool playing) {
  audio_timing_set_playing(&receiver.timing, playing);
  if (!playing) {
    receiver.blocks_read_in_sequence = 0;
  }
}

void audio_receiver_reset_timing(void) {
  audio_timing_reset(&receiver.timing);
}

bool audio_receiver_is_playing(void) {
  return receiver.timing.playing;
}

void audio_receiver_set_stream_type(audio_stream_type_t type) {
  if (!receiver.realtime_stream || !receiver.buffered_stream) {
    return;
  }
  audio_stream_t *target = receiver.realtime_stream;
  if (type == AUDIO_STREAM_BUFFERED) {
    target = receiver.buffered_stream;
  }

  if (!target) {
    return;
  }

  if (receiver.stream != target) {
    if (receiver.stream) {
      audio_receiver_copy_stream_state(target, receiver.stream);
      if (receiver.stream->running && receiver.stream->ops &&
          receiver.stream->ops->stop) {
        receiver.stream->ops->stop(receiver.stream);
      }
    }
    receiver.stream = target;
  }

  receiver.stream->type = type;
}

esp_err_t audio_receiver_start(uint16_t data_port, uint16_t control_port) {
  audio_receiver_set_stream_type(AUDIO_STREAM_REALTIME);

  if (!receiver.stream || !receiver.stream->ops ||
      !receiver.stream->ops->start) {
    return ESP_FAIL;
  }

  // Always stop and restart fresh
  if (receiver.stream->running) {
    receiver.stream->ops->stop(receiver.stream);
  }

  receiver.data_port = data_port;
  receiver.control_port = control_port;

  // Starting a stream resets all timing state (including pause tracking)
  audio_receiver_reset_stats();
  audio_buffer_flush(&receiver.buffer);
  audio_timing_reset(&receiver.timing);

  receiver.timing.ptp_locked = ptp_clock_is_locked();
  audio_receiver_reset_blocks();

  return receiver.stream->ops->start(receiver.stream, data_port);
}

esp_err_t audio_receiver_start_buffered(uint16_t tcp_port) {
  audio_receiver_set_stream_type(AUDIO_STREAM_BUFFERED);

  if (!receiver.stream || !receiver.stream->ops ||
      !receiver.stream->ops->start) {
    return ESP_FAIL;
  }

  // Buffered streams use a fixed port, no need to restart if running
  if (receiver.stream->running) {
    return ESP_OK;
  }

  // Starting a stream resets all timing state (including pause tracking)
  audio_receiver_reset_stats();
  audio_buffer_flush(&receiver.buffer);
  audio_timing_reset(&receiver.timing);

  receiver.timing.ptp_locked = ptp_clock_is_locked();
  audio_receiver_reset_blocks();

  return receiver.stream->ops->start(receiver.stream, tcp_port);
}

esp_err_t audio_receiver_start_stream(uint16_t data_port, uint16_t control_port,
                                      uint16_t tcp_port) {
  if (!receiver.stream) {
    return ESP_FAIL;
  }
  if (receiver.stream->type == AUDIO_STREAM_BUFFERED) {
    return audio_receiver_start_buffered(tcp_port);
  }

  return audio_receiver_start(data_port, control_port);
}

uint16_t audio_receiver_get_stream_port(void) {
  if (!receiver.stream || !receiver.stream->ops ||
      !receiver.stream->ops->get_port) {
    return 0;
  }

  return receiver.stream->ops->get_port(receiver.stream);
}

void audio_receiver_set_client_control(uint32_t client_ip,
                                       uint16_t client_control_port) {
  if (client_ip == 0 || client_control_port == 0) {
    receiver.retransmit_enabled = false;
    return;
  }
  memset(&receiver.client_control_addr, 0,
         sizeof(receiver.client_control_addr));
  receiver.client_control_addr.sin_family = AF_INET;
  receiver.client_control_addr.sin_addr.s_addr = client_ip;
  receiver.client_control_addr.sin_port = htons(client_control_port);
  receiver.retransmit_enabled = true;
  receiver.last_resend_error_time_us = 0;
  ESP_LOGI(TAG, "NACK retransmission enabled, client control port %u",
           client_control_port);
}

void audio_receiver_stop(void) {
  if (receiver.realtime_stream && receiver.realtime_stream->ops &&
      receiver.realtime_stream->ops->stop) {
    receiver.realtime_stream->ops->stop(receiver.realtime_stream);
  }

  if (receiver.buffered_stream && receiver.buffered_stream->ops &&
      receiver.buffered_stream->ops->stop) {
    receiver.buffered_stream->ops->stop(receiver.buffered_stream);
  }

  audio_decoder_destroy(receiver.decoder);
  receiver.decoder = NULL;

  if (receiver.realtime_stream) {
    memset(&receiver.realtime_stream->encrypt, 0,
           sizeof(receiver.realtime_stream->encrypt));
  }
  if (receiver.buffered_stream) {
    memset(&receiver.buffered_stream->encrypt, 0,
           sizeof(receiver.buffered_stream->encrypt));
  }

  receiver.retransmit_enabled = false;
  memset(&receiver.client_control_addr, 0,
         sizeof(receiver.client_control_addr));

  audio_receiver_flush();
}

void audio_receiver_stop_buffered_only(void) {
  if (receiver.buffered_stream && receiver.buffered_stream->ops &&
      receiver.buffered_stream->ops->stop) {
    receiver.buffered_stream->ops->stop(receiver.buffered_stream);
  }
}

void audio_receiver_get_stats(audio_stats_t *stats) {
  if (!stats) {
    return;
  }
  memcpy(stats, &receiver.stats, sizeof(receiver.stats));
}

size_t audio_receiver_read(int16_t *buffer, size_t samples) {
  if (!receiver.buffer.pool || !buffer || samples == 0) {
    return 0;
  }

  return audio_timing_read(&receiver.timing, &receiver.buffer, receiver.stream,
                           &receiver.stats, buffer, samples);
}

bool audio_receiver_has_data(void) {
  int buffered_frames = audio_buffer_get_frame_count(&receiver.buffer);
  return buffered_frames > 0 || receiver.timing.pending_valid;
}

void audio_receiver_flush(void) {
  // Flush is an explicit reset — clear all timing state including pause
  // tracking.  The sender will provide fresh anchor times after flush.
  // Also disarm any pending deferred flush so it does not fire on the
  // next track's frames.
  audio_buffer_flush(&receiver.buffer);
  audio_timing_reset(&receiver.timing);

  receiver.arm_gate_on_next_anchor = false;
  receiver.blocks_read_in_sequence = 1;
}

void audio_receiver_seek_flush(void) {
  // Mid-stream seek flush (FLUSH / immediate FLUSHBUFFERED).  Like
  // audio_receiver_flush() but sets timing.post_flush so audio_timing_read
  // plays frames immediately after the seek instead of silencing them while
  // the anchor's pre-buffer window (several seconds) elapses.
  // Also disarms any pending deferred flush (audio_timing_reset clears it).
  audio_receiver_flush();
  receiver.timing.post_flush = true;
  receiver.timing.post_flush_start_us = 0; // will be set on first frame
  // Flush the I2S DMA ring buffer so old pre-seek audio doesn't play out
  // as audible static/ramp while new-position frames are still in flight.
  audio_output_flush();
  // Reset the servo so it starts from 0 ppm instead of ramping from the
  // pre-seek correction (e.g. -250 ppm -> boosted positive ppm), which is
  // audible as a pitch "swoop" during the first ~1.6 s after a seek.
  audio_servo_init();
  audio_servo_start_seek_boost();
  // Request that the RTP gate be armed as soon as the next anchor arrives.
  // This covers the forward-seek case where the buffer is already empty by
  // the time SETRATEANCHORTIME arrives, so the seek-detection heuristic
  // (which needs oldest_rtp from the buffer) would otherwise miss arming it.
  receiver.arm_gate_on_next_anchor = true;
  // Arm the pre-decode TCP-stale-skip window.  See seek_drain_until_us
  // in audio_receiver_internal.h for rationale.  3 s covers normal TCP
  // drain; expires automatically so it can't kill steady-state playback.
  receiver.seek_drain_until_us = esp_timer_get_time() + 3000000LL;
}

void audio_receiver_set_deferred_flush(uint32_t flush_until_ts) {
  if (!receiver.stream) {
    return;
  }

  // Idempotency guard for duplicate FLUSHBUFFERED (round-3 fix).
  //
  // iPhone Apple Music sometimes sends two or three FLUSHBUFFERED deferred
  // packets in quick succession for the same seek.  The flush_until_ts values
  // in the second and third packets are often EARLIER (smaller) than the first
  // — they represent the overlap region rather than the final boundary.
  // Blindly overwriting flush_until_ts with an earlier value would trigger
  // the deferred flush prematurely, cutting off the last few seconds of the
  // current track and producing a brief silence glitch at the transition.
  //
  // Rule: if a deferred flush is already pending, only replace it when the new
  // flush_until_ts is LATER (signed 32-bit comparison handles RTP wrap).
  // If it is equal or earlier, silently ignore the duplicate.  The existing
  // boundary is conservative (fires later) which is always safe — we play a
  // little more of the old track, then the deferred flush fires, and the new
  // track starts cleanly.
  //
  // Exception: if the caller is re-arming after an immediate seek_flush
  // (deferred_flush_pending is false), the check doesn't apply — just arm.
  if (receiver.timing.deferred_flush_pending) {
    int32_t delta =
        (int32_t)(flush_until_ts - receiver.timing.flush_until_ts);
    if (delta <= 0) {
      ESP_LOGI(TAG,
               "Deferred flush duplicate ignored: new_ts=%" PRIu32
               " <= existing_ts=%" PRIu32,
               flush_until_ts, receiver.timing.flush_until_ts);
      return;
    }
    ESP_LOGI(TAG,
             "Deferred flush extended: %" PRIu32 " -> %" PRIu32,
             receiver.timing.flush_until_ts, flush_until_ts);
  }

  // Write flush_until_ts before arming the flag so audio_timing_read never
  // sees deferred_flush_pending=true with a stale timestamp.
  receiver.timing.flush_until_ts = flush_until_ts;
  receiver.timing.deferred_flush_pending = true;
  ESP_LOGI(TAG, "Deferred flush armed: flush_until_ts=%" PRIu32,
           flush_until_ts);
}

void audio_receiver_pause(void) {
  // Stop the consumer.  The receiver tasks keep running so the audio buffer
  // continues to fill with pre-buffered audio — TCP back-pressure naturally
  // throttles the sender.  On resume the phone sends a fresh
  // SETRATEANCHORTIME anchor that re-aligns the buffered frames to the
  // correct wall-clock position; no flush or offset compensation is needed.
  audio_timing_set_playing(&receiver.timing, false);
  receiver.blocks_read_in_sequence = 0;
}

uint16_t audio_receiver_get_buffered_port(void) {
  return receiver.buffered_port;
}

audio_stream_type_t audio_receiver_get_active_stream_type(void) {
  if (!receiver.stream || !receiver.stream->running) {
    return AUDIO_STREAM_NONE;
  }
  return receiver.stream->type;
}

void audio_receiver_drain_drift(int32_t *min_us, int32_t *max_us,
                                uint32_t *samples) {
  audio_timing_drain_drift(&receiver.timing, min_us, max_us, samples);
}

int32_t audio_receiver_get_smoothed_drift_us(void) {
  return audio_timing_get_smoothed_drift_us(&receiver.timing);
}

bool audio_receiver_setpeers_is_new(const uint8_t *body, size_t body_len) {
  // Treat missing / empty body as "new" so the caller logs and proceeds.
  if (!body || body_len == 0) {
    return true;
  }

  // FNV-1a 32-bit hash over the raw body bytes.
  // Chosen for simplicity — not cryptographic, just collision-resistant
  // enough to distinguish different peer list payloads.
  uint32_t hash = 2166136261u; // FNV offset basis
  for (size_t i = 0; i < body_len; i++) {
    hash ^= (uint32_t)body[i];
    hash *= 16777619u; // FNV prime
  }

  if (hash == receiver.last_setpeers_hash) {
    return false; // Duplicate — suppress re-lock disturbance
  }
  receiver.last_setpeers_hash = hash;
  return true; // New peer list
}

void audio_receiver_drain_buffer_depth(uint32_t *samples, uint32_t *min_out,
                                       uint32_t *max_out, uint32_t *avg_out) {
  audio_buffer_depth_stats_t s;
  audio_buffer_drain_depth_stats(&receiver.buffer, &s);
  if (samples) {
    *samples = s.samples;
  }
  if (min_out) {
    *min_out = s.samples ? s.min : 0;
  }
  if (max_out) {
    *max_out = s.max;
  }
  if (avg_out) {
    *avg_out = s.samples ? (s.sum / s.samples) : 0;
  }
}
