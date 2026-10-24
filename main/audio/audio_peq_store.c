#include "audio_peq_store.h"

#include "audio_arbiter.h"
#include "audio_envelope.h"
#include "audio_output.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "peq";

#define NVS_NAMESPACE "fable"
#define NVS_KEY       "peq_v1"
#define PEQ_RATE      CONFIG_OUTPUT_SAMPLE_RATE_HZ

// Changes are applied at once but written to NVS only once the output is
// quiet, and no sooner than this after the last one (a slider drag is many
// POSTs).  Measured 2026-10-24: an NVS write that erases a page stalls the
// flash cache long enough to step the playout ~20 ms (Playout err -20 ms,
// the servo chasing it, then a re-acquire with dropped=3).  The volume
// store follows the same rule (rtsp_conn_free()).
#define PERSIST_DELAY_MS 2000

static SemaphoreHandle_t s_mutex;
static peq_config_t s_cfg;
static peq_design_t s_design;
static bool s_dirty;
static TaskHandle_t s_persist_task;

uint32_t audio_peq_store_rate(void) {
  return PEQ_RATE;
}

static void log_design(void) {
  if (!s_design.active) {
    ESP_LOGI(TAG, "EQ %s", s_cfg.enabled ? "flat" : "bypassed");
    return;
  }
  for (int c = 0; c < PEQ_CHANNELS; c++) {
    ESP_LOGI(TAG, "EQ %c: %d bands, peak %+.1f dB, preamp %+.1f dB",
             c ? 'R' : 'L', s_design.count[c], s_design.peak_db[c],
             s_design.preamp_db[c]);
  }
}

// Design from s_cfg and hand it to the render task.  Mutex held.
static void apply_locked(void) {
  peq_config_sanitize(&s_cfg, PEQ_RATE);
  peq_design(&s_cfg, PEQ_RATE, &s_design);
  audio_output_set_peq(&s_design);
  log_design();
}

// Persist s_cfg.  Mutex held.  Only from persist_task(): the NVS write stalls
// the flash cache, which the playout timeline does not ride through.
static esp_err_t save_locked(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_blob(nvs, NVS_KEY, &s_cfg, sizeof(s_cfg));
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Saving EQ failed: %s", esp_err_to_name(err));
  }
  return err;
}

// Nothing is being timed: no AirPlay stream, no fade in progress.
static bool output_quiet(void) {
  audio_arbiter_state_t arb;
  audio_arbiter_get_state(&arb);
  audio_output_stats_t st;
  audio_output_get_stats(&st);
  return !arb.airplay_playing && st.envelope_state == ENVELOPE_SILENT &&
         !st.pause_pending;
}

// Its own task: an NVS write overflows the timer service task's 2 KB stack
// (that panicked the board).  Woken by every change; saves once
// PERSIST_DELAY_MS pass without another one and the output is quiet.
static void persist_task(void *arg) {
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PERSIST_DELAY_MS)) > 0 ||
           !output_quiet()) {
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_dirty && save_locked() == ESP_OK) {
      s_dirty = false;
      ESP_LOGI(TAG, "EQ saved");
    }
    xSemaphoreGive(s_mutex);
  }
}

// A change was applied: save it once the output is quiet.  Mutex held.
static esp_err_t mark_dirty_locked(void) {
  s_dirty = true;
  if (!s_persist_task) {
    return ESP_ERR_INVALID_STATE;
  }
  xTaskNotifyGive(s_persist_task);
  return ESP_OK;
}

esp_err_t audio_peq_store_init(void) {
  if (s_mutex) {
    return ESP_OK;
  }
  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex || xTaskCreate(persist_task, "peq_save", 4096, NULL, 1,
                              &s_persist_task) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  peq_config_default(&s_cfg);
  nvs_handle_t nvs;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
    size_t size = sizeof(s_cfg);
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY, &s_cfg, &size);
    if (err != ESP_OK || size != sizeof(s_cfg) ||
        s_cfg.version != PEQ_CONFIG_VERSION) {
      if (err == ESP_OK) {
        ESP_LOGW(TAG, "Stored EQ has another layout; starting flat");
      }
      peq_config_default(&s_cfg);
    }
    nvs_close(nvs);
  }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  apply_locked();
  xSemaphoreGive(s_mutex);
  return ESP_OK;
}

void audio_peq_store_get(peq_config_t *out) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  *out = s_cfg;
  xSemaphoreGive(s_mutex);
}

void audio_peq_store_get_design(peq_design_t *out) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  *out = s_design;
  xSemaphoreGive(s_mutex);
}

esp_err_t audio_peq_store_set(const peq_config_t *cfg) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_cfg = *cfg;
  apply_locked();
  esp_err_t err = mark_dirty_locked();
  xSemaphoreGive(s_mutex);
  return err;
}

esp_err_t audio_peq_store_set_enabled(bool enabled) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_cfg.enabled = enabled;
  apply_locked();
  esp_err_t err = mark_dirty_locked();
  xSemaphoreGive(s_mutex);
  return err;
}

esp_err_t audio_peq_store_import_rew(const char *text, size_t len, int channel,
                                     int *bands, int *skipped) {
  peq_channel_t ch;
  int bad = 0;
  int n = peq_parse_rew(text, len, &ch, &bad);
  if (bands) {
    *bands = n;
  }
  if (skipped) {
    *skipped = bad;
  }
  if (n < 0) {
    return ESP_ERR_INVALID_SIZE;
  }
  if (n == 0 && ch.preamp_db == 0.0f) {
    return ESP_ERR_NOT_FOUND;
  }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  for (int c = 0; c < PEQ_CHANNELS; c++) {
    if (channel < 0 || channel == c) {
      s_cfg.ch[c] = ch;
    }
  }
  s_cfg.enabled = true;
  apply_locked();
  esp_err_t err = mark_dirty_locked();
  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "Imported %d REW filters into %s (%d skipped)", n,
           channel < 0 ? "both channels" : (channel ? "R" : "L"), bad);
  return err;
}
