#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Audio receiver for AirPlay RTP streams
 * Handles UDP packet reception and audio decoding
 */

// Audio format info from ANNOUNCE SDP
typedef struct {
  char codec[32];      // "AppleLossless", "AAC", etc.
  int sample_rate;     // 44100, 48000, etc.
  int channels;        // 1 or 2
  int bits_per_sample; // 16, 24
  int frame_size;      // Samples per frame (ALAC: 352)

  // ALAC-specific config (from fmtp line)
  uint32_t max_samples_per_frame;
  uint8_t sample_size;
  uint8_t rice_history_mult;
  uint8_t rice_initial_history;
  uint8_t rice_limit;
  uint8_t num_channels;
  uint16_t max_run;
  uint32_t max_coded_frame_size;
  uint32_t avg_bit_rate;
  uint32_t sample_rate_config;
} audio_format_t;

// Audio encryption types
typedef enum {
  AUDIO_ENCRYPT_NONE = 0,
  AUDIO_ENCRYPT_AES_CBC,
  AUDIO_ENCRYPT_CHACHA20_POLY1305
} audio_encrypt_type_t;

// Audio encryption configuration
typedef struct {
  audio_encrypt_type_t type;
  uint8_t key[32]; // AES-128 uses 16, ChaCha20 uses 32
  uint8_t iv[16];  // AES-CBC IV
  size_t key_len;
} audio_encrypt_t;

// Audio buffer statistics
typedef struct {
  uint32_t packets_received;
  uint32_t packets_decoded;
  uint32_t packets_dropped;
  uint32_t decrypt_errors;
  uint32_t buffer_underruns;
  uint32_t buffer_overruns;
  uint32_t late_frames;
  uint16_t last_seq;
  uint32_t last_timestamp;
} audio_stats_t;

// Read-only seek diagnostics used by the WebSocket log telemetry task.
typedef struct {
  uint32_t generation;
  int64_t seek_started_us;
  int64_t anchor_received_us;
  int64_t first_rx_us;
  int64_t first_queue_us;
  int64_t playout_started_us;
  uint32_t anchor_rtp;
  uint32_t first_rx_rtp;
  uint32_t first_queue_rtp;
  uint32_t blanket_drops;
  uint32_t lower_gate_drops;
  uint32_t upper_gate_drops;
  uint32_t buffer_frames;
  uint32_t target_buffer_frames;
  bool discard_all_until_anchor;
  bool lower_gate_armed;
  bool upper_gate_armed;
  bool anchor_valid;
  bool quick_start;
  bool pending_valid;
  bool playout_started;
  bool playing;
  bool stream_running;
  bool buffered_client_connected;
  uint8_t stream_type;
  int64_t buffered_last_packet_us;
  uint32_t buffered_connections_accepted;
  uint32_t buffered_stall_recoveries;
  uint32_t buffered_reader_stack_words;
  uint32_t buffered_decoder_stack_words;
  uint32_t deferred_flush_active;
  uint32_t deferred_flush_armed;
  uint32_t deferred_flush_duplicates;
  uint32_t deferred_flush_dropped;
  uint32_t deferred_flush_expired;
  uint32_t deferred_flush_overflow;
  audio_stats_t stats;
} audio_seek_diag_t;

/**
 * Initialize audio receiver
 */
esp_err_t audio_receiver_init(void);

/**
 * Set audio format from ANNOUNCE SDP
 */
void audio_receiver_set_format(const audio_format_t *format);

/**
 * Set encryption parameters for RTP decryption
 */
void audio_receiver_set_encryption(const audio_encrypt_t *encrypt);

/**
 * Start receiving audio on specified port
 */
esp_err_t audio_receiver_start(uint16_t data_port, uint16_t control_port);

/**
 * Start the active stream type using the provided ports.
 */
esp_err_t audio_receiver_start_stream(uint16_t data_port, uint16_t control_port,
                                      uint16_t tcp_port);

/**
 * Stop receiving audio
 */
void audio_receiver_stop(void);

/**
 * Get audio statistics
 */
void audio_receiver_get_stats(audio_stats_t *stats);

/** Take a read-only snapshot of seek and buffer diagnostics. */
void audio_receiver_get_seek_diag(audio_seek_diag_t *diag);

/**
 * Read decoded PCM samples from buffer
 * @param buffer Output buffer for PCM samples (interleaved stereo, 16-bit)
 * @param samples Maximum number of samples to read (per channel)
 * @return Number of samples actually read
 */
size_t audio_receiver_read(int16_t *buffer, size_t samples);

/**
 * Check if audio data is available
 */
bool audio_receiver_has_data(void);

/**
 * Flush audio buffer (full stop path — used by TEARDOWN and stop).
 */
void audio_receiver_flush(void);

/**
 * Flush audio buffer for a mid-stream seek (FLUSH / immediate FLUSHBUFFERED).
 * Identical to audio_receiver_flush() but also sets timing.post_flush so
 * that audio_timing_read plays frames immediately after the seek instead of
 * silencing them during the phone's pre-buffer window (which can be several
 * seconds with AirPlay 2 buffered streams).  Mirrors shairport-sync's
 * first_packet_timestamp==0 behaviour: post-flush frames play unconditionally
 * until the anchor reports on-time (early_us < TIMING_THRESHOLD_US).
 */
void audio_receiver_seek_flush(void);

/**
 * Add an AirPlay 2 deferred FLUSHBUFFERED packet range. Multiple overlapping
 * requests are retained because Apple Music Audio Mix sends them in bursts.
 */
bool audio_receiver_set_deferred_flush(uint32_t flush_from_seq,
                                       uint32_t flush_from_ts,
                                       uint32_t flush_until_seq,
                                       uint32_t flush_until_ts);

/**
 * Pause playback while preserving the timing anchor.
 * Flushes the audio buffer and resets playback-start state, but does NOT
 * call audio_timing_reset() so the anchor remains valid.  The pause start
 * time is recorded so that audio_receiver_set_playing(true) can compensate
 * for the pause duration on resume.
 */
void audio_receiver_pause(void);

/**
 * Set advertised/target output latency in microseconds.
 */
void audio_receiver_set_output_latency_us(uint32_t latency_us);

/**
 * Get current output latency in microseconds (buffer latency only).
 */
uint32_t audio_receiver_get_output_latency_us(void);

/**
 * Get hardware output latency in microseconds (I2S DMA pipeline delay).
 */
uint32_t audio_receiver_get_hardware_latency_us(void);

/**
 * Get total advertised latency in microseconds.  Includes the jitter-buffer
 * target depth, hardware DMA delay, and fixed decrypt/decode/network
 * pipeline constant.  Report this in outputLatencyMicros so the phone
 * schedules sends to match our actual end-to-end depth.
 */
uint32_t audio_receiver_get_advertised_latency_us(void);

/**
 * Provide anchor timing information from SETRATEANCHORTIME.
 * @param clock_id PTP clock ID (networkTimeTimelineID)
 * @param network_time_ns Anchor time in nanoseconds (PTP timeline)
 * @param rtp_time RTP timestamp for the anchor
 */
void audio_receiver_set_anchor_time(uint64_t clock_id, uint64_t network_time_ns,
                                    uint32_t rtp_time);

/**
 * Enable or pause playout scheduling.
 */
void audio_receiver_set_playing(bool playing);

/**
 * Check if playback is currently active (not paused).
 */
bool audio_receiver_is_playing(void);

/**
 * Reset timing anchor (call when PTP clock changes, e.g., SETPEERS)
 */
void audio_receiver_reset_timing(void);

/**
 * Set the client's control address for NACK retransmission requests.
 * @param client_ip Client IP in network byte order
 * @param client_control_port Client's control port (host byte order)
 */
void audio_receiver_set_client_control(uint32_t client_ip,
                                       uint16_t client_control_port);

/**
 * Stream types for AirPlay 2
 */
typedef enum {
  AUDIO_STREAM_NONE = 0,
  AUDIO_STREAM_REALTIME = 96, // UDP, ALAC
  AUDIO_STREAM_BUFFERED = 103 // TCP, AAC-ELD
} audio_stream_type_t;

/**
 * Start buffered audio receiver (type=103) on TCP port
 * @param tcp_port Port to listen on for TCP connections
 * @return ESP_OK on success
 */
esp_err_t audio_receiver_start_buffered(uint16_t tcp_port);

/**
 * Get the active stream port (data or buffered).
 */
uint16_t audio_receiver_get_stream_port(void);

/**
 * Get the TCP port for buffered audio (after start_buffered)
 */
uint16_t audio_receiver_get_buffered_port(void);

/**
 * Stop only the buffered receiver but keep playing buffered data
 * Used when TEARDOWN with streams array is received (sender done sending)
 */
void audio_receiver_stop_buffered_only(void);

/**
 * Recovery-ladder rung 2: stop and restart the active buffered stream on the
 * same TCP port so the sender can reconnect without a new SETUP. Returns
 * ESP_ERR_INVALID_STATE when no buffered stream is running.
 */
esp_err_t audio_receiver_restart_buffered_stream(void);

/**
 * Set the stream type (realtime vs buffered)
 */
void audio_receiver_set_stream_type(audio_stream_type_t type);
