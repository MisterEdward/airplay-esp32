#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Per-source volume memory.
 *
 * Every sender that streams to this speaker remembers its own level, keyed
 * by a stable identity the sender itself supplies (AirPlay 2: the `deviceID`
 * from SETUP, a MAC-style string; USB: the fixed id "usb").  IP addresses are
 * never used as identity — DHCP hands them out to whoever asks.
 *
 * The table is small (SOURCE_VOLUME_SLOTS) with least-recently-used
 * replacement, is fully serialisable as one blob so persistence is a single
 * NVS write, and has no ESP-IDF dependency here: the pure table lives in
 * this module and is unit-tested on the host; `source_volume_store.c`
 * wraps it with a mutex and NVS.
 */

#define SOURCE_VOLUME_SLOTS   12
#define SOURCE_VOLUME_ID_SIZE 40
#define SOURCE_VOLUME_MAGIC   0x46564F4Cu /* "FVOL" */

typedef struct {
  char id[SOURCE_VOLUME_ID_SIZE];
  int32_t centi_db; // volume in dB × 100, AirPlay range -3000..0
  uint32_t last_used; // monotonically increasing use counter (LRU)
} source_volume_entry_t;

typedef struct {
  uint32_t magic;
  uint32_t use_counter;
  source_volume_entry_t entries[SOURCE_VOLUME_SLOTS];
} source_volume_table_t;

/** Reset to empty (magic set, no entries). */
void source_volume_table_init(source_volume_table_t *table);

/**
 * Validate a table loaded from storage.  Returns false (and re-initialises
 * the table) if the magic, string termination or value ranges are bad.
 */
bool source_volume_table_validate(source_volume_table_t *table);

/** Look up `id`; returns false if unknown.  Marks the entry as used. */
bool source_volume_table_get(source_volume_table_t *table, const char *id,
                             float *volume_db);

/**
 * Store `volume_db` for `id`, evicting the least recently used entry if the
 * table is full.  Returns true if the table content changed.
 */
bool source_volume_table_set(source_volume_table_t *table, const char *id,
                             float volume_db);

/** Normalise an identity string: lowercase, only [a-z0-9:-_.], bounded. */
bool source_volume_normalize_id(const char *in, char *out, size_t out_size);
