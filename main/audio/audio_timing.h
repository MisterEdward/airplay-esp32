#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_buffer.h"
#include "audio_receiver.h"
#include "audio_stream.h"

#define AUDIO_MAX_DEFERRED_FLUSH_REQUESTS 8

typedef struct {
  bool in_use;
  uint32_t from_seq;
  uint32_t from_ts;
  uint32_t until_seq;
  uint32_t until_ts;
  int64_t expires_us;
} audio_deferred_flush_request_t;

typedef struct {
  uint32_t output_latency_us;
  uint32_t target_buffer_frames;
  uint32_t nominal_frame_samples;
  bool playout_started;
  bool playing;
  bool anchor_valid;
  uint64_t anchor_network_time_ns;
  uint32_t anchor_rtp_time;
  int64_t anchor_local_time_ns;
  int64_t ready_time_us; // When buffer became ready (0 = not ready yet)
  bool ptp_locked;
  uint8_t *pending_frame;
  size_t pending_frame_len;
  size_t pending_frame_capacity;
  bool pending_valid;
  // Early-frame guard: counts consecutive early frames to detect a stuck
  // anchor. Reset whenever a new anchor is set or a late/on-time frame is
  // played.
  int consecutive_early_frames;
  // Late-frame guard: counts consecutive individually-late frames.  When this
  // Quick-start flag: set after a seek/flush/track-change so that
  // audio_timing_read starts playback with just 1 buffered frame instead of
  // waiting for target_buffer_frames.  Anchor-based timing is used from the
  // very first frame (no bypass) — early frames are held as pending and
  // silence is output until their scheduled play time, exactly like
  // shairport-sync.  Cleared once playout_started becomes true.
  bool quick_start;
  // Apple Music Audio Mix can arm several overlapping deferred flush ranges.
  // Packet sequence is authoritative because RTP timestamps may jump between
  // the old and new tracks. The RTSP, decoder and playback tasks share this
  // table under a short critical section.
  portMUX_TYPE deferred_flush_lock;
  audio_deferred_flush_request_t
      deferred_flush[AUDIO_MAX_DEFERRED_FLUSH_REQUESTS];
  uint32_t deferred_flush_armed;
  uint32_t deferred_flush_duplicates;
  uint32_t deferred_flush_dropped;
  uint32_t deferred_flush_expired;
  uint32_t deferred_flush_overflow;
} audio_timing_t;

void audio_timing_init(audio_timing_t *timing, size_t pending_capacity);
void audio_timing_reset(audio_timing_t *timing);
void audio_timing_set_format(audio_timing_t *timing,
                             const audio_format_t *format);
void audio_timing_set_output_latency(audio_timing_t *timing,
                                     const audio_format_t *format,
                                     uint32_t latency_us);
uint32_t audio_timing_get_output_latency(const audio_timing_t *timing);
uint32_t audio_timing_get_hardware_latency(void);
// Total advertised latency (output + HW + fixed pipeline processing).
// Use this for outputLatencyMicros, NOT (output + HW) alone — otherwise the
// phone schedules sends 15 ms tight and the fill controller will pad
// silence to compensate for the missing decode/decrypt/net delay.
uint32_t audio_timing_get_advertised_latency(const audio_timing_t *timing);
void audio_timing_set_anchor(audio_timing_t *timing,
                             const audio_format_t *format, uint64_t clock_id,
                             uint64_t network_time_ns, uint32_t rtp_time);
void audio_timing_set_playing(audio_timing_t *timing, bool playing);
bool audio_timing_add_deferred_flush(audio_timing_t *timing,
                                     uint32_t from_seq, uint32_t from_ts,
                                     uint32_t until_seq, uint32_t until_ts);
void audio_timing_reset_deferred_flushes(audio_timing_t *timing);
bool audio_timing_deferred_flush_contains_sequence(audio_timing_t *timing,
                                                   uint32_t sequence_number);
size_t audio_timing_read(audio_timing_t *timing, audio_buffer_t *buffer,
                         const audio_stream_t *stream, audio_stats_t *stats,
                         int16_t *out, size_t samples);
