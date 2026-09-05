#include "rtsp_conn.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <inttypes.h>
#include <math.h>

#include "audio_output.h"
#include "audio_receiver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ptp_clock.h"
#include "settings.h"
#include "source_volume.h"
#include "source_volume_store.h"

static const char *TAG = "rtsp_conn";
static uint32_t s_next_sid;

// AirPlay volume: 0 dB = max, -30 dB = mute, squared curve for perceptual
// control.  Shared by the initial load and every later change.
static int32_t volume_db_to_q15(float volume_db) {
  if (volume_db <= -30.0f) {
    return 0;
  }
  if (volume_db >= 0.0f) {
    return 32768;
  }
  float normalized = (volume_db + 30.0f) / 30.0f;
  return (int32_t)(normalized * normalized * 32768.0f);
}

rtsp_conn_t *rtsp_conn_create(void) {
  rtsp_conn_t *conn = calloc(1, sizeof(rtsp_conn_t));
  if (!conn) {
    return NULL;
  }

  conn->sid = ++s_next_sid;
  conn->connected_us = esp_timer_get_time();

  // Start from the last level used on this speaker until the sender
  // identifies itself (then its own remembered level applies) or sends a
  // volume.  Snap, do not ramp: no audio has played yet.
  float saved_volume;
  if (settings_get_volume(&saved_volume) != ESP_OK) {
    saved_volume = -15.0f; // midpoint of -30..0 dB
  }
  conn->volume_db = saved_volume;
  conn->volume_q15 = volume_db_to_q15(saved_volume);
  audio_output_set_source_volume(AUDIO_SOURCE_AIRPLAY, conn->volume_q15, true);

  conn->data_socket = -1;
  conn->control_socket = -1;
  conn->event_socket = -1;

  return conn;
}

void rtsp_conn_free(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  // Persist volumes at disconnect (never during playback: an NVS write
  // stalls the cache).  Both the global "last level" and the per-sender one.
  settings_persist_volume();
  source_volume_store_persist();
  ESP_LOGI(TAG,
           "sid=%" PRIu32 " closed: requests=%" PRIu32 " lifetime=%lld ms "
           "source=%s",
           conn->sid, conn->requests,
           (long long)((esp_timer_get_time() - conn->connected_us) / 1000LL),
           conn->source_id[0] ? conn->source_id : "unknown");

  // Cleanup any resources
  rtsp_conn_cleanup(conn);

  // Free HAP session if present
  if (conn->hap_session) {
    hap_session_free(conn->hap_session);
    conn->hap_session = NULL;
  }

  free(conn);
}

void rtsp_conn_reset_stream(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  // Reset stream state but keep session alive
  conn->stream_active = false;
  conn->stream_paused = true; // Paused, not fully torn down

  // Keep ports allocated for quick resume
  // Don't clear: data_port, control_port, timing_port, event_port
}

void rtsp_conn_cleanup(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  // Note: audio_receiver_stop() is NOT called here — it is a global operation
  // and must be managed by the caller (rtsp_server cleanup / handle_teardown)
  // to avoid killing a new session's audio during client replacement.

  // Close sockets
  if (conn->data_socket >= 0) {
    close(conn->data_socket);
    conn->data_socket = -1;
  }
  if (conn->control_socket >= 0) {
    close(conn->control_socket);
    conn->control_socket = -1;
  }
  if (conn->event_socket >= 0) {
    close(conn->event_socket);
    conn->event_socket = -1;
  }

  // Reset stream state
  conn->stream_active = false;
  conn->stream_paused = false;
  conn->data_port = 0;
  conn->control_port = 0;
  conn->timing_port = 0;
  conn->event_port = 0;
  conn->buffered_port = 0;

  // Clear PTP clock for fresh sync on next connection
  ptp_clock_clear();

  // Reset encryption state
  conn->encrypted_mode = false;
}

void rtsp_conn_set_volume(rtsp_conn_t *conn, float volume_db) {
  if (!conn || !isfinite(volume_db)) {
    return;
  }
  // AirPlay senders use -144 dB for "mute"; anything below the -30 dB floor
  // is silence.
  if (volume_db < -30.0f) {
    volume_db = -30.0f;
  }
  if (volume_db > 0.0f) {
    volume_db = 0.0f;
  }

  conn->volume_db = volume_db;
  conn->volume_q15 = volume_db_to_q15(volume_db);
  conn->volume_from_sender = true;

  // Ramped by the render task's envelope (no zipper), remembered per sender.
  audio_output_set_source_volume(AUDIO_SOURCE_AIRPLAY, conn->volume_q15, false);
  if (conn->source_id[0]) {
    source_volume_store_set(conn->source_id, volume_db);
  }
  ESP_LOGI(TAG, "sid=%" PRIu32 " volume %.2f dB (q15=%" PRId32 ") source=%s",
           conn->sid, volume_db, conn->volume_q15,
           conn->source_id[0] ? conn->source_id : "unknown");

  // Update cached volume + DAC (NVS persisted at disconnect)
  settings_set_volume(volume_db);
}

void rtsp_conn_identify_source(rtsp_conn_t *conn, const char *device_id,
                               const char *name, const char *model) {
  if (!conn || conn->source_id[0]) {
    return; // identity is bound once per connection
  }
  char id[sizeof(conn->source_id)];
  if (!source_volume_normalize_id(device_id, id, sizeof(id))) {
    ESP_LOGW(TAG, "sid=%" PRIu32 " sender id rejected: '%s'", conn->sid,
             device_id ? device_id : "(null)");
    return;
  }
  strlcpy(conn->source_id, id, sizeof(conn->source_id));
  if (name) {
    strlcpy(conn->source_name, name, sizeof(conn->source_name));
  }
  if (model) {
    strlcpy(conn->source_model, model, sizeof(conn->source_model));
  }

  float remembered;
  if (!conn->volume_from_sender &&
      source_volume_store_get(conn->source_id, &remembered)) {
    conn->volume_db = remembered;
    conn->volume_q15 = volume_db_to_q15(remembered);
    audio_output_set_source_volume(AUDIO_SOURCE_AIRPLAY, conn->volume_q15,
                                   true);
    settings_set_volume(remembered);
    ESP_LOGI(TAG, "sid=%" PRIu32 " sender %s (%s, %s): restored %.2f dB",
             conn->sid, conn->source_id, conn->source_name, conn->source_model,
             remembered);
  } else {
    if (conn->volume_from_sender) {
      source_volume_store_set(conn->source_id, conn->volume_db);
    }
    ESP_LOGI(TAG, "sid=%" PRIu32 " sender %s (%s, %s): %s %.2f dB", conn->sid,
             conn->source_id, conn->source_name, conn->source_model,
             conn->volume_from_sender ? "keeping sender's" : "no memory, using",
             conn->volume_db);
  }
}

int32_t rtsp_conn_get_volume_q15(rtsp_conn_t *conn) {
  if (!conn) {
    return 32768; // Default full volume
  }
  return conn->volume_q15;
}
