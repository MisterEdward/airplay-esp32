#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_buffer.h"
#include "audio_decoder.h"
#include "audio_receiver.h"
#include "audio_stream.h"
#include "audio_timing.h"
#include "spiram_task.h"

#define MAX_RTP_PACKET_SIZE 2048

typedef struct {
  audio_stream_t *stream;
  audio_stream_t *realtime_stream;
  audio_stream_t *buffered_stream;

  audio_decoder_t *decoder;
  audio_buffer_t buffer;
  audio_timing_t timing;

  audio_stats_t stats;

  int data_socket;
  int control_socket;
  TaskHandle_t task_handle;
  TaskHandle_t control_task_handle;
  uint16_t data_port;
  uint16_t control_port;

  int buffered_listen_socket;
  int buffered_client_socket;
  uint16_t buffered_port;
  TaskHandle_t buffered_task_handle;
  spiram_task_mem_t buffered_task_mem;
  uint8_t *buffered_recv_buffer;

  uint8_t *decrypt_buffer;
  size_t decrypt_buffer_size;

  uint64_t blocks_read;
  uint64_t blocks_read_in_sequence;

  // NACK retransmission support
  struct sockaddr_in client_control_addr; // Client's control address for NACKs
  bool retransmit_enabled;                // True when client address is set
  int64_t last_resend_error_time_us;      // Backoff timer on sendto failure

  // RFC 3550 inter-arrival jitter tracking (realtime UDP path).
  // last_arrival_local_us: esp_timer time when previous in-order packet arrived.
  // last_arrival_rtp_ts:   RTP timestamp of that previous packet.
  // jitter_initialised:    false until two consecutive packets observed.
  int64_t last_arrival_local_us;
  uint32_t last_arrival_rtp_ts;
  bool jitter_initialised;

  // PTP master tracking: clock_id from the most recent anchor.  When a new
  // anchor arrives with a different clock_id (sender switched, e.g. iPhone ->
  // Mac), the old PTP offset is stale relative to the new master's clock and
  // compute_early_us produces nonsense (anchor_network_time_ns from the new
  // master minus offset locked to the old master).  Detecting the change here
  // lets us force ptp_clock_clear() so the slave re-locks to the new master,
  // and during the re-lock window we fall back to SYNC_MODE_NONE (local
  // anchor_local_time_ns) which is always correct.  Zero is reserved for
  // AirPlay 1 NTP-style anchors that don't carry a clock_id.
  uint64_t last_anchor_clock_id;

  // Set by audio_receiver_seek_flush() to ensure the gates are armed on the
  // next SETRATEANCHORTIME even when the buffer was already empty (forward
  // seek: flush empties buffer before anchor arrives, so seek detection in
  // set_anchor_time would otherwise find no oldest_rtp and skip arming).
  bool arm_gate_on_next_anchor;

  // Pre-decode TCP-stale-skip deadline.  Set to esp_timer_get_time() + 3 s
  // by audio_receiver_seek_flush() and by the seek path in
  // audio_receiver_set_anchor_time().  While now() < seek_drain_until_us,
  // the buffered TCP task discards packets whose RTP is far outside the
  // keep window around the new anchor — this drains the OS TCP socket
  // backlog (several seconds of pre-flush audio) at network speed instead
  // of decoder speed.  Time-bounded rather than tied to timing.post_flush
  // because post_flush can persist much longer than the actual drain (it
  // only exits when 10 consecutive frames land within ±60 ms; if the
  // post-seek steady-state drift settles outside that band, post_flush
  // would stay true forever and the static `anchor_rtp + 15 s` window
  // would eventually reject all real-time packets as wall clock advances,
  // killing playback dead a few seconds after every seek).  3 s is well
  // above any plausible TCP drain time (sender pre-buffer ≤ ~10 s
  // delivered at multi-x real-time over WiFi) and short enough that a
  // run-on into normal-play packets is harmless.
  int64_t seek_drain_until_us;

  // SETPEERS dedup (round-3 fix): 32-bit FNV-1a hash of the most-recently
  // accepted SETPEERS bplist body.  When a new SETPEERS arrives with the
  // same hash, it is silently acked without disturbing the PTP layer.
  // iPhone Apple Music spams 3-4 SETPEERS in quick succession during a seek;
  // without dedup each one is logged as "PTP peers changed" and delays PTP
  // convergence diagnostics.  Zero means "no previous peer list stored".
  uint32_t last_setpeers_hash;
} audio_receiver_state_t;

bool audio_stream_process_frame(audio_receiver_state_t *state,
                                uint32_t timestamp, const uint8_t *audio_data,
                                size_t audio_len);

static inline audio_receiver_state_t *
audio_stream_state(audio_stream_t *stream) {
  return (audio_receiver_state_t *)stream->ctx;
}
