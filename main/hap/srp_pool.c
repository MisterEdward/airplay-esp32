// One precomputed transient SRP server context, so pair-setup M1 answers at
// once instead of spending ~0.75 s on two 3072-bit modexps (v and g^b)
// while the sender waits.  Every AirPlay 2 session from a Mac or iPhone
// starts with a transient pair-setup ("Pair-Setup", PIN 3939, flag 0x10),
// and nothing in M1 feeds that computation, so it can be done ahead.
//
// The context is used once.  The refill runs at the lowest priority, only
// after a pairing has finished or a connection has closed, so it never
// competes with the M3 computation of the session that consumed it.
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sodium.h"

#include "srp.h"

static const char *TAG = "srp_pool";

#define POOL_TASK_STACK      6144
#define POOL_REFILL_DELAY_MS 100
// M1 waits this long for a refill already under way rather than starting a
// second computation next to it.
#define POOL_TAKE_WAIT_MS 2000

static srp_session_t s_ready;
static bool s_ready_valid;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

static void pool_task(void *arg) {
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(POOL_REFILL_DELAY_MS));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ready_valid) {
      int64_t t0 = esp_timer_get_time();
      if (srp_start(&s_ready, "Pair-Setup", "3939") == ESP_OK) {
        s_ready_valid = true;
        ESP_LOGI(TAG, "transient pair-setup context ready (%lld ms)",
                 (long long)((esp_timer_get_time() - t0) / 1000));
      } else {
        sodium_memzero(&s_ready, sizeof(s_ready));
        ESP_LOGW(TAG, "precompute failed; M1 will compute inline");
      }
    }
    xSemaphoreGive(s_lock);
  }
}

void srp_pool_init(void) {
  if (s_task) {
    return;
  }
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) {
    ESP_LOGE(TAG, "no memory for the pool lock");
    return;
  }
  if (xTaskCreate(pool_task, "srp_pool", POOL_TASK_STACK, NULL,
                  tskIDLE_PRIORITY + 1, &s_task) != pdPASS) {
    ESP_LOGE(TAG, "failed to start the precompute task");
    s_task = NULL;
    return;
  }
  srp_pool_refill();
}

void srp_pool_refill(void) {
  if (s_task) {
    xTaskNotifyGive(s_task);
  }
}

esp_err_t srp_start_transient(srp_session_t *session) {
  if (s_lock &&
      xSemaphoreTake(s_lock, pdMS_TO_TICKS(POOL_TAKE_WAIT_MS)) == pdTRUE) {
    bool taken = s_ready_valid;
    if (taken) {
      memcpy(session, &s_ready, sizeof(*session));
      sodium_memzero(&s_ready, sizeof(s_ready));
      s_ready_valid = false;
    }
    xSemaphoreGive(s_lock);
    if (taken) {
      ESP_LOGI(TAG, "pair-setup M1 served from the precomputed context");
      return ESP_OK;
    }
  }
  return srp_start(session, "Pair-Setup", "3939");
}
