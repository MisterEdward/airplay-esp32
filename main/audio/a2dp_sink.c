/**
 * Bluetooth A2DP Sink implementation for ESP32.
 *
 * Uses the ESP-IDF Bluedroid stack to receive A2DP audio (SBC codec,
 * decoded internally to PCM) and play it through the shared I2S output.
 *
 * Architecture:
 *   BT stack → data callback → ringbuffer → I2S writer task → I2S DMA
 *
 * Integration with AirPlay:
 *   - On BT connect:    notify main → stop AirPlay services
 *   - On BT disconnect: notify main → restart AirPlay services
 *   - LED/display updates via rtsp_events (reuses the same event types)
 *   - AVRCP metadata → RTSP_EVENT_METADATA for display
 */

#include "a2dp_sink.h"
#include "spiram_task.h"

#include "audio_output.h"
#include "dac.h"
#include "led.h"
#include "rtsp_events.h"
#include "settings.h"

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "bt_a2dp";

/* ========================================================================== */
/* Configuration                                                              */
/* ========================================================================== */

#define RINGBUF_SIZE     ((size_t)16 * 1024)
#define RINGBUF_PREFETCH (RINGBUF_SIZE * 6 / 10)

#define BT_TASK_STACK 4096
#define BT_TASK_PRIO  (configMAX_PRIORITIES - 3)
#define BT_TASK_QLEN  10

#define I2S_TASK_STACK 2560
#define I2S_TASK_PRIO  7

#if CONFIG_FREERTOS_UNICORE
#define I2S_TASK_CORE 0
#else
#define I2S_TASK_CORE 1
#endif

/* ========================================================================== */
/* BT App Task Types                                                          */
/* ========================================================================== */

typedef void (*bt_app_cb_t)(uint16_t event, void *param);

typedef struct {
  uint16_t sig;
  uint16_t event;
  bt_app_cb_t cb;
  void *param;
} bt_app_msg_t;

enum {
  BT_APP_SIG_WORK_DISPATCH = 0,
  BT_APP_SIG_STOP,
};
enum { BT_APP_EVT_STACK_UP = 0 };

/* ========================================================================== */
/* Ringbuffer mode                                                            */
/* ========================================================================== */

typedef enum {
  RINGBUF_MODE_PREFETCHING,
  RINGBUF_MODE_PROCESSING,
  RINGBUF_MODE_DROPPING,
} ringbuf_mode_t;

/* ========================================================================== */
/* State                                                                      */
/* ========================================================================== */

static bt_a2dp_state_cb_t s_state_cb = NULL;
static volatile bool s_connected = false;
static volatile bool s_audio_started = false;
static volatile bool s_avrc_playing = false; /* AVRCP play state (instant) */
static volatile bool s_i2s_task_running = false;
static volatile bool s_bt_running = false;
static bool s_ble_mem_released = false;
static bool s_bt_discoverable = true;
static uint8_t s_avrc_volume = 64; /* 0-127, AVRCP absolute volume */
static volatile bool s_vol_ntf_pending =
    false; /* phone registered for volume change */

static RingbufHandle_t s_ringbuf = NULL;
static SemaphoreHandle_t s_i2s_sem = NULL;
static ringbuf_mode_t s_ringbuf_mode = RINGBUF_MODE_PREFETCHING;

static TaskHandle_t s_bt_task_handle = NULL;
static TaskHandle_t s_i2s_task_handle = NULL;
static QueueHandle_t s_bt_task_queue = NULL;
static SemaphoreHandle_t s_a2dp_deinit_sem = NULL;

static uint32_t s_sample_rate = 44100;

// Device name cached at init so bt_stack_evt_handler (which runs on the BT
// app task, not the caller's task) can call esp_bt_gap_set_device_name after
// the GAP callback is registered, rather than before stack-up (where the call
// can be silently dropped by Bluedroid on a re-init path).
static char s_device_name[64] = {0};

/* ========================================================================== */
/* BT App Task — context-switch from BT stack to app task                     */
/* ========================================================================== */

static bool bt_app_send_msg(bt_app_msg_t *msg) {
  if (s_bt_task_queue == NULL) {
    return false;
  }
  return xQueueSend(s_bt_task_queue, msg, pdMS_TO_TICKS(10)) == pdTRUE;
}

static bool bt_app_work_dispatch(bt_app_cb_t cb, uint16_t event, void *param,
                                 int param_len) {
  bt_app_msg_t msg = {
      .sig = BT_APP_SIG_WORK_DISPATCH,
      .event = event,
      .cb = cb,
      .param = NULL,
  };
  if (param_len > 0 && param != NULL) {
    msg.param = malloc((size_t)param_len);
    if (!msg.param) {
      return false;
    }
    memcpy(msg.param, param, (size_t)param_len);
  }
  if (!bt_app_send_msg(&msg)) {
    free(msg.param);
    return false;
  }
  return true;
}

static void bt_app_task(void *arg) {
  bt_app_msg_t msg;
  while (true) {
    if (xQueueReceive(s_bt_task_queue, &msg, portMAX_DELAY) == pdTRUE) {
      if (msg.sig == BT_APP_SIG_STOP) {
        free(msg.param);
        break;
      }
      if (msg.sig == BT_APP_SIG_WORK_DISPATCH && msg.cb) {
        msg.cb(msg.event, msg.param);
      }
      free(msg.param);
    }
  }
  s_bt_task_handle = NULL;
  vTaskDelete(NULL);
}

/* ========================================================================== */
/* I2S Writer Task — reads ringbuffer, writes to I2S                          */
/* ========================================================================== */

static void bt_i2s_writer_task(void *arg) {
  // Wait for ringbuffer to reach prefetch level
  xSemaphoreTake(s_i2s_sem, portMAX_DELAY);

  int16_t silence[256] = {0};

  while (s_i2s_task_running) {
    size_t item_size = 0;
    void *data =
        xRingbufferReceiveUpTo(s_ringbuf, &item_size, pdMS_TO_TICKS(20), 512);
    if (data != NULL && item_size > 0) {
      audio_output_write(data, item_size, portMAX_DELAY);
      vRingbufferReturnItem(s_ringbuf, data);
    } else {
      // Buffer underrun — write silence to keep I2S fed
      audio_output_write(silence, sizeof(silence), pdMS_TO_TICKS(10));
    }
  }

  s_i2s_task_handle = NULL;
  vTaskDelete(NULL);
}

static void i2s_task_start(void) {
  if (s_i2s_task_handle != NULL) {
    return;
  }

  s_ringbuf_mode = RINGBUF_MODE_PREFETCHING;

  if (s_ringbuf == NULL) {
    s_ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
  }
  if (s_i2s_sem == NULL) {
    s_i2s_sem = xSemaphoreCreateBinary();
  }

  s_i2s_task_running = true;
  xTaskCreatePinnedToCore(bt_i2s_writer_task, "bt_i2s", I2S_TASK_STACK, NULL,
                          I2S_TASK_PRIO, &s_i2s_task_handle, I2S_TASK_CORE);
}

static void i2s_task_stop(void) {
  s_i2s_task_running = false;

  // Unblock the writer if it's waiting on the semaphore
  if (s_i2s_sem) {
    xSemaphoreGive(s_i2s_sem);
  }

  // Wait for the task to exit
  int timeout = 20;
  while (s_i2s_task_handle != NULL && timeout-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // Flush and free the ringbuffer so heap is returned before AirPlay restarts
  if (s_ringbuf) {
    void *data;
    size_t size;
    while ((data = xRingbufferReceive(s_ringbuf, &size, 0)) != NULL) {
      vRingbufferReturnItem(s_ringbuf, data);
    }
    vRingbufferDelete(s_ringbuf);
    s_ringbuf = NULL;
  }

  if (s_i2s_sem) {
    vSemaphoreDelete(s_i2s_sem);
    s_i2s_sem = NULL;
  }

  // Reset sample rate to AirPlay default
  audio_output_set_sample_rate(44100);
  audio_output_flush();
}

/* ========================================================================== */
/* A2DP data callback — BT stack context → ringbuffer                         */
/* ========================================================================== */

static void bt_a2dp_data_cb(const uint8_t *data, uint32_t len) {
  // Snapshot the ringbuf handle so it can't be freed between the NULL
  // check and the API calls (i2s_task_stop sets s_ringbuf = NULL).
  RingbufHandle_t rb = s_ringbuf;
  if (rb == NULL || len == 0) {
    return;
  }

  // Feed LED VU meter from the decoded PCM
  led_audio_feed((const int16_t *)data, len / 4);

  size_t free_size = xRingbufferGetCurFreeSize(rb);

  switch (s_ringbuf_mode) {
  case RINGBUF_MODE_PREFETCHING:
    xRingbufferSend(rb, data, len, 0);
    if ((RINGBUF_SIZE - free_size + len) >= RINGBUF_PREFETCH) {
      s_ringbuf_mode = RINGBUF_MODE_PROCESSING;
      xSemaphoreGive(s_i2s_sem);
    }
    break;

  case RINGBUF_MODE_PROCESSING:
    if (free_size < (size_t)len) {
      s_ringbuf_mode = RINGBUF_MODE_DROPPING;
      ESP_LOGW(TAG, "Ringbuf full, dropping");
    } else {
      xRingbufferSend(rb, data, len, 0);
    }
    break;

  case RINGBUF_MODE_DROPPING:
    if (free_size > RINGBUF_PREFETCH) {
      s_ringbuf_mode = RINGBUF_MODE_PROCESSING;
      xRingbufferSend(rb, data, len, 0);
    }
    break;
  }
}

/* ========================================================================== */
/* A2DP event handler (runs in app task context) */
/* ========================================================================== */

static void bt_a2dp_evt_handler(uint16_t event, void *param) {
  esp_a2d_cb_param_t *a2d = (esp_a2d_cb_param_t *)param;

  switch (event) {
  case ESP_A2D_CONNECTION_STATE_EVT: {
    esp_a2d_connection_state_t state = a2d->conn_stat.state;

    if (state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
      ESP_LOGI(TAG, "A2DP connecting...");
    } else if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
      ESP_LOGI(TAG, "A2DP connected");
      s_connected = true;

      // Notify main app to disable AirPlay (stops playback task + RTSP)
      if (s_state_cb) {
        s_state_cb(true);
      }

      // Start BT playback
      i2s_task_start();

      // LED/display: show connected state
      rtsp_events_emit(RTSP_EVENT_CLIENT_CONNECTED, NULL);

      // Don't accept more connections while connected
      esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTING) {
      ESP_LOGI(TAG, "A2DP disconnecting...");
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
      ESP_LOGI(TAG, "A2DP disconnected");
      s_connected = false;
      s_audio_started = false;
      s_avrc_playing = false;

      // Stop BT playback
      i2s_task_stop();

      // LED/display: show disconnected state
      rtsp_events_emit(RTSP_EVENT_DISCONNECTED, NULL);

      // Notify main app to re-enable AirPlay (restarts playback + RTSP)
      if (s_state_cb) {
        s_state_cb(false);
      }

      // Discoverable state is managed by main via set_discoverable()

      // Persist the last-used volume to NVS once at disconnect
      settings_persist_bt_volume();
    }
    break;
  }

  case ESP_A2D_AUDIO_STATE_EVT: {
    esp_a2d_audio_state_t state = a2d->audio_stat.state;
    if (state == ESP_A2D_AUDIO_STATE_STARTED) {
      ESP_LOGI(TAG, "Audio stream started");
      s_audio_started = true;
      rtsp_events_emit(RTSP_EVENT_PLAYING, NULL);
    } else {
      ESP_LOGI(TAG, "Audio stream stopped/suspended");
      s_audio_started = false;
      rtsp_events_emit(RTSP_EVENT_PAUSED, NULL);
    }
    break;
  }

  case ESP_A2D_AUDIO_CFG_EVT: {
    ESP_LOGI(TAG, "Audio codec configured, type=%d", a2d->audio_cfg.mcc.type);
    if (a2d->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
      // Use the new struct-based CIE accessors
      uint8_t sf = a2d->audio_cfg.mcc.cie.sbc_info.samp_freq;
      if (sf & ESP_A2D_SBC_CIE_SF_48K) {
        s_sample_rate = 48000;
      } else if (sf & ESP_A2D_SBC_CIE_SF_44K) {
        s_sample_rate = 44100;
      } else if (sf & ESP_A2D_SBC_CIE_SF_32K) {
        s_sample_rate = 32000;
      } else if (sf & ESP_A2D_SBC_CIE_SF_16K) {
        s_sample_rate = 16000;
      }
      ESP_LOGI(TAG, "SBC sample rate: %" PRIu32 " Hz", s_sample_rate);
      audio_output_set_sample_rate(s_sample_rate);
    }
    break;
  }

  case ESP_A2D_PROF_STATE_EVT:
    ESP_LOGI(TAG, "A2DP profile %s",
             a2d->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS
                 ? "inited"
                 : "deinited");
    if (a2d->a2d_prof_stat.init_state == ESP_A2D_DEINIT_SUCCESS &&
        s_a2dp_deinit_sem) {
      xSemaphoreGive(s_a2dp_deinit_sem);
    }
    break;

  default:
    break;
  }
}

/* A2DP callback (BT stack context) → dispatch to app task */
static void bt_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  bt_app_work_dispatch(bt_a2dp_evt_handler, (uint16_t)event, param,
                       sizeof(esp_a2d_cb_param_t));
}

/* ========================================================================== */
/* AVRCP Controller — get metadata from source device                         */
/* ========================================================================== */

static void bt_avrc_ct_evt_handler(uint16_t event, void *param) {
  esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)param;
  static rtsp_event_data_t meta_data;

  switch (event) {
  case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    ESP_LOGI(TAG, "AVRC CT %s",
             rc->conn_stat.connected ? "connected" : "disconnected");
    if (rc->conn_stat.connected) {
      // Request track metadata
      esp_avrc_ct_send_metadata_cmd(
          0, ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
                 ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_GENRE);
      // Request current play status (duration + position + play state)
      esp_avrc_ct_send_get_play_status_cmd(0);
      // Register for notifications: track change, play status, position
      esp_avrc_ct_send_register_notification_cmd(1, ESP_AVRC_RN_TRACK_CHANGE,
                                                 0);
      esp_avrc_ct_send_register_notification_cmd(
          2, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
      esp_avrc_ct_send_register_notification_cmd(3,
                                                 ESP_AVRC_RN_PLAY_POS_CHANGED,
                                                 10); // report every 10 seconds
    }
    break;

  case ESP_AVRC_CT_METADATA_RSP_EVT: {
    // Metadata response — one attribute per event.
    // attr_text was deep-copied in bt_avrc_ct_cb and must be freed here.
    uint8_t attr_id = rc->meta_rsp.attr_id;
    uint8_t *text = rc->meta_rsp.attr_text;
    int len = rc->meta_rsp.attr_length;

    if (text == NULL || len == 0) {
      free(text);
      break;
    }

    // Build metadata event for display
    char *dst = NULL;
    size_t dst_size = 0;

    switch (attr_id) {
    case ESP_AVRC_MD_ATTR_TITLE:
      dst = meta_data.metadata.title;
      dst_size = sizeof(meta_data.metadata.title);
      break;
    case ESP_AVRC_MD_ATTR_ARTIST:
      dst = meta_data.metadata.artist;
      dst_size = sizeof(meta_data.metadata.artist);
      break;
    case ESP_AVRC_MD_ATTR_ALBUM:
      dst = meta_data.metadata.album;
      dst_size = sizeof(meta_data.metadata.album);
      break;
    case ESP_AVRC_MD_ATTR_GENRE:
      dst = meta_data.metadata.genre;
      dst_size = sizeof(meta_data.metadata.genre);
      break;
    default:
      break;
    }

    if (dst) {
      size_t copy_len = (size_t)len < dst_size - 1 ? (size_t)len : dst_size - 1;
      memcpy(dst, text, copy_len);
      dst[copy_len] = '\0';
      ESP_LOGI(TAG, "Metadata attr %d: %s", attr_id, dst);

      // Emit metadata event after each attribute update
      rtsp_events_emit(RTSP_EVENT_METADATA, &meta_data);
    }
    free(text);
    break;
  }

  case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT: {
    uint32_t duration_ms = rc->play_status_rsp.song_length;
    uint32_t position_ms = rc->play_status_rsp.song_position;
    esp_avrc_playback_stat_t status = rc->play_status_rsp.play_status;

    ESP_LOGI(TAG, "Play status: state=%d pos=%lums dur=%lums", status,
             (unsigned long)position_ms, (unsigned long)duration_ms);

    // Update duration and position in metadata for display
    // 0xFFFFFFFF means "not available" per AVRCP spec
    if (duration_ms != 0xFFFFFFFF) {
      meta_data.metadata.duration_secs = duration_ms / 1000;
    }
    if (position_ms != 0xFFFFFFFF) {
      meta_data.metadata.position_secs = position_ms / 1000;
    }
    rtsp_events_emit(RTSP_EVENT_METADATA, &meta_data);

    // Emit play state from AVRCP (faster than A2D audio state)
    if (status == ESP_AVRC_PLAYBACK_PLAYING) {
      s_avrc_playing = true;
      rtsp_events_emit(RTSP_EVENT_PLAYING, NULL);
    } else if (status == ESP_AVRC_PLAYBACK_PAUSED ||
               status == ESP_AVRC_PLAYBACK_STOPPED) {
      s_avrc_playing = false;
      rtsp_events_emit(RTSP_EVENT_PAUSED, NULL);
    }
    break;
  }

  case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    if (rc->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
      ESP_LOGI(TAG, "Track changed");
      // Clear old metadata so stale fields don't persist
      memset(&meta_data, 0, sizeof(meta_data));

      // Request new metadata and play status
      esp_avrc_ct_send_metadata_cmd(
          0, ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
                 ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_GENRE);
      esp_avrc_ct_send_get_play_status_cmd(0);
      // Re-register for track change
      esp_avrc_ct_send_register_notification_cmd(1, ESP_AVRC_RN_TRACK_CHANGE,
                                                 0);
    } else if (rc->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
      esp_avrc_playback_stat_t status = rc->change_ntf.event_parameter.playback;
      ESP_LOGI(TAG, "Play status changed: %d", status);

      // Refresh position/duration from source
      esp_avrc_ct_send_get_play_status_cmd(0);

      if (status == ESP_AVRC_PLAYBACK_PLAYING) {
        s_avrc_playing = true;
        rtsp_events_emit(RTSP_EVENT_PLAYING, NULL);
      } else if (status == ESP_AVRC_PLAYBACK_PAUSED ||
                 status == ESP_AVRC_PLAYBACK_STOPPED) {
        s_avrc_playing = false;
        rtsp_events_emit(RTSP_EVENT_PAUSED, NULL);
      }
      // Re-register
      esp_avrc_ct_send_register_notification_cmd(
          2, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
    } else if (rc->change_ntf.event_id == ESP_AVRC_RN_PLAY_POS_CHANGED) {
      uint32_t pos_ms = rc->change_ntf.event_parameter.play_pos;
      ESP_LOGD(TAG, "Play position: %lums", (unsigned long)pos_ms);

      if (pos_ms != 0xFFFFFFFF) {
        meta_data.metadata.position_secs = pos_ms / 1000;
        rtsp_events_emit(RTSP_EVENT_METADATA, &meta_data);
      }
      // Re-register
      esp_avrc_ct_send_register_notification_cmd(
          3, ESP_AVRC_RN_PLAY_POS_CHANGED, 10);
    }
    break;

  default:
    break;
  }
}

static void bt_avrc_ct_cb(esp_avrc_ct_cb_event_t event,
                          esp_avrc_ct_cb_param_t *param) {
  esp_avrc_ct_cb_param_t local = *param;

  // Deep-copy attr_text for metadata responses — the BT stack frees the
  // original buffer after this callback returns, so the shallow copy done
  // by bt_app_work_dispatch would leave a dangling pointer.
  if (event == ESP_AVRC_CT_METADATA_RSP_EVT && param->meta_rsp.attr_text &&
      param->meta_rsp.attr_length > 0) {
    uint8_t *copy = malloc((size_t)param->meta_rsp.attr_length + 1);
    if (copy) {
      memcpy(copy, param->meta_rsp.attr_text,
             (size_t)param->meta_rsp.attr_length);
      copy[param->meta_rsp.attr_length] = '\0';
      local.meta_rsp.attr_text = copy;
    }
  }

  bt_app_work_dispatch(bt_avrc_ct_evt_handler, (uint16_t)event, &local,
                       sizeof(esp_avrc_ct_cb_param_t));
}

/* ========================================================================== */
/* AVRCP Target — handle volume commands from source device                   */
/* ========================================================================== */

static void bt_avrc_tg_evt_handler(uint16_t event, void *param) {
  esp_avrc_tg_cb_param_t *tg = (esp_avrc_tg_cb_param_t *)param;

  switch (event) {
  case ESP_AVRC_TG_CONNECTION_STATE_EVT:
    ESP_LOGI(TAG, "AVRC TG %s",
             tg->conn_stat.connected ? "connected" : "disconnected");
    break;

  case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
    uint8_t volume = tg->set_abs_vol.volume; // 0-127
    s_avrc_volume = volume;
    ESP_LOGD(TAG, "Set absolute volume: %d/127", volume);

    // Map 0-127 → -30..0 dB and apply to DAC
    // dac_set_volume does I2C — safe here because bt_app_task dispatches
    // sequentially, but keep it lightweight
    float volume_db = ((float)volume / 127.0f) * 30.0f - 30.0f;
    dac_set_volume(volume_db);
    break;
  }

  case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
    if (tg->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
      // Respond with current volume (interim response)
      esp_avrc_rn_param_t rn_param;
      rn_param.volume = s_avrc_volume;
      esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE,
                              ESP_AVRC_RN_RSP_INTERIM, &rn_param);
      s_vol_ntf_pending = true;
    }
    break;

  default:
    break;
  }
}

static void bt_avrc_tg_cb(esp_avrc_tg_cb_event_t event,
                          esp_avrc_tg_cb_param_t *param) {
  bt_app_work_dispatch(bt_avrc_tg_evt_handler, (uint16_t)event, param,
                       sizeof(esp_avrc_tg_cb_param_t));
}

/* ========================================================================== */
/* GAP callback                                                               */
/* ========================================================================== */

static void bt_gap_cb(esp_bt_gap_cb_event_t event,
                      esp_bt_gap_cb_param_t *param) {
  switch (event) {
  case ESP_BT_GAP_AUTH_CMPL_EVT:
    if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
      ESP_LOGI(TAG, "Auth success: %s", param->auth_cmpl.device_name);
    } else {
      ESP_LOGW(TAG, "Auth failed: status=%d", param->auth_cmpl.stat);
    }
    break;

  case ESP_BT_GAP_CFM_REQ_EVT:
    ESP_LOGI(TAG, "SSP confirm request (num=%" PRIu32 "), auto-accepting",
             param->cfm_req.num_val);
    esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
    break;

  case ESP_BT_GAP_PIN_REQ_EVT:
    ESP_LOGI(TAG, "Legacy PIN request");
    break;

  case ESP_BT_GAP_KEY_NOTIF_EVT:
    ESP_LOGI(TAG, "SSP passkey notify: %" PRIu32, param->key_notif.passkey);
    break;

  case ESP_BT_GAP_KEY_REQ_EVT:
    ESP_LOGI(TAG, "SSP passkey request");
    break;

  case ESP_BT_GAP_MODE_CHG_EVT:
    ESP_LOGD(TAG, "GAP mode changed to %d", param->mode_chg.mode);
    break;

  default:
    break;
  }
}

/* ========================================================================== */
/* Stack-up handler — called when Bluedroid is (re-)enabled                   */
/* ========================================================================== */

static void bt_stack_evt_handler(uint16_t event, void *param) {
  (void)param;

  if (event != BT_APP_EVT_STACK_UP) {
    return;
  }

  ESP_LOGI(TAG, "BT stack up, initializing profiles");

  // GAP — register callback first, then configure.  Device name is set here
  // (not before bluedroid_enable) because GAP API calls issued before the
  // callback is registered can be silently dropped by Bluedroid if the stack
  // isn't fully ready, causing the device to appear with a stale name after
  // an AirPlay→BT handoff re-init.  s_device_name was cached at init time.
  esp_bt_gap_register_callback(bt_gap_cb);
  esp_bt_gap_set_device_name(s_device_name);

#ifdef CONFIG_BT_SSP_ENABLED
  // Secure Simple Pairing — numeric comparison for BT 2.1+ devices
  esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
  esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
  esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

  // Legacy Pairing — fixed PIN from Kconfig
  esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
  esp_bt_pin_code_t pin_code;
  const char *pin_str = CONFIG_BT_PIN_CODE;
  uint8_t pin_len = strlen(pin_str);
  if (pin_len > ESP_BT_PIN_CODE_LEN) {
    pin_len = ESP_BT_PIN_CODE_LEN;
  }
  memcpy(pin_code, pin_str, pin_len);
  {
    esp_err_t pin_err = esp_bt_gap_set_pin(pin_type, pin_len, pin_code);
    if (pin_err != ESP_OK) {
      ESP_LOGW(TAG, "BT set_pin failed: %s", esp_err_to_name(pin_err));
    } else {
      ESP_LOGI(TAG, "BT PIN set (%d digits)", pin_len);
    }
  }

  // AVRC Controller (to get metadata from source)
  esp_avrc_ct_register_callback(bt_avrc_ct_cb);
  esp_avrc_ct_init();

  // AVRC Target (to receive volume commands)
  esp_avrc_tg_register_callback(bt_avrc_tg_cb);
  esp_avrc_tg_init();
  esp_avrc_rn_evt_cap_mask_t evt_set = {0};
  evt_set.bits = (1 << ESP_AVRC_RN_VOLUME_CHANGE);
  esp_avrc_tg_set_rn_evt_cap(&evt_set);

  // A2DP Sink
  esp_a2d_register_callback(bt_a2dp_cb);
  esp_a2d_sink_register_data_callback(bt_a2dp_data_cb);
  esp_a2d_sink_init();

  // Apply saved discoverable state.  A 100 ms delay is inserted before the
  // set_scan_mode call because after a full controller deinit + re-init (the
  // AirPlay→BT handoff path), the BR/EDR inquiry/page scheduler can take a
  // few tens of ms to be ready to honour scan-mode changes.  The boot path
  // does not need this (stack is fresh), but the re-init path can silently
  // drop the call without it.
  vTaskDelay(pdMS_TO_TICKS(100));
  ESP_LOGI(TAG, "BT scan mode: %s",
           s_bt_discoverable ? "discoverable" : "hidden");
  {
    esp_err_t scan_err =
        s_bt_discoverable
            ? esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                       ESP_BT_GENERAL_DISCOVERABLE)
            : esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
                                       ESP_BT_NON_DISCOVERABLE);
    if (scan_err != ESP_OK) {
      ESP_LOGW(TAG, "BT set_scan_mode failed: %s", esp_err_to_name(scan_err));
    }
  }

  // Restore saved BT volume (falls back to 64 / -15 dB if none stored)
  {
    uint8_t saved_vol;
    if (settings_get_bt_volume(&saved_vol) == ESP_OK) {
      s_avrc_volume = saved_vol;
    }
    float volume_db = ((float)s_avrc_volume / 127.0f) * 30.0f - 30.0f;
    dac_set_volume(volume_db);
    ESP_LOGI(TAG, "BT volume restored: %d/127 (%.1f dB)", s_avrc_volume,
             volume_db);
  }

  ESP_LOGI(TAG, "BT A2DP sink ready, waiting for connections");
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

esp_err_t bt_a2dp_sink_init(const char *device_name,
                            bt_a2dp_state_cb_t state_cb) {
  s_state_cb = state_cb;
  // Cache device name for use in bt_stack_evt_handler (see s_device_name).
  snprintf(s_device_name, sizeof(s_device_name), "%s", device_name ? device_name : "");

  if (s_bt_running) {
    bt_a2dp_sink_set_discoverable(s_bt_discoverable);
    return ESP_OK;
  }

  // On the re-init path (stop→start after AirPlay session), s_bt_discoverable
  // may be false because bt_a2dp_sink_stop called set_discoverable(false) to
  // hide BT during AirPlay streaming.  Reset it here so bt_stack_evt_handler
  // (the async init completion callback) makes BT discoverable on start.
  // This mirrors the boot path where s_bt_discoverable starts as true.
  s_bt_discoverable = true;
  ESP_LOGI(TAG, "BT discoverable enabled");

  ESP_LOGI(TAG, "Initializing Bluetooth A2DP Sink: %s", device_name);

  // Release BLE memory — we only need Classic BT
  if (!s_ble_mem_released) {
    esp_err_t rel_err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (rel_err != ESP_OK && rel_err != ESP_ERR_NOT_FOUND) {
      ESP_LOGW(TAG, "BLE memory release failed: %s",
               esp_err_to_name(rel_err));
    } else {
      s_ble_mem_released = true;
    }
  }

  // Create the BT app task and event queue before profiles can emit callbacks.
  s_bt_task_queue = xQueueCreate(BT_TASK_QLEN, sizeof(bt_app_msg_t));
  if (s_bt_task_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create BT task queue");
    return ESP_ERR_NO_MEM;
  }

  BaseType_t task_ok =
      task_create_spiram(bt_app_task, "bt_app", BT_TASK_STACK, NULL,
                         BT_TASK_PRIO, &s_bt_task_handle, NULL);
  if (task_ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create BT app task");
    vQueueDelete(s_bt_task_queue);
    s_bt_task_queue = NULL;
    return ESP_ERR_NO_MEM;
  }

  s_a2dp_deinit_sem = xSemaphoreCreateBinary();
  if (s_a2dp_deinit_sem == NULL) {
    ESP_LOGE(TAG, "Failed to create A2DP deinit semaphore");
    bt_a2dp_sink_stop();
    return ESP_ERR_NO_MEM;
  }

  // Initialize BT controller
  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_bt_controller_init(&bt_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(err));
    bt_a2dp_sink_stop();
    return err;
  }

  err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(err));
    bt_a2dp_sink_stop();
    return err;
  }

  // Initialize Bluedroid
  esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
#ifndef CONFIG_BT_SSP_ENABLED
  bluedroid_cfg.ssp_en = false; // Disable SSP to force legacy PIN pairing
#endif
  err = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(err));
    bt_a2dp_sink_stop();
    return err;
  }

  err = esp_bluedroid_enable();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(err));
    bt_a2dp_sink_stop();
    return err;
  }

  // Device name is set inside bt_stack_evt_handler (after
  // esp_bt_gap_register_callback), not here.  Calling it before the callback
  // is registered can be silently dropped by Bluedroid on a re-init path.

  // Dispatch stack-up event to app task
  bt_app_work_dispatch(bt_stack_evt_handler, BT_APP_EVT_STACK_UP, NULL, 0);

  s_bt_running = true;
  return ESP_OK;
}

esp_err_t bt_a2dp_sink_stop(void) {
  if (!s_bt_running &&
      esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE &&
      esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED &&
      s_bt_task_queue == NULL && s_a2dp_deinit_sem == NULL) {
    return ESP_OK;
  }

  if (s_connected) {
    ESP_LOGW(TAG, "Refusing to stop BT while an A2DP client is connected");
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(TAG, "Stopping Bluetooth A2DP Sink");

  esp_bluedroid_status_t bt_stack_status = esp_bluedroid_get_status();
  if (bt_stack_status == ESP_BLUEDROID_STATUS_ENABLED) {
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    i2s_task_stop();

    if (s_a2dp_deinit_sem) {
      xSemaphoreTake(s_a2dp_deinit_sem, 0);
    }
    esp_err_t err = esp_avrc_ct_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "AVRCP CT deinit failed: %s", esp_err_to_name(err));
    }
    err = esp_avrc_tg_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "AVRCP TG deinit failed: %s", esp_err_to_name(err));
    }
    err = esp_a2d_sink_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "A2DP sink deinit failed: %s", esp_err_to_name(err));
    } else if (s_a2dp_deinit_sem) {
      xSemaphoreTake(s_a2dp_deinit_sem, pdMS_TO_TICKS(1000));
    }

    err = esp_bluedroid_disable();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Bluedroid disable failed: %s", esp_err_to_name(err));
    }
    bt_stack_status = esp_bluedroid_get_status();
  }

  if (bt_stack_status == ESP_BLUEDROID_STATUS_INITIALIZED) {
    esp_err_t err = esp_bluedroid_deinit();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Bluedroid deinit failed: %s", esp_err_to_name(err));
    }
  } else if (bt_stack_status == ESP_BLUEDROID_STATUS_ENABLED) {
    ESP_LOGW(TAG, "Bluedroid still enabled, skipping deinit");
  }

  esp_bt_controller_status_t ctrl_status = esp_bt_controller_get_status();
  if (ctrl_status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    esp_err_t err = esp_bt_controller_disable();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "BT controller disable failed: %s", esp_err_to_name(err));
    }
    ctrl_status = esp_bt_controller_get_status();
  }
  if (ctrl_status == ESP_BT_CONTROLLER_STATUS_INITED) {
    esp_err_t err = esp_bt_controller_deinit();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "BT controller deinit failed: %s", esp_err_to_name(err));
    }
  } else if (ctrl_status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    ESP_LOGW(TAG, "BT controller still enabled, skipping deinit");
  }

  if (s_a2dp_deinit_sem) {
    vSemaphoreDelete(s_a2dp_deinit_sem);
    s_a2dp_deinit_sem = NULL;
  }
  if (s_bt_task_queue) {
    bt_app_msg_t stop_msg = {.sig = BT_APP_SIG_STOP};
    xQueueSend(s_bt_task_queue, &stop_msg, 0);
    for (int i = 0; s_bt_task_handle != NULL && i < 40; i++) {
      vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (s_bt_task_handle == NULL) {
      vQueueDelete(s_bt_task_queue);
      s_bt_task_queue = NULL;
    } else {
      ESP_LOGW(TAG, "BT app task did not stop in time");
    }
  }

  s_connected = false;
  s_audio_started = false;
  s_avrc_playing = false;
  s_vol_ntf_pending = false;
  s_sample_rate = 44100;
  s_bt_running = false;
  audio_output_set_sample_rate(44100);
  ESP_LOGI(TAG, "Bluetooth stopped");
  return ESP_OK;
}

bool bt_a2dp_sink_is_connected(void) {
  return s_connected;
}

bool bt_a2dp_sink_is_running(void) {
  return s_bt_running;
}

void bt_a2dp_sink_set_discoverable(bool discoverable) {
  s_bt_discoverable = discoverable;
  if (!s_bt_running ||
      esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
    return;
  }
  if (s_connected) {
    return;
  }
  if (discoverable) {
    ESP_LOGI(TAG, "BT discoverable enabled");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
  } else {
    ESP_LOGI(TAG, "BT discoverable disabled");
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  }
}

// ============================================================================
// AVRCP passthrough commands for hardware button control
// ============================================================================

static void send_passthrough(uint8_t key_code) {
  if (!s_connected) {
    return;
  }
  esp_avrc_ct_send_passthrough_cmd(0, key_code, ESP_AVRC_PT_CMD_STATE_PRESSED);
  vTaskDelay(pdMS_TO_TICKS(50));
  esp_avrc_ct_send_passthrough_cmd(0, key_code, ESP_AVRC_PT_CMD_STATE_RELEASED);
}

void bt_a2dp_send_playpause(void) {
  if (s_avrc_playing) {
    send_passthrough(ESP_AVRC_PT_CMD_PAUSE);
    ESP_LOGI(TAG, "AVRCP: pause");
  } else {
    send_passthrough(ESP_AVRC_PT_CMD_PLAY);
    ESP_LOGI(TAG, "AVRCP: play");
  }
}

void bt_a2dp_send_next(void) {
  send_passthrough(ESP_AVRC_PT_CMD_FORWARD);
  ESP_LOGI(TAG, "AVRCP: next");
}

void bt_a2dp_send_prev(void) {
  send_passthrough(ESP_AVRC_PT_CMD_BACKWARD);
  ESP_LOGI(TAG, "AVRCP: prev");
}

/**
 * Notify the source (phone) that local volume changed via AVRCP TG.
 * Only sends if the phone previously registered for the notification.
 */
static void notify_volume_changed(void) {
  if (s_vol_ntf_pending) {
    esp_avrc_rn_param_t rn_param;
    rn_param.volume = s_avrc_volume;
    esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED,
                            &rn_param);
    s_vol_ntf_pending = false;
  }
}

void bt_a2dp_send_volume_up(void) {
  // Adjust local AVRCP volume by ~10 units (≈3 dB equivalent)
  uint8_t new_vol = s_avrc_volume;
  if (new_vol <= 117) {
    new_vol += 10;
  } else {
    new_vol = 127;
  }
  s_avrc_volume = new_vol;
  float volume_db = ((float)new_vol / 127.0f) * 30.0f - 30.0f;
  dac_set_volume(volume_db);
  notify_volume_changed();
  ESP_LOGI(TAG, "AVRCP: volume up -> %d/127", new_vol);
}

void bt_a2dp_send_volume_down(void) {
  uint8_t new_vol = s_avrc_volume;
  if (new_vol >= 10) {
    new_vol -= 10;
  } else {
    new_vol = 0;
  }
  s_avrc_volume = new_vol;
  float volume_db = ((float)new_vol / 127.0f) * 30.0f - 30.0f;
  dac_set_volume(volume_db);
  notify_volume_changed();
  ESP_LOGI(TAG, "AVRCP: volume down -> %d/127", new_vol);
}
