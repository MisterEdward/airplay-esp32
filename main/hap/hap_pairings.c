// Controllers that completed a non-transient pair-setup (M5/M6), kept in NVS
// so a later pair-verify can check their long-term Ed25519 key.  A small
// fixed table, oldest entry replaced when full.
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "hap_pairings.h"

static const char *TAG = "hap_pairings";

#define NVS_NAMESPACE "airplay"
#define NVS_KEY_TABLE "hap_ctrls"

typedef struct {
  uint8_t used;
  uint8_t id_len;
  char id[HAP_PAIRING_ID_MAX];
  uint8_t ltpk[32];
} pairing_entry_t;

typedef struct {
  uint32_t next; // slot to overwrite when the table is full
  pairing_entry_t entry[HAP_PAIRINGS_MAX];
} pairing_table_t;

static bool load(pairing_table_t *t) {
  memset(t, 0, sizeof(*t));
  nvs_handle_t nvs;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
    return false;
  }
  size_t len = sizeof(*t);
  esp_err_t err = nvs_get_blob(nvs, NVS_KEY_TABLE, t, &len);
  nvs_close(nvs);
  if (err != ESP_OK || len != sizeof(*t)) {
    memset(t, 0, sizeof(*t));
    return false;
  }
  return true;
}

static esp_err_t store(const pairing_table_t *t) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_blob(nvs, NVS_KEY_TABLE, t, sizeof(*t));
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);
  return err;
}

static int find(const pairing_table_t *t, const uint8_t *id, size_t id_len) {
  for (int i = 0; i < HAP_PAIRINGS_MAX; i++) {
    const pairing_entry_t *e = &t->entry[i];
    if (e->used && e->id_len == id_len && memcmp(e->id, id, id_len) == 0) {
      return i;
    }
  }
  return -1;
}

esp_err_t hap_pairings_add(const uint8_t *id, size_t id_len,
                           const uint8_t ltpk[32]) {
  if (!id || id_len == 0 || id_len > HAP_PAIRING_ID_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  static pairing_table_t t; // ~1 KB: keep it off the RTSP task stack
  load(&t);
  int slot = find(&t, id, id_len);
  if (slot < 0) {
    for (int i = 0; i < HAP_PAIRINGS_MAX && slot < 0; i++) {
      if (!t.entry[i].used) {
        slot = i;
      }
    }
  }
  if (slot < 0) {
    slot = (int)(t.next % HAP_PAIRINGS_MAX);
    t.next = (uint32_t)(slot + 1);
  }
  pairing_entry_t *e = &t.entry[slot];
  memset(e, 0, sizeof(*e));
  e->used = 1;
  e->id_len = (uint8_t)id_len;
  memcpy(e->id, id, id_len);
  memcpy(e->ltpk, ltpk, 32);
  esp_err_t err = store(&t);
  ESP_LOGI(TAG, "controller %.*s paired (slot %d): %s", (int)id_len,
           (const char *)id, slot, esp_err_to_name(err));
  return err;
}

bool hap_pairings_find(const uint8_t *id, size_t id_len, uint8_t ltpk[32]) {
  static pairing_table_t t;
  if (!id || id_len == 0 || id_len > HAP_PAIRING_ID_MAX || !load(&t)) {
    return false;
  }
  int slot = find(&t, id, id_len);
  if (slot < 0) {
    return false;
  }
  memcpy(ltpk, t.entry[slot].ltpk, 32);
  return true;
}
