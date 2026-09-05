#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Start the AirPlay RTSP server on port 7000
 * Handles initial connection requests from iOS devices
 */
esp_err_t rtsp_server_start(void);

/**
 * Stop the RTSP server
 */
void rtsp_server_stop(void);

/**
 * Set volume from AirPlay (in dB, range -144 to 0)
 * @param volume_db Volume in dB (0 = max, -144 = mute)
 */
void airplay_set_volume(float volume_db);

/**
 * Get current volume as Q15 scale factor for audio processing
 * @return Q15 fixed-point multiplier (0 = mute, 32768 = unity)
 */
int32_t airplay_get_volume_q15(void);

/**
 * Request resume during the AirPlay v1 grace period.
 * Called from the play/pause button when the source is still AirPlay
 * but the RTSP connection has been torn down (paused). Sends a DACP
 * playpause to the phone so it reconnects.
 */
void rtsp_server_request_resume(void);

/** Snapshot of the active AirPlay session for the status API. */
typedef struct {
  uint32_t sid;
  char source_id[40];
  char source_name[48];
  char source_model[32];
  float volume_db;
  bool stream_active;
  bool stream_paused;
  uint8_t protocol_version;
  int64_t connected_us;
} rtsp_session_info_t;

/** Fill `out` for the current client; returns false if none is connected. */
bool rtsp_server_get_session_info(rtsp_session_info_t *out);
