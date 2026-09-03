#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lightweight, one-shot AirPlay lifecycle metrics. The audio hot paths only
// take the lock while a first-PCM/first-output marker is pending.

uint32_t airplay_metrics_connection_open(uint32_t client_ip,
                                         int64_t *connected_at_us);
void airplay_metrics_initial_setup(uint32_t session_id);
void airplay_metrics_stream_setup(uint32_t session_id, int64_t connected_at_us,
                                  uint8_t protocol, int64_t stream_type,
                                  bool buffered);
void airplay_metrics_pause(void);
void airplay_metrics_resume(void);
void airplay_metrics_seek_immediate(void);
void airplay_metrics_deferred_flush_requested(uint32_t from_seq,
                                              uint32_t from_ts,
                                              uint32_t until_seq,
                                              uint32_t until_ts);
void airplay_metrics_deferred_flush_applied(uint32_t boundary_ts);
void airplay_metrics_first_pcm(uint32_t rtp_timestamp, size_t samples);
void airplay_metrics_first_output(size_t frames);
void airplay_metrics_stream_end(uint32_t session_id, const char *reason);
void airplay_metrics_connection_closed(uint32_t session_id,
                                       int64_t connected_at_us,
                                       const char *reason);
