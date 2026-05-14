#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Drift-compensating linear-interpolation resampler.
 *
 * Sits between the main fixed-ratio resampler (audio_resample) and the I2S
 * output.  The I2S clock is the master reference (we cannot change it without
 * touching APLL — proven unsafe on ESP32-A1S+ES8388), so any difference
 * between sender clock and our local I2S clock accumulates as a steady-state
 * A/V skew that the late-frame threshold cannot recover.  This module bridges
 * that gap by applying a tiny rate adjustment in software: it consumes
 * (output_rate × (1 + correction)) input samples per second and emits
 * output_rate samples per second to I2S.
 *
 * Correction is bounded to ±2000 ppm in steady state and a P-controller
 * targets the smoothed drift over a 24-second time constant.  After seek/scrub
 * a short catch-up mode allows a larger correction so phase error converges
 * quickly.  Small drift is ignored via a 5 ms deadband to avoid hunting on
 * jitter.
 *
 * The resampler is always in the path; at zero correction it is a unity
 * delay of one input frame (~22 µs), which is inaudible.
 */

void audio_servo_init(void);

/**
 * Reset the resampler state and zero the integrator.  Call on stream start,
 * teardown, or buffer flush.  Does not zero target correction immediately —
 * lets the smoother walk the rate back to 1.0 over the next ~1 s.
 */
void audio_servo_reset(void);

/**
 * Temporarily allow stronger rate correction after seek/scrub.  The boost
 * self-expires and also exits once drift is close enough for normal servo.
 */
void audio_servo_start_seek_boost(void);

/**
 * Update the controller with the current smoothed drift signal.  Called by
 * the playback task once per audio chunk (~8 ms cadence).  Positive
 * drift_us means we are pulling frames before their anchor (audio leads);
 * negative means we are pulling them after (audio lags).
 *
 * @param drift_us  smoothed early_us from audio_timing
 * @param playing   false when paused/idle — controller holds correction at 0
 */
void audio_servo_update(int32_t drift_us, bool playing);

/**
 * Apply the current rate correction to a chunk of interleaved stereo PCM.
 * Returns the number of output frames produced.  Caller must size out for at
 * least in_frames + 4 frames of headroom.
 */
size_t audio_servo_process(const int16_t *in, size_t in_frames, int16_t *out,
                           size_t out_capacity);

/**
 * @return current applied (smoothed) correction in parts-per-million.
 * Positive = source plays faster than I2S clock (we are catching up).
 */
int32_t audio_servo_get_correction_ppm(void);
