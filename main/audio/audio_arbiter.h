#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Decides which source owns the speaker.
 *
 * Policy (the user's): AirPlay is absolute while a sender is connected —
 * including while it is paused or idle.  The USB speaker path gets the
 * output only when no AirPlay session exists, and only after a short grace
 * period following a disconnect so that a sender switching tracks/devices
 * (which can look like disconnect + reconnect) does not bounce the output.
 *
 * Inputs: RTSP session events.  Output: audio_output_select_source() and
 * usb_audio_source_set_active().  USB activity is deliberately NOT an input:
 * Windows streams silence to a selected output device forever, so "USB is
 * playing" carries no information.
 */

#define AIRPLAY_RELEASE_GRACE_MS 3000

typedef struct {
  bool airplay_connected;
  bool airplay_playing;
  bool release_pending; // grace timer running
  uint32_t hand_overs;  // total source changes decided
  int64_t last_change_us;
} audio_arbiter_state_t;

esp_err_t audio_arbiter_init(bool usb_available);
void audio_arbiter_get_state(audio_arbiter_state_t *out);
