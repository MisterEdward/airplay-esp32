#include "audio_telemetry.h"

#include <inttypes.h>

#include "audio_receiver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_seek";
static TaskHandle_t telemetry_task_handle;

static int64_t elapsed_ms(int64_t event_us, int64_t seek_us) {
  return event_us > 0 && seek_us > 0 ? (event_us - seek_us) / 1000LL : -1;
}

static void telemetry_task(void *arg) {
  (void)arg;
  audio_stats_t previous = {0};
  uint32_t previous_generation = 0;
  bool previous_started = false;

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    audio_seek_diag_t d;
    audio_receiver_get_seek_diag(&d);

    if (d.generation != previous_generation) {
      previous_generation = d.generation;
      previous_started = false;
    }

    uint32_t rx_delta = d.stats.packets_received - previous.packets_received;
    uint32_t decoded_delta = d.stats.packets_decoded - previous.packets_decoded;
    uint32_t drop_delta = d.stats.packets_dropped - previous.packets_dropped;
    uint32_t late_delta = d.stats.late_frames - previous.late_frames;
    uint32_t underrun_delta =
        d.stats.buffer_underruns - previous.buffer_underruns;
    previous = d.stats;

    if (d.generation == 0 ||
        (!d.playing && rx_delta == 0 && d.generation == previous_generation)) {
      continue;
    }

    int64_t age_ms = d.seek_started_us
                         ? (esp_timer_get_time() - d.seek_started_us) / 1000LL
                         : -1;
    ESP_LOGI(TAG,
             "S3TRACE gen=%" PRIu32 " age=%lldms rx/dc/dr=%" PRIu32
             "/%" PRIu32 "/%" PRIu32 " late/ur=%" PRIu32 "/%" PRIu32
             " buf=%" PRIu32 "/%" PRIu32 " gates=%" PRIu32 "/%" PRIu32
             "/%" PRIu32 " anchor=%d(+%lldms) first_rx=%" PRIu32
             "(+%lldms) first_q=%" PRIu32 "(+%lldms) q/p/start=%d/%d/%d",
             d.generation, (long long)age_ms, rx_delta, decoded_delta,
             drop_delta, late_delta, underrun_delta, d.buffer_frames,
             d.target_buffer_frames, d.blanket_drops, d.lower_gate_drops,
             d.upper_gate_drops, d.anchor_valid,
             (long long)elapsed_ms(d.anchor_received_us, d.seek_started_us),
             d.first_rx_rtp,
             (long long)elapsed_ms(d.first_rx_us, d.seek_started_us),
             d.first_queue_rtp,
             (long long)elapsed_ms(d.first_queue_us, d.seek_started_us),
             d.quick_start, d.pending_valid, d.playout_started);

    if (d.playout_started && !previous_started) {
      ESP_LOGI(TAG,
               "S3TRACE gen=%" PRIu32
               " audible=+%lldms anchor_rtp=%" PRIu32
               " first_rx_offset=%ld first_q_offset=%ld",
               d.generation,
               (long long)elapsed_ms(d.playout_started_us,
                                     d.seek_started_us),
               d.anchor_rtp,
               (long)(d.first_rx_rtp - d.anchor_rtp),
               (long)(d.first_queue_rtp - d.anchor_rtp));
    }
    previous_started = d.playout_started;
  }
}

esp_err_t audio_telemetry_start(void) {
  if (telemetry_task_handle) {
    return ESP_OK;
  }
  BaseType_t result = xTaskCreate(telemetry_task, "audio_telem", 3072, NULL, 2,
                                  &telemetry_task_handle);
  return result == pdPASS ? ESP_OK : ESP_FAIL;
}
