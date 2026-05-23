/**
 * DAC dispatch layer
 *
 * Routes abstract DAC API calls to the registered DAC driver.
 * When no driver is registered, all functions are no-ops.
 *
 * Thread safety: every DAC op below performs blocking I2C writes (the
 * underlying codec drivers — ES8388 / TAS57xx / TAS58xx — all bang the
 * codec's I2C register file).  Multiple FreeRTOS tasks call these:
 *   - bt_app_task → set_volume on AVRCP absolute-volume commands
 *   - audio playback task → set_power_mode / enable_speaker on start/stop
 *   - rtsp task → set_volume on AirPlay volume changes
 *   - audio_alert task → set_volume to bracket the chime
 *   - buttons task → set_volume from physical button presses
 * Without serialization, two concurrent set_volume calls interleave their
 * I2C transactions and one can crash the codec driver (or the I2C peripheral
 * itself).  A single mutex around every op guarantees one-at-a-time access.
 */

#include "dac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const dac_ops_t *s_ops = NULL;
static SemaphoreHandle_t s_dac_mutex = NULL;
static StaticSemaphore_t s_dac_mutex_buf;

static inline void dac_lock_init_once(void) {
  // Lazy init.  Safe because dac_register() is called once at boot before
  // any other DAC op fires.  If the mutex hasn't been created yet, it
  // means no driver is registered — but we still create it here so the
  // first user-side caller gets a deterministic ordering.
  if (s_dac_mutex == NULL) {
    s_dac_mutex = xSemaphoreCreateMutexStatic(&s_dac_mutex_buf);
  }
}

static inline bool dac_lock(void) {
  dac_lock_init_once();
  if (!s_dac_mutex) {
    return false;
  }
  // 200 ms is far longer than any legitimate I2C codec transaction; if we
  // can't get the lock in that window something else is stuck and waiting
  // longer would just stack up callers.
  return xSemaphoreTake(s_dac_mutex, pdMS_TO_TICKS(200)) == pdTRUE;
}

static inline void dac_unlock(void) {
  if (s_dac_mutex) {
    xSemaphoreGive(s_dac_mutex);
  }
}

void dac_register(const dac_ops_t *ops) {
  dac_lock_init_once();
  s_ops = ops;
}

esp_err_t dac_init(void *i2c_bus) {
  if (!dac_lock()) {
    return ESP_ERR_TIMEOUT;
  }
  esp_err_t err = (s_ops && s_ops->init) ? s_ops->init(i2c_bus) : ESP_OK;
  dac_unlock();
  return err;
}

esp_err_t dac_deinit(void) {
  if (!dac_lock()) {
    return ESP_ERR_TIMEOUT;
  }
  esp_err_t err = (s_ops && s_ops->deinit) ? s_ops->deinit() : ESP_OK;
  dac_unlock();
  return err;
}

void dac_set_volume(float volume_db) {
  if (!dac_lock()) {
    return;
  }
  if (s_ops && s_ops->set_volume) {
    s_ops->set_volume(volume_db);
  }
  dac_unlock();
}

void dac_set_power_mode(dac_power_mode_t mode) {
  if (!dac_lock()) {
    return;
  }
  if (s_ops && s_ops->set_power_mode) {
    s_ops->set_power_mode(mode);
  }
  dac_unlock();
}

void dac_enable_speaker(bool enable) {
  if (!dac_lock()) {
    return;
  }
  if (s_ops && s_ops->enable_speaker) {
    s_ops->enable_speaker(enable);
  }
  dac_unlock();
}

void dac_enable_line_out(bool enable) {
  if (!dac_lock()) {
    return;
  }
  if (s_ops && s_ops->enable_line_out) {
    s_ops->enable_line_out(enable);
  }
  dac_unlock();
}
