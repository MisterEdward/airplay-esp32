// One precomputed set of SRP server keys for the next transient pair-setup.
//
// A transient M1 needs a fresh salt, secret b, verifier v = g^x and public
// key B = k*v + g^b: two modexps (~150 ms on the S3) that depend on nothing
// the sender sends.  A low-priority task computes them ahead of time, so M1
// only copies them.  The keys are single-use: srp_pool_take() hands them to
// one session and clears the slot.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "sodium.h"

#include "srp.h"

static const char *TAG = "srp_pool";

// The refill waits this long after a take: the M3 that follows an M1 by one
// round trip must get the accelerator to itself.
#define SRP_POOL_REFILL_DELAY_MS 1000
#define SRP_POOL_STACK           4096
#define SRP_POOL_PRIO            1

static const char *const POOL_USER = "Pair-Setup";
static const char *const POOL_PASS = "3939";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static srp_session_t s_spare; // guarded by s_lock
static bool s_spare_ready;    // guarded by s_lock
static srp_session_t s_work;  // pool task only

static void pool_task(void *arg) {
  (void)arg;
  bool first = true;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!first) {
      vTaskDelay(pdMS_TO_TICKS(SRP_POOL_REFILL_DELAY_MS));
    }
    first = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_spare_ready;
    xSemaphoreGive(s_lock);
    if (ready) {
      continue;
    }

    int64_t t0 = esp_timer_get_time();
    memset(&s_work, 0, sizeof(s_work));
    if (srp_start(&s_work, POOL_USER, POOL_PASS) != ESP_OK) {
      ESP_LOGW(TAG, "precompute failed; M1 will compute inline");
      continue;
    }
    int64_t took = esp_timer_get_time() - t0;
    int64_t gx_us = s_work.t1_us;
    int64_t gb_us = s_work.t2_us;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_spare = s_work;
    s_spare_ready = true;
    xSemaphoreGive(s_lock);
    sodium_memzero(&s_work, sizeof(s_work));

    ESP_LOGI(TAG, "spare keys ready: g^x=%lld g^b=%lld total=%lld us",
             (long long)gx_us, (long long)gb_us, (long long)took);
  }
}

void srp_pool_start(void) {
  if (s_task) {
    return;
  }
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) {
    ESP_LOGW(TAG, "no mutex; M1 will compute inline");
    return;
  }
  if (xTaskCreate(pool_task, "srp_pool", SRP_POOL_STACK, NULL, SRP_POOL_PRIO,
                  &s_task) != pdPASS) {
    s_task = NULL;
    ESP_LOGW(TAG, "no task; M1 will compute inline");
    return;
  }
  xTaskNotifyGive(s_task);
}

bool srp_pool_take(srp_session_t *session, const char *username,
                   const char *password) {
  if (!s_task || !session || !username || !password ||
      strcmp(username, POOL_USER) != 0 || strcmp(password, POOL_PASS) != 0) {
    return false;
  }
  bool took = false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_spare_ready) {
    *session = s_spare;
    sodium_memzero(&s_spare, sizeof(s_spare));
    s_spare_ready = false;
    took = true;
  }
  xSemaphoreGive(s_lock);
  xTaskNotifyGive(s_task);
  return took;
}
