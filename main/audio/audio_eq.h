#pragma once

/**
 * 15-band software equalizer — DAC-independent.
 *
 * Exists for boards whose DAC has no on-chip DSP (PCM5102A, plain I2S,
 * S/PDIF, ES8388...).  Boards built around the TAS5825M use the on-chip
 * biquads instead (see components/dac_tas58xx/dac_tas58xx_eq.h); the two
 * paths are mutually exclusive and selected by CONFIG_AUDIO_EQ_SOFTWARE.
 *
 * Band centre frequencies and Q values are deliberately identical to the
 * TAS58xx table so that the web UI, its presets and the NVS blob layout
 * (SETTINGS_EQ_BANDS) are shared verbatim between the two backends.
 *
 * Implementation notes that matter for the audio hot path:
 *
 *  - Cascaded RBJ peaking biquads, Direct Form I, evaluated in fixed point
 *    (Q27 coefficients, int64 accumulator).  No floating point per sample:
 *    the project keeps the playback loop integer-clean, and single-precision
 *    float would in fact be *less* accurate here — a 20 Hz biquad at 44.1 kHz
 *    needs an a1 coefficient near -2.0, where float32's 24-bit mantissa
 *    resolves ~2.4e-7 against Q27's 7.5e-9.
 *
 *  - Coefficients are computed with floating point, but only when a gain
 *    changes (a web request), never per sample.
 *
 *  - When every band sits at 0 dB the filter is bypassed entirely, so a
 *    listener who never opens the EQ page pays no CPU for it.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Number of bands.  Must match SETTINGS_EQ_BANDS and TAS58XX_EQ_BANDS. */
#define AUDIO_EQ_BANDS 15

#ifndef CONFIG_AUDIO_EQ_SOFTWARE

/* Not built for this target (either a TAS58xx board, which equalises on
   chip, or the feature was turned off).  No-op stubs keep the call sites
   in the playback loops free of #ifdef clutter; the compiler drops them
   entirely. */

static inline esp_err_t audio_eq_init(uint32_t sample_rate) {
  (void)sample_rate;
  return ESP_OK;
}
static inline void audio_eq_process(int16_t *pcm, size_t frames) {
  (void)pcm;
  (void)frames;
}
static inline void audio_eq_reset(void) {
}
static inline bool audio_eq_is_active(void) {
  return false;
}

#else

/** Per-band gain limits (dB) — matches the TAS58xx backend and the web UI. */
#define AUDIO_EQ_MAX_GAIN_DB 15.0f
#define AUDIO_EQ_MIN_GAIN_DB (-15.0f)

/**
 * Initialise the equaliser for the given output sample rate, restore any
 * saved gains from NVS, and register the eq_events listener.
 *
 * Safe to call more than once; a repeat call recomputes coefficients for
 * the new rate.  Must be called after settings_init().
 *
 * @param sample_rate Output sample rate in Hz (the rate at the point the
 *                    EQ sits in the chain, i.e. after resample/servo).
 */
esp_err_t audio_eq_init(uint32_t sample_rate);

/**
 * Set all band gains at once.  Coefficients are recomputed on the calling
 * task and handed to the playback task through a double buffer, so this is
 * safe to call from the web server task while audio is playing.
 *
 * Gains are clamped to [AUDIO_EQ_MIN_GAIN_DB, AUDIO_EQ_MAX_GAIN_DB].
 */
esp_err_t audio_eq_set_all(const float gains_db[AUDIO_EQ_BANDS]);

/** Set a single band's gain.  @param band 0–14 */
esp_err_t audio_eq_set_band(int band, float gain_db);

/** Reset every band to 0 dB (bypass). */
esp_err_t audio_eq_flat(void);

/** Copy the current gains into @p out. */
void audio_eq_get_gains(float out[AUDIO_EQ_BANDS]);

/** True when at least one band is non-zero (i.e. the filter is running). */
bool audio_eq_is_active(void);

/**
 * Clear all filter state.
 *
 * Call on flush/seek: a biquad carries up to two samples of history, and
 * feeding it a fresh, unrelated signal without clearing that history makes
 * the filter ring briefly.  Cheap (a few hundred bytes memset).
 */
void audio_eq_reset(void);

/**
 * Apply the equaliser in place to interleaved stereo 16-bit PCM.
 *
 * Returns immediately when the EQ is flat or uninitialised.
 *
 * @param pcm    Interleaved L/R samples, modified in place
 * @param frames Number of stereo frames (not samples)
 */
void audio_eq_process(int16_t *pcm, size_t frames);

/** Centre frequency (Hz) of a band, or 0.0f for an invalid index. */
float audio_eq_get_center_freq(int band);

#endif /* CONFIG_AUDIO_EQ_SOFTWARE */
