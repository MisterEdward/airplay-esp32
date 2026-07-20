#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * Conservative recovery ladder for the buffered AirPlay audio path.
 *
 * Escalation order (rung 1, the 8-second socket timeout, lives inside
 * audio_stream_buffered.c and is not driven from here):
 *
 *   2. restart the active buffered stream when task liveness is lost or the
 *      stream is stuck while a sender is connected and playing;
 *   3. restart the RTSP service after repeated stream restarts fail;
 *   4. restart the ESP only after repeated service restarts fail.
 *
 * No action is taken for late frames, seeks, PTP acquisition, isolated
 * drops or a sender that simply went away (idle listener is healthy).
 */

typedef struct {
  uint8_t level;                // highest rung reached since last full health
  uint32_t stream_restarts;     // cumulative rung-2 actions
  uint32_t service_restarts;    // cumulative rung-3 actions
  uint32_t task_loss_events;    // rung-2 triggers from a missing audio task
  uint32_t stuck_stream_events; // rung-2 triggers from a stuck live stream
} audio_recovery_stats_t;

/** Start the recovery supervisor task. */
esp_err_t audio_recovery_start(void);

/** Read-only counters for the health telemetry line. */
void audio_recovery_get_stats(audio_recovery_stats_t *stats);
