#include "device_status.h"

#include "audio_arbiter.h"
#include "audio_output.h"
#include "audio_receiver.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "log_stream.h"
#include "ptp_clock.h"
#include "rtsp_events.h"
#include "rtsp_server.h"
#include "settings.h"

#include <inttypes.h>
#include <string.h>

#ifdef CONFIG_USB_AUDIO_SOURCE
#include "usb_audio_source.h"
#endif

static const char *TAG = "status";

static rtsp_metadata_t s_meta;
static int64_t s_meta_us;
static bool s_meta_valid;
static SemaphoreHandle_t s_lock;

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)user_data;
  if (!s_lock) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  switch (event) {
  case RTSP_EVENT_METADATA:
    if (data) {
      // Senders deliver title/artist/album and progress in separate
      // messages; merge so a progress-only update does not wipe the title.
      if (data->metadata.title[0]) {
        strlcpy(s_meta.title, data->metadata.title, sizeof(s_meta.title));
      }
      if (data->metadata.artist[0]) {
        strlcpy(s_meta.artist, data->metadata.artist, sizeof(s_meta.artist));
      }
      if (data->metadata.album[0]) {
        strlcpy(s_meta.album, data->metadata.album, sizeof(s_meta.album));
      }
      if (data->metadata.duration_secs) {
        s_meta.duration_secs = data->metadata.duration_secs;
        s_meta.position_secs = data->metadata.position_secs;
      }
      s_meta_us = esp_timer_get_time();
      s_meta_valid = true;
    }
    break;
  case RTSP_EVENT_DISCONNECTED:
    memset(&s_meta, 0, sizeof(s_meta));
    s_meta_valid = false;
    break;
  default:
    break;
  }
  xSemaphoreGive(s_lock);
}

esp_err_t device_status_init(void) {
  if (s_lock) {
    return ESP_OK;
  }
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) {
    return ESP_ERR_NO_MEM;
  }
  rtsp_events_register(on_rtsp_event, NULL);
  return ESP_OK;
}

static const char *source_name(audio_source_t src) {
  switch (src) {
  case AUDIO_SOURCE_AIRPLAY:
    return "airplay";
  case AUDIO_SOURCE_EXTERNAL:
    return "usb";
  default:
    return "none";
  }
}

static const char *envelope_name(int state) {
  switch (state) {
  case 1:
    return "fading_in";
  case 2:
    return "open";
  case 3:
    return "fading_out";
  default:
    return "silent";
  }
}

cJSON *device_status_build_json(void) {
  cJSON *root = cJSON_CreateObject();

  // ---- output / source -------------------------------------------------
  audio_output_stats_t out;
  audio_output_get_stats(&out);
  cJSON *output = cJSON_CreateObject();
  cJSON_AddStringToObject(output, "source", source_name(out.active_source));
  cJSON_AddStringToObject(output, "envelope",
                          envelope_name(out.envelope_state));
  cJSON_AddBoolToObject(output, "pause_pending", out.pause_pending);
  cJSON_AddNumberToObject(output, "dma_underruns", out.dma_underruns);
  cJSON_AddNumberToObject(output, "source_starved", out.source_starved);
  cJSON_AddNumberToObject(output, "fades_in", out.fades_in);
  cJSON_AddNumberToObject(output, "fades_out", out.fades_out);
  cJSON_AddNumberToObject(output, "flushes", out.flushes);
  cJSON_AddNumberToObject(output, "source_switches", out.source_switches);
  cJSON_AddNumberToObject(output, "sample_rate", CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  cJSON_AddItemToObject(root, "output", output);

  audio_arbiter_state_t arb;
  audio_arbiter_get_state(&arb);
  cJSON *arbiter = cJSON_CreateObject();
  cJSON_AddBoolToObject(arbiter, "airplay_connected", arb.airplay_connected);
  cJSON_AddBoolToObject(arbiter, "airplay_playing", arb.airplay_playing);
  cJSON_AddBoolToObject(arbiter, "release_pending", arb.release_pending);
  cJSON_AddNumberToObject(arbiter, "hand_overs", arb.hand_overs);
  cJSON_AddItemToObject(root, "arbiter", arbiter);

  // ---- AirPlay session --------------------------------------------------
  rtsp_session_info_t sess;
  cJSON *airplay = cJSON_CreateObject();
  if (rtsp_server_get_session_info(&sess)) {
    cJSON_AddBoolToObject(airplay, "connected", true);
    cJSON_AddNumberToObject(airplay, "sid", sess.sid);
    cJSON_AddStringToObject(airplay, "sender_id", sess.source_id);
    cJSON_AddStringToObject(airplay, "sender_name", sess.source_name);
    cJSON_AddStringToObject(airplay, "sender_model", sess.source_model);
    cJSON_AddNumberToObject(airplay, "volume_db", sess.volume_db);
    cJSON_AddBoolToObject(airplay, "stream_active", sess.stream_active);
    cJSON_AddBoolToObject(airplay, "paused", sess.stream_paused);
    cJSON_AddNumberToObject(airplay, "protocol", sess.protocol_version);
    cJSON_AddNumberToObject(
        airplay, "connected_s",
        (double)((esp_timer_get_time() - sess.connected_us) / 1000000LL));
  } else {
    cJSON_AddBoolToObject(airplay, "connected", false);
  }
  cJSON_AddItemToObject(root, "airplay", airplay);

  // ---- Now playing -----------------------------------------------------
  cJSON *np = cJSON_CreateObject();
  if (s_lock) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddBoolToObject(np, "valid", s_meta_valid);
    if (s_meta_valid) {
      cJSON_AddStringToObject(np, "title", s_meta.title);
      cJSON_AddStringToObject(np, "artist", s_meta.artist);
      cJSON_AddStringToObject(np, "album", s_meta.album);
      cJSON_AddNumberToObject(np, "duration_s", s_meta.duration_secs);
      cJSON_AddNumberToObject(np, "position_s", s_meta.position_secs);
      cJSON_AddNumberToObject(
          np, "age_s",
          (double)((esp_timer_get_time() - s_meta_us) / 1000000LL));
    }
    xSemaphoreGive(s_lock);
  }
  cJSON_AddItemToObject(root, "now_playing", np);

  // ---- Timing / sync ----------------------------------------------------
  audio_receiver_diag_t diag;
  audio_receiver_get_diag(&diag);
  cJSON *timing = cJSON_CreateObject();
  cJSON_AddStringToObject(timing, "domain", diag.sync_domain);
  cJSON_AddBoolToObject(timing, "anchor_valid", diag.anchor_valid);
  cJSON_AddBoolToObject(timing, "playing", diag.playing);
  cJSON_AddBoolToObject(timing, "acquired", diag.acquired);
  cJSON_AddNumberToObject(timing, "acquire_err_us",
                          (double)diag.acquire_err_us);
  cJSON_AddNumberToObject(timing, "servo_err_us", (double)diag.servo_err_us);
  cJSON_AddBoolToObject(timing, "servo_engaged", diag.servo_engaged);
  cJSON_AddNumberToObject(timing, "servo_trims", diag.servo_trims);
  cJSON_AddNumberToObject(timing, "buffered_frames", diag.buffered_frames);
  cJSON_AddNumberToObject(timing, "gaps", diag.gaps);
  cJSON_AddNumberToObject(timing, "late_frames", diag.late_frames);
  cJSON_AddNumberToObject(timing, "packets_received", diag.packets_received);
  cJSON_AddNumberToObject(timing, "packets_dropped", diag.packets_dropped);
  cJSON_AddNumberToObject(timing, "decrypt_errors", diag.decrypt_errors);
  cJSON_AddItemToObject(root, "timing", timing);

  ptp_stats_t ps;
  ptp_clock_get_stats(&ps);
  cJSON *ptp = cJSON_CreateObject();
  cJSON_AddBoolToObject(ptp, "locked", ptp_clock_is_locked());
  char clk[20];
  snprintf(clk, sizeof(clk), "%016llx",
           (unsigned long long)ptp_clock_get_tracked_clock_id());
  cJSON_AddStringToObject(ptp, "clock", clk);
  cJSON_AddNumberToObject(ptp, "sync_count", ps.sync_count);
  cJSON_AddNumberToObject(ptp, "outliers", ps.outlier_count);
  cJSON_AddNumberToObject(
      ptp, "gap_us",
      (double)((ps.last_offset_ns - ps.filtered_offset_ns) / 1000LL));
  cJSON_AddItemToObject(root, "ptp", ptp);

  // ---- USB / PC ---------------------------------------------------------
  cJSON *usb = cJSON_CreateObject();
#ifdef CONFIG_USB_AUDIO_SOURCE
  usb_audio_stats_t us;
  usb_audio_source_get_stats(&us);
  cJSON_AddBoolToObject(usb, "available", true);
  cJSON_AddStringToObject(usb, "pc_state",
                          us.host_state == USB_HOST_ACTIVE      ? "on"
                          : us.host_state == USB_HOST_SUSPENDED ? "asleep"
                                                                : "off");
  cJSON_AddBoolToObject(usb, "bus_connected", us.bus_connected);
  cJSON_AddBoolToObject(usb, "mounted", us.mounted);
  cJSON_AddBoolToObject(usb, "streaming", us.streaming);
  cJSON_AddBoolToObject(usb, "remote_wakeup_armed", us.remote_wakeup_armed);
  cJSON_AddNumberToObject(usb, "ring_ms",
                          (double)us.ring_frames * 1000.0 /
                              CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  cJSON_AddNumberToObject(usb, "ring_target_ms",
                          (double)us.ring_target * 1000.0 /
                              CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  cJSON_AddNumberToObject(usb, "underruns", us.underruns);
  cJSON_AddNumberToObject(usb, "overruns", us.overruns);
  cJSON_AddNumberToObject(usb, "trims", us.trims);
  cJSON_AddNumberToObject(usb, "discarded_kb", us.discarded_bytes / 1024);
  cJSON_AddNumberToObject(usb, "volume_q15", us.volume_q15);
  cJSON_AddBoolToObject(usb, "muted", us.muted);
#else
  cJSON_AddBoolToObject(usb, "available", false);
#endif
  char wol[24] = {0};
  if (settings_get_wol_mac(wol, sizeof(wol)) == ESP_OK) {
    cJSON_AddStringToObject(usb, "wol_mac", wol);
  } else {
    cJSON_AddStringToObject(usb, "wol_mac", "");
  }
  cJSON_AddItemToObject(root, "pc", usb);

  // ---- Firmware ----------------------------------------------------------
  cJSON *fw = cJSON_CreateObject();
  const esp_app_desc_t *app = esp_app_get_description();
  cJSON_AddStringToObject(fw, "version", app->version);
  cJSON_AddStringToObject(fw, "built", app->date);
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
    cJSON_AddStringToObject(fw, "ota_state",
                            state == ESP_OTA_IMG_PENDING_VERIFY
                                ? "pending_verify"
                            : state == ESP_OTA_IMG_VALID ? "valid"
                            : state == ESP_OTA_IMG_NEW   ? "new"
                                                         : "undefined");
    cJSON_AddStringToObject(fw, "partition", running->label);
  }
  cJSON_AddNumberToObject(fw, "uptime_s",
                          (double)(esp_timer_get_time() / 1000000LL));
  cJSON_AddNumberToObject(fw, "log_journal_kb",
                          log_stream_journal_used() / 1024);
  cJSON_AddItemToObject(root, "firmware", fw);

  cJSON_AddBoolToObject(root, "success", true);
  return root;
}

void device_status_ota_mark_valid_if_pending(const char *reason) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) {
    return;
  }
  if (state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "OTA image on %s marked valid (%s): %s", running->label,
             reason, esp_err_to_name(err));
  }
}
