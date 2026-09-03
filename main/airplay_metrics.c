#include "airplay_metrics.h"

#include <inttypes.h>

#include "audio_output.h"
#include "audio_receiver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "ap_metrics";

typedef enum {
  METRIC_ACTION_START = 0,
  METRIC_ACTION_SEEK,
  METRIC_ACTION_RESUME,
  METRIC_ACTION_DEFERRED_FLUSH,
} metric_action_t;

typedef struct {
  uint32_t active_session_id;
  int64_t connected_at_us;
  int64_t setup_at_us;
  int64_t action_at_us;
  int64_t boundary_at_us;
  int64_t deferred_requested_at_us;
  uint32_t output_underruns_at_start;
  metric_action_t action;
  volatile bool first_pcm_pending;
  volatile bool first_output_pending;
} metrics_state_t;

static portMUX_TYPE s_metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static metrics_state_t s_metrics;
static uint32_t s_next_session_id;

static const char *action_name(metric_action_t action) {
  switch (action) {
  case METRIC_ACTION_SEEK:
    return "seek";
  case METRIC_ACTION_RESUME:
    return "resume";
  case METRIC_ACTION_DEFERRED_FLUSH:
    return "deferred_flush";
  case METRIC_ACTION_START:
  default:
    return "start";
  }
}

static int64_t elapsed_ms(int64_t now_us, int64_t then_us) {
  return then_us > 0 ? (now_us - then_us) / 1000LL : -1;
}

uint32_t airplay_metrics_connection_open(uint32_t client_ip,
                                         int64_t *connected_at_us) {
  int64_t now_us = esp_timer_get_time();
  uint32_t session_id;
  portENTER_CRITICAL(&s_metrics_lock);
  session_id = ++s_next_session_id;
  if (session_id == 0) {
    session_id = ++s_next_session_id;
  }
  portEXIT_CRITICAL(&s_metrics_lock);

  if (connected_at_us) {
    *connected_at_us = now_us;
  }
  ESP_LOGI(TAG, "sid=%" PRIu32 " event=CONNECTED ip=%u.%u.%u.%u", session_id,
           (unsigned int)(client_ip & 0xFF),
           (unsigned int)((client_ip >> 8) & 0xFF),
           (unsigned int)((client_ip >> 16) & 0xFF),
           (unsigned int)((client_ip >> 24) & 0xFF));
  return session_id;
}

void airplay_metrics_initial_setup(uint32_t session_id) {
  ESP_LOGI(TAG, "sid=%" PRIu32 " event=INITIAL_SETUP", session_id);
}

void airplay_metrics_stream_setup(uint32_t session_id, int64_t connected_at_us,
                                  uint8_t protocol, int64_t stream_type,
                                  bool buffered) {
  int64_t now_us = esp_timer_get_time();
  uint32_t previous_session_id;

  portENTER_CRITICAL(&s_metrics_lock);
  previous_session_id = s_metrics.active_session_id;
  s_metrics.active_session_id = session_id;
  s_metrics.connected_at_us = connected_at_us;
  s_metrics.setup_at_us = now_us;
  s_metrics.action_at_us = now_us;
  s_metrics.boundary_at_us = 0;
  s_metrics.deferred_requested_at_us = 0;
  s_metrics.output_underruns_at_start = audio_output_get_underruns();
  s_metrics.action = METRIC_ACTION_START;
  s_metrics.first_pcm_pending = true;
  s_metrics.first_output_pending = true;
  portEXIT_CRITICAL(&s_metrics_lock);

  if (previous_session_id != 0 && previous_session_id != session_id) {
    ESP_LOGW(TAG, "sid=%" PRIu32 " event=ACTIVE_REPLACED previous_sid=%" PRIu32,
             session_id, previous_session_id);
  }
  ESP_LOGI(
      TAG,
      "sid=%" PRIu32
      " event=STREAM_SETUP protocol=%u type=%lld buffered=%d connect_ms=%lld",
      session_id, protocol, (long long)stream_type, buffered,
      (long long)elapsed_ms(now_us, connected_at_us));
}

void airplay_metrics_pause(void) {
  int64_t now_us = esp_timer_get_time();
  uint32_t session_id;
  int64_t setup_at_us;
  portENTER_CRITICAL(&s_metrics_lock);
  session_id = s_metrics.active_session_id;
  setup_at_us = s_metrics.setup_at_us;
  portEXIT_CRITICAL(&s_metrics_lock);
  if (session_id != 0) {
    ESP_LOGI(TAG, "sid=%" PRIu32 " event=PAUSE stream_ms=%lld", session_id,
             (long long)elapsed_ms(now_us, setup_at_us));
  }
}

void airplay_metrics_resume(void) {
  int64_t now_us = esp_timer_get_time();
  uint32_t session_id;
  portENTER_CRITICAL(&s_metrics_lock);
  session_id = s_metrics.active_session_id;
  if (session_id != 0) {
    s_metrics.action = METRIC_ACTION_RESUME;
    s_metrics.action_at_us = now_us;
    s_metrics.boundary_at_us = 0;
    s_metrics.first_pcm_pending = false;
    s_metrics.first_output_pending = true;
  }
  portEXIT_CRITICAL(&s_metrics_lock);
  if (session_id != 0) {
    ESP_LOGI(TAG, "sid=%" PRIu32 " event=RESUME", session_id);
  }
}

void airplay_metrics_seek_immediate(void) {
  int64_t now_us = esp_timer_get_time();
  uint32_t session_id;
  portENTER_CRITICAL(&s_metrics_lock);
  session_id = s_metrics.active_session_id;
  if (session_id != 0) {
    s_metrics.action = METRIC_ACTION_SEEK;
    s_metrics.action_at_us = now_us;
    s_metrics.boundary_at_us = 0;
    s_metrics.deferred_requested_at_us = 0;
    s_metrics.first_pcm_pending = true;
    s_metrics.first_output_pending = true;
  }
  portEXIT_CRITICAL(&s_metrics_lock);
  if (session_id != 0) {
    ESP_LOGI(TAG, "sid=%" PRIu32 " event=SEEK_IMMEDIATE", session_id);
  }
}

void airplay_metrics_deferred_flush_requested(uint32_t from_seq,
                                              uint32_t from_ts,
                                              uint32_t until_seq,
                                              uint32_t until_ts) {
  int64_t now_us = esp_timer_get_time();
  uint32_t session_id;
  portENTER_CRITICAL(&s_metrics_lock);
  session_id = s_metrics.active_session_id;
  if (session_id != 0) {
    s_metrics.deferred_requested_at_us = now_us;
  }
  portEXIT_CRITICAL(&s_metrics_lock);
  if (session_id != 0) {
    ESP_LOGI(TAG,
             "sid=%" PRIu32 " event=FLUSH_DEFERRED from_seq=%" PRIu32
             " from_ts=%" PRIu32 " until_seq=%" PRIu32 " until_ts=%" PRIu32,
             session_id, from_seq, from_ts, until_seq, until_ts);
  }
}

void airplay_metrics_deferred_flush_applied(uint32_t boundary_ts) {
  int64_t now_us = esp_timer_get_time();
  uint32_t session_id;
  int64_t requested_at_us;
  portENTER_CRITICAL(&s_metrics_lock);
  session_id = s_metrics.active_session_id;
  requested_at_us = s_metrics.deferred_requested_at_us;
  if (session_id != 0) {
    s_metrics.action = METRIC_ACTION_DEFERRED_FLUSH;
    s_metrics.action_at_us = requested_at_us > 0 ? requested_at_us : now_us;
    s_metrics.boundary_at_us = now_us;
    s_metrics.first_pcm_pending = true;
    s_metrics.first_output_pending = true;
  }
  portEXIT_CRITICAL(&s_metrics_lock);
  if (session_id != 0) {
    ESP_LOGI(TAG,
             "sid=%" PRIu32 " event=FLUSH_BOUNDARY rtp=%" PRIu32
             " request_ms=%lld",
             session_id, boundary_ts,
             (long long)elapsed_ms(now_us, requested_at_us));
  }
}

void airplay_metrics_first_pcm(uint32_t rtp_timestamp, size_t samples) {
  if (!s_metrics.first_pcm_pending) {
    return;
  }

  int64_t now_us = esp_timer_get_time();
  uint32_t session_id = 0;
  int64_t connected_at_us = 0;
  int64_t action_at_us = 0;
  metric_action_t action = METRIC_ACTION_START;
  portENTER_CRITICAL(&s_metrics_lock);
  if (s_metrics.active_session_id != 0 && s_metrics.first_pcm_pending) {
    session_id = s_metrics.active_session_id;
    connected_at_us = s_metrics.connected_at_us;
    action_at_us = s_metrics.action_at_us;
    action = s_metrics.action;
    s_metrics.first_pcm_pending = false;
  }
  portEXIT_CRITICAL(&s_metrics_lock);

  if (session_id != 0) {
    ESP_LOGI(
        TAG,
        "sid=%" PRIu32
        " event=FIRST_PCM phase=%s action_ms=%lld connect_ms=%lld rtp=%" PRIu32
        " samples=%u",
        session_id, action_name(action),
        (long long)elapsed_ms(now_us, action_at_us),
        (long long)elapsed_ms(now_us, connected_at_us), rtp_timestamp,
        (unsigned int)samples);
  }
}

void airplay_metrics_first_output(size_t frames) {
  if (!s_metrics.first_output_pending) {
    return;
  }

  int64_t now_us = esp_timer_get_time();
  uint32_t session_id = 0;
  int64_t connected_at_us = 0;
  int64_t action_at_us = 0;
  int64_t boundary_at_us = 0;
  metric_action_t action = METRIC_ACTION_START;
  portENTER_CRITICAL(&s_metrics_lock);
  if (s_metrics.active_session_id != 0 && s_metrics.first_output_pending) {
    session_id = s_metrics.active_session_id;
    connected_at_us = s_metrics.connected_at_us;
    action_at_us = s_metrics.action_at_us;
    boundary_at_us = s_metrics.boundary_at_us;
    action = s_metrics.action;
    s_metrics.first_output_pending = false;
  }
  portEXIT_CRITICAL(&s_metrics_lock);

  if (session_id != 0) {
    ESP_LOGI(TAG,
             "sid=%" PRIu32 " event=FIRST_I2S phase=%s action_ms=%lld "
                            "boundary_ms=%lld connect_ms=%lld frames=%u",
             session_id, action_name(action),
             (long long)elapsed_ms(now_us, action_at_us),
             (long long)elapsed_ms(now_us, boundary_at_us),
             (long long)elapsed_ms(now_us, connected_at_us),
             (unsigned int)frames);
  }
}

void airplay_metrics_stream_end(uint32_t session_id, const char *reason) {
  int64_t now_us = esp_timer_get_time();
  int64_t setup_at_us = 0;
  uint32_t underruns_at_start = 0;

  portENTER_CRITICAL(&s_metrics_lock);
  if (session_id != 0 && s_metrics.active_session_id == session_id) {
    setup_at_us = s_metrics.setup_at_us;
    underruns_at_start = s_metrics.output_underruns_at_start;
    s_metrics.active_session_id = 0;
    s_metrics.first_pcm_pending = false;
    s_metrics.first_output_pending = false;
    s_metrics.deferred_requested_at_us = 0;
  }
  portEXIT_CRITICAL(&s_metrics_lock);

  if (setup_at_us == 0) {
    return;
  }

  audio_stats_t stats = {0};
  audio_receiver_get_stats(&stats);
  uint32_t output_underruns = audio_output_get_underruns();
  ESP_LOGI(TAG,
           "sid=%" PRIu32
           " event=SUMMARY reason=%s duration_ms=%lld rx=%" PRIu32
           " decoded=%" PRIu32 " dropped=%" PRIu32 " decrypt=%" PRIu32
           " buf_under=%" PRIu32 " buf_over=%" PRIu32 " late=%" PRIu32
           " i2s_under=%" PRIu32,
           session_id, reason ? reason : "unknown",
           (long long)elapsed_ms(now_us, setup_at_us), stats.packets_received,
           stats.packets_decoded, stats.packets_dropped, stats.decrypt_errors,
           stats.buffer_underruns, stats.buffer_overruns, stats.late_frames,
           output_underruns - underruns_at_start);
}

void airplay_metrics_connection_closed(uint32_t session_id,
                                       int64_t connected_at_us,
                                       const char *reason) {
  airplay_metrics_stream_end(session_id, reason);
  ESP_LOGI(TAG, "sid=%" PRIu32 " event=DISCONNECTED reason=%s lifetime_ms=%lld",
           session_id, reason ? reason : "unknown",
           (long long)elapsed_ms(esp_timer_get_time(), connected_at_us));
}
