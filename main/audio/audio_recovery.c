#include "audio_recovery.h"

#include <inttypes.h>
#include <stdbool.h>

#include "audio_receiver.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtsp_server.h"

#ifdef CONFIG_BT_A2DP_ENABLE
#include "a2dp_sink.h"
#endif

#define RECOVERY_SAMPLE_MS 1000

// A live buffered stream whose reader or decoder task handle stays missing
// for this many consecutive samples has really lost a task; the sub-second
// window during task creation cannot survive five samples.
#define TASK_LOSS_TRIGGER_SAMPLES 5

// A connected, playing sender that has not delivered one complete packet for
// this long is stuck in a way rung 1 cannot fix: the 8-second socket timeout
// only fires while the reader is blocked in recv, not while it waits for a
// packet slot behind a wedged decoder or a consumer that stopped draining.
// The condition must also hold for several consecutive samples: on RESUME
// after a long pause the packet age is momentarily large while playing just
// turned true, and the first fresh packets need a moment to arrive.
#define STUCK_PACKET_AGE_US   (30LL * 1000000LL)
#define STUCK_TRIGGER_SAMPLES 5

// Grace period after an action before the ladder re-evaluates, so the sender
// has time to reconnect and re-anchor before we judge the result.
#define STREAM_RESTART_GRACE_US  (60LL * 1000000LL)
#define SERVICE_RESTART_GRACE_US (120LL * 1000000LL)

// Consecutive-failure thresholds between rungs.
#define STREAM_RESTARTS_PER_SERVICE 3
#define SERVICE_RESTARTS_PER_REBOOT 3

// Two minutes of a running stream with advancing packet counters resets the
// escalation state completely.
#define FULL_HEALTH_RESET_SECONDS 120

// Never reboot early after boot; every escalation path already needs more
// than ten minutes of repeated failures to reach the final rung.
#define REBOOT_MIN_UPTIME_US (900LL * 1000000LL)

static const char *TAG = "audio_recover";
static TaskHandle_t recovery_task_handle;
static audio_recovery_stats_t recovery_stats;

static void note_level(uint8_t level) {
  if (level > recovery_stats.level) {
    recovery_stats.level = level;
  }
}

static void restart_airplay_service(void) {
  ESP_LOGE(TAG,
           "Rung 3: restarting RTSP service after %d failed stream restarts",
           STREAM_RESTARTS_PER_SERVICE);
  rtsp_server_stop();
  audio_receiver_stop();
  esp_err_t err = rtsp_server_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "RTSP service restart failed: %s", esp_err_to_name(err));
  }
}

static void recovery_task(void *arg) {
  (void)arg;

  uint32_t previous_rx = 0;
  bool have_previous_rx = false;
  uint32_t task_loss_samples = 0;
  uint32_t stuck_samples = 0;
  uint32_t healthy_seconds = 0;
  uint32_t stream_restart_streak = 0;
  uint32_t service_restart_streak = 0;
  int64_t last_action_us = 0;
  int64_t action_grace_us = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(RECOVERY_SAMPLE_MS));

    audio_seek_diag_t diag = {0};
    audio_receiver_get_seek_diag(&diag);
    int64_t now_us = esp_timer_get_time();

    bool buffered_active =
        diag.stream_running && diag.stream_type == AUDIO_STREAM_BUFFERED;

    // Full health: the buffered stream is running and packets keep arriving.
    // An idle listener (no stream) takes no action but does not count as
    // proof of recovery either.
    bool packets_advanced =
        have_previous_rx && diag.stats.packets_received != previous_rx;
    previous_rx = diag.stats.packets_received;
    have_previous_rx = true;
    if (buffered_active && packets_advanced) {
      healthy_seconds++;
      if (healthy_seconds >= FULL_HEALTH_RESET_SECONDS &&
          (stream_restart_streak || service_restart_streak ||
           recovery_stats.level)) {
        ESP_LOGI(TAG, "Stream healthy for %us; escalation state cleared",
                 FULL_HEALTH_RESET_SECONDS);
        stream_restart_streak = 0;
        service_restart_streak = 0;
        recovery_stats.level = 0;
      }
    } else {
      healthy_seconds = 0;
    }

    if (last_action_us && now_us - last_action_us < action_grace_us) {
      task_loss_samples = 0;
      stuck_samples = 0;
      continue;
    }

    // Trigger 1: the stream claims to run but the reader or decoder task is
    // gone. Stack words are only reported for live task handles.
    bool task_loss =
        buffered_active && (diag.buffered_reader_stack_words == 0 ||
                            diag.buffered_decoder_stack_words == 0);
    task_loss_samples = task_loss ? task_loss_samples + 1 : 0;

    // Trigger 2: a sender is connected and nominally playing, yet no complete
    // packet has been read for a long time. Requiring the connected client
    // keeps a sender that simply vanished (rung 1 already closed its socket)
    // from climbing the ladder: the idle listener needs no repair.
    bool stuck = buffered_active && diag.playing &&
                 diag.buffered_client_connected &&
                 diag.buffered_last_packet_us > 0 &&
                 now_us - diag.buffered_last_packet_us > STUCK_PACKET_AGE_US;
    stuck_samples = stuck ? stuck_samples + 1 : 0;

    if (task_loss_samples < TASK_LOSS_TRIGGER_SAMPLES &&
        stuck_samples < STUCK_TRIGGER_SAMPLES) {
      continue;
    }

    if (task_loss_samples >= TASK_LOSS_TRIGGER_SAMPLES) {
      recovery_stats.task_loss_events++;
      ESP_LOGE(TAG,
               "Trigger: audio task lost (reader/decoder stack=%" PRIu32
               "/%" PRIu32 ")",
               diag.buffered_reader_stack_words,
               diag.buffered_decoder_stack_words);
    } else {
      recovery_stats.stuck_stream_events++;
      ESP_LOGE(TAG,
               "Trigger: stream stuck (packet age=%lldms, playing, "
               "client connected)",
               (long long)(now_us - diag.buffered_last_packet_us) / 1000LL);
    }
    task_loss_samples = 0;
    stuck_samples = 0;
    healthy_seconds = 0;

#ifdef CONFIG_BT_A2DP_ENABLE
    if (bt_a2dp_sink_is_connected()) {
      // BT owns the audio path right now; AirPlay teardown is in progress.
      continue;
    }
#endif

    if (stream_restart_streak < STREAM_RESTARTS_PER_SERVICE) {
      note_level(2);
      recovery_stats.stream_restarts++;
      stream_restart_streak++;
      ESP_LOGW(TAG,
               "Rung 2: restarting buffered stream (attempt %" PRIu32 "/%d)",
               stream_restart_streak, STREAM_RESTARTS_PER_SERVICE);
      esp_err_t err = audio_receiver_restart_buffered_stream();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stream restart failed: %s", esp_err_to_name(err));
      }
      last_action_us = now_us;
      action_grace_us = STREAM_RESTART_GRACE_US;
      continue;
    }

    if (service_restart_streak < SERVICE_RESTARTS_PER_REBOOT ||
        now_us < REBOOT_MIN_UPTIME_US) {
      note_level(3);
      recovery_stats.service_restarts++;
      service_restart_streak++;
      stream_restart_streak = 0;
      restart_airplay_service();
      last_action_us = now_us;
      action_grace_us = SERVICE_RESTART_GRACE_US;
      continue;
    }

    note_level(4);
    ESP_LOGE(
        TAG,
        "Rung 4: rebooting after %" PRIu32 " service restarts failed "
        "(stream_restarts=%" PRIu32 " task_loss=%" PRIu32 " stuck=%" PRIu32 ")",
        service_restart_streak, recovery_stats.stream_restarts,
        recovery_stats.task_loss_events, recovery_stats.stuck_stream_events);
    vTaskDelay(pdMS_TO_TICKS(250)); // let the log stream flush
    esp_restart();
  }
}

esp_err_t audio_recovery_start(void) {
  if (recovery_task_handle) {
    return ESP_OK;
  }
  BaseType_t result = xTaskCreate(recovery_task, "audio_recover", 4096, NULL, 2,
                                  &recovery_task_handle);
  if (result != pdPASS || !recovery_task_handle) {
    recovery_task_handle = NULL;
    ESP_LOGE(TAG, "Failed to create recovery supervisor task");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG,
           "Recovery ladder started: stuck>%llds, %d stream restarts/service, "
           "%d service restarts/reboot",
           STUCK_PACKET_AGE_US / 1000000LL, STREAM_RESTARTS_PER_SERVICE,
           SERVICE_RESTARTS_PER_REBOOT);
  return ESP_OK;
}

void audio_recovery_get_stats(audio_recovery_stats_t *stats) {
  if (!stats) {
    return;
  }
  *stats = recovery_stats;
}
