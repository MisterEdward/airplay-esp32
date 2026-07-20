#include "audio_telemetry.h"

#include <inttypes.h>
#include <stdbool.h>

#include "audio_output.h"
#include "audio_receiver.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ptp_clock.h"

#define TELEMETRY_SAMPLE_MS       1000
#define HEALTH_LOG_INTERVAL_TICKS 10
#define INTERNAL_HEAP_WARN_BYTES  (64 * 1024)
#define LARGEST_BLOCK_WARN_BYTES  (24 * 1024)
#define STACK_WARN_WORDS          256

static const char *TAG = "audio_health";
static TaskHandle_t telemetry_task_handle;

static int64_t elapsed_ms(int64_t event_us, int64_t start_us) {
  return event_us > 0 && start_us > 0 ? (event_us - start_us) / 1000LL : -1;
}

static bool counters_moved_back(const audio_stats_t *current,
                                const audio_stats_t *previous) {
  return current->packets_received < previous->packets_received ||
         current->packets_decoded < previous->packets_decoded ||
         current->packets_dropped < previous->packets_dropped ||
         current->late_frames < previous->late_frames ||
         current->buffer_underruns < previous->buffer_underruns ||
         current->decrypt_errors < previous->decrypt_errors;
}

static uint32_t counter_delta(uint32_t current, uint32_t previous,
                              bool reset) {
  return reset ? 0 : current - previous;
}

static void log_seek_event(const audio_seek_diag_t *diag,
                           uint32_t *previous_generation,
                           bool *previous_started) {
  if (diag->generation != *previous_generation) {
    *previous_generation = diag->generation;
    *previous_started = false;
    ESP_LOGI(TAG,
             "SEEK begin gen=%" PRIu32 " blanket/lower/upper=%" PRIu32
             "/%" PRIu32 "/%" PRIu32,
             diag->generation, diag->blanket_drops, diag->lower_gate_drops,
             diag->upper_gate_drops);
  }

  if (diag->playout_started && !*previous_started &&
      diag->playout_started_us >= diag->seek_started_us) {
    ESP_LOGI(TAG,
             "SEEK ready gen=%" PRIu32 " audible=+%lldms anchor=+%lldms"
             " first_rx=+%lldms first_q=+%lldms gates=%" PRIu32 "/%" PRIu32
             "/%" PRIu32,
             diag->generation,
             (long long)elapsed_ms(diag->playout_started_us,
                                   diag->seek_started_us),
             (long long)elapsed_ms(diag->anchor_received_us,
                                   diag->seek_started_us),
             (long long)elapsed_ms(diag->first_rx_us, diag->seek_started_us),
             (long long)elapsed_ms(diag->first_queue_us,
                                   diag->seek_started_us),
             diag->blanket_drops, diag->lower_gate_drops,
             diag->upper_gate_drops);
  }
  *previous_started = diag->playout_started;
}

static void telemetry_task(void *arg) {
  (void)arg;

  audio_stats_t previous_stats = {0};
  bool have_previous_stats = false;
  uint32_t previous_generation = 0;
  bool previous_started = false;
  uint32_t health_ticks = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(TELEMETRY_SAMPLE_MS));

    audio_seek_diag_t diag = {0};
    audio_receiver_get_seek_diag(&diag);
    log_seek_event(&diag, &previous_generation, &previous_started);

    if (!diag.stream_running) {
      previous_stats = diag.stats;
      have_previous_stats = true;
      health_ticks = 0;
      continue;
    }

    health_ticks++;
    if (health_ticks < HEALTH_LOG_INTERVAL_TICKS) {
      continue;
    }
    health_ticks = 0;

    bool counters_reset = !have_previous_stats ||
                          counters_moved_back(&diag.stats, &previous_stats);
    uint32_t received = counter_delta(diag.stats.packets_received,
                                      previous_stats.packets_received,
                                      counters_reset);
    uint32_t decoded = counter_delta(diag.stats.packets_decoded,
                                     previous_stats.packets_decoded,
                                     counters_reset);
    uint32_t dropped = counter_delta(diag.stats.packets_dropped,
                                     previous_stats.packets_dropped,
                                     counters_reset);
    uint32_t late = counter_delta(diag.stats.late_frames,
                                  previous_stats.late_frames, counters_reset);
    uint32_t underruns = counter_delta(
        diag.stats.buffer_underruns, previous_stats.buffer_underruns,
        counters_reset);
    uint32_t decrypt_errors = counter_delta(
        diag.stats.decrypt_errors, previous_stats.decrypt_errors,
        counters_reset);
    previous_stats = diag.stats;
    have_previous_stats = true;

    uint32_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t spiram_free =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ptp_stats_t ptp = {0};
    ptp_clock_get_stats(&ptp);

    uint32_t output_stack = audio_output_get_stack_high_watermark();
    uint32_t telemetry_stack =
        (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    int64_t packet_age_ms =
        diag.buffered_client_connected && diag.buffered_last_packet_us > 0
            ? (esp_timer_get_time() - diag.buffered_last_packet_us) / 1000LL
            : -1;

    bool low_stack =
        (output_stack > 0 && output_stack < STACK_WARN_WORDS) ||
        (diag.buffered_reader_stack_words > 0 &&
         diag.buffered_reader_stack_words < STACK_WARN_WORDS) ||
        (diag.buffered_decoder_stack_words > 0 &&
         diag.buffered_decoder_stack_words < STACK_WARN_WORDS) ||
        telemetry_stack < STACK_WARN_WORDS;
    bool unhealthy = internal_free < INTERNAL_HEAP_WARN_BYTES ||
                     internal_largest < LARGEST_BLOCK_WARN_BYTES || low_stack ||
                     underruns > 0 || decrypt_errors > 0 ||
                     diag.deferred_flush_overflow > 0;

    ESP_LOGI(
        TAG,
        "HEALTH %s type=%u run/play=%d/%d reset=%d"
        " rx/dc/dr=%" PRIu32 "/%" PRIu32 "/%" PRIu32
        " late/ur/de=%" PRIu32 "/%" PRIu32 "/%" PRIu32
        " buf=%" PRIu32 "/%" PRIu32
        " ptp=%d off=%" PRId64 "us"
        " heap=%" PRIu32 " largest=%" PRIu32 " psram=%" PRIu32
        " stack(out/read/dec/tel)=%" PRIu32 "/%" PRIu32 "/%" PRIu32
        "/%" PRIu32
        " tcp(up/age/conn/stall)=%d/%lld/%" PRIu32 "/%" PRIu32
        " flush(active/armed/dup/drop/exp/full)=%" PRIu32 "/%" PRIu32
        "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32,
        unhealthy ? "WARN" : "OK", diag.stream_type,
        diag.stream_running, diag.playing, counters_reset, received, decoded,
        dropped, late, underruns, decrypt_errors, diag.buffer_frames,
        diag.target_buffer_frames, ptp_clock_is_locked(),
        ptp.filtered_offset_ns / 1000LL, internal_free, internal_largest,
        spiram_free, output_stack, diag.buffered_reader_stack_words,
        diag.buffered_decoder_stack_words, telemetry_stack,
        diag.buffered_client_connected, (long long)packet_age_ms,
        diag.buffered_connections_accepted, diag.buffered_stall_recoveries,
        diag.deferred_flush_active, diag.deferred_flush_armed,
        diag.deferred_flush_duplicates, diag.deferred_flush_dropped,
        diag.deferred_flush_expired, diag.deferred_flush_overflow);
  }
}

esp_err_t audio_telemetry_start(void) {
  if (telemetry_task_handle) {
    return ESP_OK;
  }
  BaseType_t result = xTaskCreate(telemetry_task, "audio_health", 4096, NULL, 2,
                                  &telemetry_task_handle);
  if (result != pdPASS || !telemetry_task_handle) {
    telemetry_task_handle = NULL;
    ESP_LOGE(TAG, "Failed to create health telemetry task");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Health telemetry started: sample=%dms report=%ds",
           TELEMETRY_SAMPLE_MS,
           (TELEMETRY_SAMPLE_MS * HEALTH_LOG_INTERVAL_TICKS) / 1000);
  return ESP_OK;
}
