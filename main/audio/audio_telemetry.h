#pragma once

#include "esp_err.h"

/**
 * Periodic AirPlay realtime-path telemetry.
 *
 * Once started, a background task logs a single-line summary every
 * AUDIO_TELEMETRY_INTERVAL_MS containing: stream type, packets/decoded/dropped,
 * sequence gaps, NACKs, retransmits, RFC3550 jitter, RTP ts delta,
 * buffer depth (min/max/avg, underruns, late frames, deferred-flush state),
 * I2S write cadence (avg/max us, silence-write count) and PTP
 * (locked/lock-time, offset, max deviation).
 *
 * The first AirPlay session in a process triggers logging.  No-op when no
 * stream is active.  Counters are reset on stream start/teardown by the
 * existing audio_receiver_reset_stats path; the telemetry task computes
 * deltas internally so the output is per-window, not lifetime.
 */
esp_err_t audio_telemetry_start(void);
