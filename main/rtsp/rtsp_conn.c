#include "rtsp_conn.h"

#include "audio_output_profile.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_receiver.h"
#include "ptp_clock.h"
#include "settings.h"

rtsp_conn_t *rtsp_conn_create(void) {
  rtsp_conn_t *conn = calloc(1, sizeof(rtsp_conn_t));
  if (!conn) {
    return NULL;
  }

  // Load saved volume or use default
  float saved_volume;
  if (settings_get_volume(&saved_volume) == ESP_OK) {
    conn->volume_db = saved_volume;
    conn->volume_q15 = audio_output_profile_volume_q15(saved_volume);
  } else {
    conn->volume_db = -15.0f; // Half volume (midpoint of -30..0 dB range)
    conn->volume_q15 = audio_output_profile_volume_q15(conn->volume_db);
  }

  conn->data_socket = -1;
  conn->control_socket = -1;
  conn->event_socket = -1;

  return conn;
}

void rtsp_conn_free(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  // Persist volume at disconnect
  settings_persist_volume();

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
  if (!conn) {
    return;
  }

  conn->volume_db = volume_db;

  conn->volume_q15 = audio_output_profile_volume_q15(volume_db);

  // Update cached volume + DAC (NVS persisted at disconnect)
  settings_set_volume(volume_db);
}

int32_t rtsp_conn_get_volume_q15(rtsp_conn_t *conn) {
  if (!conn) {
    return 32768; // Default full volume
  }
  return conn->volume_q15;
}
