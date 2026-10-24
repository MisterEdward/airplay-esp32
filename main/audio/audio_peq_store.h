#pragma once

#include "audio_peq.h"
#include "esp_err.h"

/**
 * The speaker's parametric EQ as a setting: owns the configuration, keeps it
 * in NVS, designs the cascade and hands it to the render task.  Every call
 * here is control-plane (web server, boot); none runs per sample.
 */

/** Load the saved configuration (or a flat one) and apply it. */
esp_err_t audio_peq_store_init(void);

/** Copy of the configuration in use. */
void audio_peq_store_get(peq_config_t *out);

/** Copy of the design in use (preamp actually applied, peak). */
void audio_peq_store_get_design(peq_design_t *out);

/** Replace the whole configuration, apply it, persist it. */
esp_err_t audio_peq_store_set(const peq_config_t *cfg);

/** Bypass on/off (A/B), applied and persisted. */
esp_err_t audio_peq_store_set_enabled(bool enabled);

/**
 * Import a REW / Equalizer APO filter list into one channel (0 = left,
 * 1 = right) or both (-1).  Applied and persisted.
 *
 * @param bands    filters imported
 * @param skipped  filter lines not understood
 * @return ESP_ERR_INVALID_SIZE if the file has more than PEQ_MAX_BANDS
 *         filters, ESP_ERR_NOT_FOUND if it has none
 */
esp_err_t audio_peq_store_import_rew(const char *text, size_t len, int channel,
                                     int *bands, int *skipped);

/** Output sample rate the designs are made for. */
uint32_t audio_peq_store_rate(void);
