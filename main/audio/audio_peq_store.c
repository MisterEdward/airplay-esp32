#include "audio_peq_store.h"

#include "audio_output.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "peq";

#define NVS_NAMESPACE "fable"
#define NVS_KEY       "peq_v1"
#define PEQ_RATE      CONFIG_OUTPUT_SAMPLE_RATE_HZ

static SemaphoreHandle_t s_mutex;
static peq_config_t s_cfg;
static peq_design_t s_design;

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

// Persist s_cfg.  Mutex held.  The NVS write stalls the flash cache for a
// few tens of ms; the 42 ms DMA ring rides through it.
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

esp_err_t audio_peq_store_init(void) {
  if (s_mutex) {
    return ESP_OK;
  }
  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) {
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
  esp_err_t err = save_locked();
  xSemaphoreGive(s_mutex);
  return err;
}

esp_err_t audio_peq_store_set_enabled(bool enabled) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_cfg.enabled = enabled;
  apply_locked();
  esp_err_t err = save_locked();
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
  esp_err_t err = save_locked();
  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "Imported %d REW filters into %s (%d skipped)", n,
           channel < 0 ? "both channels" : (channel ? "R" : "L"), bad);
  return err;
}
