#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Output gain envelope: volume slew + fade in/out, applied sample by sample
 * by the single render task that owns the DMA writer.
 *
 * Two independent gains are multiplied together:
 *
 *   volume   — the user's level (Q15, 32768 = unity).  Follows the target
 *              exponentially with a ~3 ms time constant so a volume slider
 *              never produces a "zipper" click.
 *   fade     — a 0..1 multiplier with a smoothstep shape (3x²-2x³) that ramps
 *              up on start/seek/resume and down on pause/stop.  The S-curve
 *              has zero slope at both ends, so neither the start nor the end
 *              of the ramp introduces a discontinuity in the derivative of
 *              the waveform (a linear ramp pops faintly on loud content).
 *
 * The envelope never fabricates audio: it only scales PCM the caller already
 * has.  Silence frames emitted while waiting for a scheduled play time must
 * NOT be passed through it, otherwise they would consume the fade-in and
 * the first real sample would arrive at full level (that was an observed
 * failure mode in earlier attempts).
 *
 * All state lives in the struct; no allocation, no ESP-IDF dependencies, so
 * the module is unit-tested on the host (tests/host/test_envelope.c).
 */

typedef enum {
  ENVELOPE_SILENT = 0, // fade level at 0, nothing audible
  ENVELOPE_FADING_IN,
  ENVELOPE_OPEN, // fade level at 1
  ENVELOPE_FADING_OUT,
} envelope_state_t;

typedef struct {
  uint32_t fade_in_frames;
  uint32_t fade_out_frames;
  // Linear fade position in Q23 (0..2^23) and per-frame step.  Direction
  // changes reverse the step from the CURRENT level, so a pause requested in
  // the middle of a fade-in ramps back down from where it is rather than
  // jumping.
  int32_t level_q23;
  int32_t step_q23;
  envelope_state_t state;
  // Volume slew state.  -1 = uninitialised (first call snaps to target).
  int32_t volume_q15;
} audio_envelope_t;

/** Initialise with the output sample rate and the two fade lengths. */
void audio_envelope_init(audio_envelope_t *env, uint32_t sample_rate,
                         uint32_t fade_in_ms, uint32_t fade_out_ms);

/** Start (or continue) a fade-in from the current level. */
void audio_envelope_fade_in(audio_envelope_t *env);

/** Start a fade-out from the current level towards silence. */
void audio_envelope_fade_out(audio_envelope_t *env);

/** Jump straight to silence (used after a flush when nothing was playing). */
void audio_envelope_cut(audio_envelope_t *env);

/** Snap the volume slew to a value without ramping (session start). */
void audio_envelope_set_volume_now(audio_envelope_t *env, int32_t volume_q15);

/**
 * Apply the envelope in place to interleaved stereo PCM.
 *
 * @param pcm            interleaved L/R int16 samples
 * @param frames         number of stereo frames
 * @param volume_target  desired volume gain, Q15
 * @return true if any sample in the block is non-silent after gain (i.e. the
 *         envelope was not fully closed for the whole block)
 */
bool audio_envelope_apply(audio_envelope_t *env, int16_t *pcm, size_t frames,
                          int32_t volume_target_q15);

/** Current fade state. */
envelope_state_t audio_envelope_state(const audio_envelope_t *env);

/** True once a fade-out has completed (level reached 0). */
bool audio_envelope_is_silent(const audio_envelope_t *env);

/** Smoothstep shaping of a Q15 linear level (exposed for tests). */
int32_t audio_envelope_shape_q15(int32_t linear_q15);
