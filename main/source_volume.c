#include "source_volume.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#define MIN_CENTI_DB (-3000)
#define MAX_CENTI_DB 0

void source_volume_table_init(source_volume_table_t *table) {
  memset(table, 0, sizeof(*table));
  table->magic = SOURCE_VOLUME_MAGIC;
}

bool source_volume_table_validate(source_volume_table_t *table) {
  bool ok = table->magic == SOURCE_VOLUME_MAGIC;
  for (size_t i = 0; ok && i < SOURCE_VOLUME_SLOTS; i++) {
    source_volume_entry_t *e = &table->entries[i];
    if (!memchr(e->id, '\0', sizeof(e->id))) {
      ok = false;
    } else if (e->id[0] != '\0' &&
               (e->centi_db < MIN_CENTI_DB || e->centi_db > MAX_CENTI_DB)) {
      ok = false;
    }
  }
  if (!ok) {
    source_volume_table_init(table);
  }
  return ok;
}

bool source_volume_normalize_id(const char *in, char *out, size_t out_size) {
  if (!in || !out || out_size < 2) {
    return false;
  }
  size_t n = 0;
  for (const char *p = in; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == ' ' || c == '\t') {
      continue;
    }
    if (!isalnum(c) && c != ':' && c != '-' && c != '_' && c != '.') {
      return false;
    }
    if (n + 1 >= out_size) {
      return false; // do not silently truncate an identity
    }
    out[n++] = (char)tolower(c);
  }
  out[n] = '\0';
  return n > 0;
}

static source_volume_entry_t *find(source_volume_table_t *table,
                                   const char *id) {
  for (size_t i = 0; i < SOURCE_VOLUME_SLOTS; i++) {
    if (table->entries[i].id[0] != '\0' &&
        strcmp(table->entries[i].id, id) == 0) {
      return &table->entries[i];
    }
  }
  return NULL;
}

bool source_volume_table_get(source_volume_table_t *table, const char *id,
                             float *volume_db) {
  if (!id || !id[0] || !volume_db) {
    return false;
  }
  source_volume_entry_t *e = find(table, id);
  if (!e) {
    return false;
  }
  e->last_used = ++table->use_counter;
  *volume_db = (float)e->centi_db / 100.0f;
  return true;
}

bool source_volume_table_set(source_volume_table_t *table, const char *id,
                             float volume_db) {
  if (!id || !id[0] || strlen(id) >= SOURCE_VOLUME_ID_SIZE ||
      !isfinite(volume_db)) {
    return false;
  }
  int32_t centi = (int32_t)lroundf(volume_db * 100.0f);
  if (centi < MIN_CENTI_DB) {
    centi = MIN_CENTI_DB;
  }
  if (centi > MAX_CENTI_DB) {
    centi = MAX_CENTI_DB;
  }

  source_volume_entry_t *e = find(table, id);
  bool changed = false;
  if (!e) {
    // Pick an empty slot, else the least recently used one.
    e = &table->entries[0];
    for (size_t i = 0; i < SOURCE_VOLUME_SLOTS; i++) {
      source_volume_entry_t *c = &table->entries[i];
      if (c->id[0] == '\0') {
        e = c;
        break;
      }
      if (c->last_used < e->last_used) {
        e = c;
      }
    }
    memset(e, 0, sizeof(*e));
    strncpy(e->id, id, sizeof(e->id) - 1);
    changed = true;
  }
  if (e->centi_db != centi) {
    e->centi_db = centi;
    changed = true;
  }
  e->last_used = ++table->use_counter;
  return changed;
}
