#include "source_volume_store.h"
#include "source_volume.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "source_vol";

#define NVS_NAMESPACE "fable"
#define NVS_KEY       "src_vol_v1"

static source_volume_table_t s_table;
static SemaphoreHandle_t s_mutex;
static bool s_dirty;

esp_err_t source_volume_store_init(void) {
  if (s_mutex) {
    return ESP_OK;
  }
  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) {
    return ESP_ERR_NO_MEM;
  }
  source_volume_table_init(&s_table);

  nvs_handle_t nvs;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
    size_t size = sizeof(s_table);
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY, &s_table, &size);
    if (err != ESP_OK || size != sizeof(s_table)) {
      source_volume_table_init(&s_table);
    } else if (!source_volume_table_validate(&s_table)) {
      ESP_LOGW(TAG, "Stored volume table invalid; starting empty");
    }
    nvs_close(nvs);
  }

  int known = 0;
  for (int i = 0; i < SOURCE_VOLUME_SLOTS; i++) {
    if (s_table.entries[i].id[0]) {
      known++;
      ESP_LOGI(TAG, "  remembered %s = %.2f dB", s_table.entries[i].id,
               s_table.entries[i].centi_db / 100.0f);
    }
  }
  ESP_LOGI(TAG, "Per-source volume table loaded: %d entr%s", known,
           known == 1 ? "y" : "ies");
  return ESP_OK;
}

bool source_volume_store_get(const char *id, float *volume_db) {
  if (!s_mutex || !id || !id[0] || !volume_db) {
    return false;
  }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool found = source_volume_table_get(&s_table, id, volume_db);
  xSemaphoreGive(s_mutex);
  return found;
}

void source_volume_store_set(const char *id, float volume_db) {
  if (!s_mutex || !id || !id[0]) {
    return;
  }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  if (source_volume_table_set(&s_table, id, volume_db)) {
    s_dirty = true;
  }
  xSemaphoreGive(s_mutex);
}

esp_err_t source_volume_store_persist(void) {
  if (!s_mutex) {
    return ESP_ERR_INVALID_STATE;
  }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  esp_err_t err = ESP_OK;
  if (s_dirty) {
    nvs_handle_t nvs;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
      err = nvs_set_blob(nvs, NVS_KEY, &s_table, sizeof(s_table));
      if (err == ESP_OK) {
        err = nvs_commit(nvs);
      }
      nvs_close(nvs);
    }
    if (err == ESP_OK) {
      s_dirty = false;
      ESP_LOGI(TAG, "Per-source volumes persisted");
    } else {
      ESP_LOGW(TAG, "Persist failed: %s", esp_err_to_name(err));
    }
  }
  xSemaphoreGive(s_mutex);
  return err;
}
