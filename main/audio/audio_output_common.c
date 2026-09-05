/**
 * @file audio_output_common.c
 * @brief Weak defaults for the optional half of the audio output API.
 *
 * Exactly one backend (audio_output.c, _spdif.c, _usb.c, ...) is compiled in,
 * chosen by Kconfig. Callers such as web_server.c and audio_timing.c reference
 * the full API unconditionally, so a backend that does not implement the
 * capability calls used to fail the link — which is why only the I2S build,
 * the one the CI matrix covers, kept working.
 *
 * The defaults below describe a backend with no channel routing and no
 * hardware completion cursor. A backend overrides one by defining it, the
 * same way boards override iot_board_*() in board_common.c. Only genuinely
 * optional entry points belong here: the core ones (init/start/write/...) are
 * deliberately left undefined so a backend missing them still fails loudly.
 */

#include "audio_output.h"
#include "audio_receiver.h"

#include <string.h>

__attribute__((weak)) bool audio_output_get_pipeline_us(int64_t *now_us,
                                                        uint32_t *pipeline_us) {
  (void)now_us;
  (void)pipeline_us;
  // No completion cursor: the timing engine falls back to the modelled
  // hardware latency.
  return false;
}

__attribute__((weak)) uint32_t audio_output_get_underruns(void) {
  return 0;
}

__attribute__((weak)) audio_channel_mode_t
audio_output_cycle_channel_mode(void) {
  return AUDIO_CHANNEL_STEREO;
}

__attribute__((weak)) void
audio_output_set_channel_mode(audio_channel_mode_t mode) {
  (void)mode;
}

__attribute__((weak)) audio_channel_mode_t audio_output_get_channel_mode(void) {
  return AUDIO_CHANNEL_STEREO;
}

// Routing is fixed at stereo, which is what "locked" reports to the web UI so
// it renders the control as unavailable rather than as a working toggle.
__attribute__((weak)) bool audio_output_channel_mode_locked(void) {
  return true;
}

__attribute__((weak)) bool audio_output_channel_mode_in_dsp(void) {
  return false;
}

/* Transition shaping and source arbitration are implemented by the I2S
 * backend.  Other backends get inert defaults so the control plane links. */
__attribute__((weak)) void audio_output_pause(void) {
  audio_receiver_pause();
}

__attribute__((weak)) void audio_output_resume(void) {
}

__attribute__((weak)) void
audio_output_register_external_source(audio_output_pull_fn fn, void *ctx) {
  (void)fn;
  (void)ctx;
}

__attribute__((weak)) void audio_output_select_source(audio_source_t source) {
  (void)source;
}

__attribute__((weak)) audio_source_t audio_output_active_source(void) {
  return AUDIO_SOURCE_AIRPLAY;
}

__attribute__((weak)) void
audio_output_set_source_volume(audio_source_t source, int32_t volume_q15,
                               bool immediate) {
  (void)source;
  (void)volume_q15;
  (void)immediate;
}

__attribute__((weak)) void audio_output_get_stats(audio_output_stats_t *out) {
  if (out) {
    memset(out, 0, sizeof(*out));
  }
}
