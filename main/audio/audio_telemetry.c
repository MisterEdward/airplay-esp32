#include <inttypes.h>
#include <string.h>

#include "audio_telemetry.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_output.h"
#include "audio_receiver.h"
#include "ptp_clock.h"

#define AUDIO_TELEMETRY_INTERVAL_MS 1000

static const char *TAG = "telemetry";

static TaskHandle_t s_task = NULL;

static const char *stream_type_name(audio_stream_type_t t) {
  switch (t) {
  case AUDIO_STREAM_REALTIME:
    return "rt96";
  case AUDIO_STREAM_BUFFERED:
    return "buf103";
  default:
    return "none";
  }
}

static void telemetry_task(void *arg) {
  (void)arg;

  audio_stats_t prev = {0};
  audio_stream_type_t prev_type = AUDIO_STREAM_NONE;
  bool have_prev = false;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(AUDIO_TELEMETRY_INTERVAL_MS));

    audio_stream_type_t type = audio_receiver_get_active_stream_type();

    audio_stats_t cur = {0};
    audio_receiver_get_stats(&cur);

    // Reset deltas if stream type changed (counters were reset by recv path).
    if (!have_prev || type != prev_type) {
      prev = cur;
      prev_type = type;
      have_prev = true;
      continue;
    }

    // Detect a counter reset (recv stop+start clears stats); avoid huge deltas.
    if (cur.packets_received < prev.packets_received) {
      prev = cur;
      continue;
    }

    if (type == AUDIO_STREAM_NONE && cur.packets_received == prev.packets_received) {
      // Idle — drain the per-window accumulators so they don't carry over,
      // but skip the log line.
      uint32_t s, mn, mx, av;
      audio_receiver_drain_buffer_depth(&s, &mn, &mx, &av);
      audio_output_write_stats_t ws;
      audio_output_drain_write_stats(&ws);
      prev = cur;
      continue;
    }

    uint32_t d_received = cur.packets_received - prev.packets_received;
    uint32_t d_decoded = cur.packets_decoded - prev.packets_decoded;
    uint32_t d_dropped = cur.packets_dropped - prev.packets_dropped;
    uint32_t d_decrypt_err = cur.decrypt_errors - prev.decrypt_errors;
    uint32_t d_underruns = cur.buffer_underruns - prev.buffer_underruns;
    uint32_t d_late = cur.late_frames - prev.late_frames;
    uint32_t d_gaps = cur.seq_gap_events - prev.seq_gap_events;
    uint32_t d_gap_pkts =
        cur.seq_gap_total_packets - prev.seq_gap_total_packets;
    uint32_t d_nack = cur.nack_requests_sent - prev.nack_requests_sent;
    uint32_t d_nack_err = cur.nack_send_errors - prev.nack_send_errors;
    uint32_t d_retx =
        cur.retransmit_packets_received - prev.retransmit_packets_received;

    uint32_t depth_samples = 0, depth_min = 0, depth_max = 0, depth_avg = 0;
    audio_receiver_drain_buffer_depth(&depth_samples, &depth_min, &depth_max,
                                      &depth_avg);

    int32_t drift_min = 0, drift_max = 0;
    uint32_t drift_samples = 0;
    audio_receiver_drain_drift(&drift_min, &drift_max, &drift_samples);

    audio_output_write_stats_t ws = {0};
    audio_output_drain_write_stats(&ws);

    ptp_stats_t pts = {0};
    ptp_clock_get_stats(&pts);

    // Jitter exposed as RFC3550 J in samples (Q4 -> divide by 16).
    uint32_t jitter_samples = cur.arrival_jitter_q4 >> 4;

    ESP_LOGI(TAG,
             "%s rx=%" PRIu32 " dec=%" PRIu32 " drop=%" PRIu32
             " gap_evt=%" PRIu32 " gap_pkt=%" PRIu32 " maxgap=%" PRIu32
             " nack=%" PRIu32 "/%" PRIu32 " retx=%" PRIu32
             " jitter=%" PRIu32 "smp dts=%" PRId32
             " | buf min/avg/max=%" PRIu32 "/%" PRIu32 "/%" PRIu32
             " ur=%" PRIu32 " late=%" PRIu32
             " | drift min/max=%" PRId32 "/%" PRId32 "ms"
             " | i2s w=%" PRIu32 " avg=%" PRIu32 "us max=%" PRIu32
             "us sil=%" PRIu32
             " | ptp lock=%d t=%" PRIu32 "ms off=%" PRId64
             "ns dev=%" PRId64 "ns dec_err=%" PRIu32,
             stream_type_name(type), d_received, d_decoded, d_dropped, d_gaps,
             d_gap_pkts, cur.max_seq_gap, d_nack, d_nack_err, d_retx,
             jitter_samples, cur.last_rtp_ts_delta, depth_min, depth_avg,
             depth_max, d_underruns, d_late,
             drift_samples ? drift_min / 1000 : 0,
             drift_samples ? drift_max / 1000 : 0,
             ws.writes, ws.avg_us, ws.max_us,
             ws.silence_writes, pts.locked ? 1 : 0, pts.lock_time_ms,
             pts.filtered_offset_ns, pts.max_dev_ns, d_decrypt_err);

    prev = cur;
  }
}

esp_err_t audio_telemetry_start(void) {
  if (s_task) {
    return ESP_OK;
  }
  BaseType_t ok =
      xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 2, &s_task);
  if (ok != pdPASS) {
    s_task = NULL;
    ESP_LOGE(TAG, "Failed to create telemetry task");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Telemetry task started (interval=%dms)",
           AUDIO_TELEMETRY_INTERVAL_MS);
  return ESP_OK;
}
