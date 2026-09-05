#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * ESP-IDF glue around the pure per-source volume table (source_volume.h):
 * a mutex, one NVS blob, and lazy persistence.
 *
 * Volumes are remembered in RAM on every change and written to NVS only at
 * quiet moments (session end), so a slider drag never causes flash writes
 * while audio is playing (an NVS write disables the cache for tens of ms).
 */

esp_err_t source_volume_store_init(void);

/** Look up the remembered level for a sender identity.  false = unknown. */
bool source_volume_store_get(const char *id, float *volume_db);

/** Remember a level for a sender identity (RAM; persisted later). */
void source_volume_store_set(const char *id, float volume_db);

/** Write the table to NVS if it changed since the last persist. */
esp_err_t source_volume_store_persist(void);
