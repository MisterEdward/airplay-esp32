#include "usb_audio_source.h"

#include "audio_output.h"
#include "usb_descriptors.h"
#include "usb_device_uac.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "tusb.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

static const char *TAG = "usb_audio";

#define OUTPUT_RATE     CONFIG_OUTPUT_SAMPLE_RATE_HZ
#define BYTES_PER_FRAME 4 // stereo int16

// Ring: 24 KB = 6144 frames = 128 ms at 48 kHz.  Internal RAM so the 1 ms
// output_cb never touches the PSRAM cache.
#define RING_BYTES (24 * 1024)
// Target depth the clock adapter keeps.  10 ms absorbs host burstiness
// (Windows delivers in 1-10 ms chunks) and adds little latency.
#define RING_TARGET_FRAMES (OUTPUT_RATE / 100)
// Adapter engages when the smoothed depth leaves target ± ENGAGE, releases
// inside target ± RELEASE.  Sizes in frames.
#define ADAPTER_ENGAGE_FRAMES  (OUTPUT_RATE / 250) // 4 ms
#define ADAPTER_RELEASE_FRAMES (OUTPUT_RATE / 500) // 2 ms
// Depth filter: EMA with 1/32 weight per pull (~0.25 s at 7 ms blocks).
#define DEPTH_FILTER_SHIFT   5
#define STREAMING_TIMEOUT_US 500000

static RingbufHandle_t s_ring;
static volatile bool s_active;
static volatile usb_host_state_t s_host_state;
static volatile bool s_remote_wakeup_armed;
static volatile int64_t s_last_data_us;
static volatile int32_t s_volume_q15 = 32768;
static volatile bool s_muted;
static usb_audio_stats_t s_stats;
static int32_t s_depth_filtered; // frames, Q0
static int s_adapter_dir;        // 0 idle, +1 duplicate (ring low), -1 drop

/* ------------------------------------------------------------------ */
/*  TinyUSB device callbacks (ours, because CONFIG_USB_DEVICE_UAC_AS_PART) */
/* ------------------------------------------------------------------ */

void tud_mount_cb(void) {
  s_host_state = USB_HOST_ACTIVE;
  s_stats.mounts++;
  ESP_LOGI(TAG, "Host: mounted (PC on)");
}

void tud_umount_cb(void) {
  s_host_state = USB_HOST_DISCONNECTED;
  s_remote_wakeup_armed = false;
  ESP_LOGI(TAG, "Host: unmounted (PC off / cable out)");
}

void tud_suspend_cb(bool remote_wakeup_en) {
  s_host_state = USB_HOST_SUSPENDED;
  s_remote_wakeup_armed = remote_wakeup_en;
  s_stats.suspends++;
  ESP_LOGI(TAG, "Host: suspended (PC asleep), remote_wakeup=%s",
           remote_wakeup_en ? "allowed" : "not allowed");
}

void tud_resume_cb(void) {
  s_host_state = tud_mounted() ? USB_HOST_ACTIVE : USB_HOST_DISCONNECTED;
  ESP_LOGI(TAG, "Host: resumed (%s)",
           s_host_state == USB_HOST_ACTIVE ? "mounted" : "not mounted");
}

/* ------------------------------------------------------------------ */
/*  UAC callbacks                                                      */
/* ------------------------------------------------------------------ */

// Runs in the UAC component's speaker task right after its ISR filled a
// single shared buffer; the next isochronous packet overwrites that buffer
// ~1 ms later.  Copy and return — nothing else.
static esp_err_t uac_output_cb(uint8_t *buf, size_t len, void *ctx) {
  (void)ctx;
  s_last_data_us = esp_timer_get_time();
  s_stats.packets++;
  if (!s_active) {
    s_stats.discarded_bytes += (uint32_t)len;
    return ESP_OK;
  }
  RingbufHandle_t rb = s_ring;
  if (!rb) {
    return ESP_OK;
  }
  if (xRingbufferSend(rb, buf, len, 0) != pdTRUE) {
    // Full: the render task is behind (or not pulling).  Drop the oldest
    // block's worth to make room so the ring stays near real time rather
    // than accumulating latency.
    s_stats.overruns++;
    size_t got = 0;
    void *old = xRingbufferReceiveUpTo(rb, &got, 0, len);
    if (old) {
      vRingbufferReturnItem(rb, old);
    }
    xRingbufferSend(rb, buf, len, 0);
  }
  return ESP_OK;
}

static void uac_set_mute_cb(uint32_t mute, void *ctx) {
  (void)ctx;
  s_muted = mute != 0;
  audio_output_set_source_volume(AUDIO_SOURCE_EXTERNAL,
                                 s_muted ? 0 : s_volume_q15, false);
  ESP_LOGI(TAG, "Host mute: %s", s_muted ? "on" : "off");
}

// The component hands us 0..100 derived from the host's -50..0 dB request.
static void uac_set_volume_cb(uint32_t volume, void *ctx) {
  (void)ctx;
  if (volume > 100) {
    volume = 100;
  }
  float db = ((float)volume - 100.0f) / 2.0f; // back to -50..0 dB
  int32_t q15 = volume == 0 ? 0 : (int32_t)(powf(10.0f, db / 20.0f) * 32768.0f);
  s_volume_q15 = q15;
  s_stats.volume_q15 = q15;
  audio_output_set_source_volume(AUDIO_SOURCE_EXTERNAL, s_muted ? 0 : q15,
                                 false);
  ESP_LOGI(TAG, "Host volume: %lu%% (%.1f dB, q15=%ld)", (unsigned long)volume,
           db, (long)q15);
}

/* ------------------------------------------------------------------ */
/*  Pull side (render task)                                            */
/* ------------------------------------------------------------------ */

static size_t quietest_frame(const int16_t *pcm, size_t frames) {
  size_t best = frames - 1;
  int32_t best_mag = INT32_MAX;
  for (size_t i = 0; i < frames; i++) {
    int32_t mag = abs(pcm[2 * i]) + abs(pcm[2 * i + 1]);
    if (mag < best_mag) {
      best_mag = mag;
      best = i;
    }
  }
  return best;
}

static size_t usb_pull(int16_t *pcm, size_t max_frames, void *ctx) {
  (void)ctx;
  RingbufHandle_t rb = s_ring;
  if (!rb || !s_active) {
    return 0;
  }
  int64_t now = esp_timer_get_time();
  bool streaming = (now - s_last_data_us) < STREAMING_TIMEOUT_US;
  s_stats.streaming = streaming;

  UBaseType_t waiting = 0;
  vRingbufferGetInfo(rb, NULL, NULL, NULL, NULL, &waiting);
  size_t depth = waiting / BYTES_PER_FRAME;
  s_stats.ring_frames = (uint32_t)depth;

  // Prefill: after silence, wait until the ring holds the target depth so
  // playback starts with the intended cushion instead of at the edge.
  if (s_depth_filtered == 0 && depth < RING_TARGET_FRAMES) {
    return 0;
  }
  if (depth == 0) {
    if (streaming) {
      s_stats.underruns++;
    }
    s_depth_filtered = 0; // re-prefill on the next data
    s_adapter_dir = 0;
    return 0;
  }

  // Smooth the depth and decide on a one-sample correction for this block.
  s_depth_filtered += ((int32_t)depth - s_depth_filtered) >> DEPTH_FILTER_SHIFT;
  if (s_depth_filtered <= 0) {
    s_depth_filtered = 1;
  }
  int32_t err = s_depth_filtered - (int32_t)RING_TARGET_FRAMES;
  if (s_adapter_dir == 0) {
    if (err > ADAPTER_ENGAGE_FRAMES) {
      s_adapter_dir = -1; // too deep: consume one extra sample per block
    } else if (err < -ADAPTER_ENGAGE_FRAMES) {
      s_adapter_dir = +1; // too shallow: stretch by one sample per block
    }
  } else if (abs(err) < ADAPTER_RELEASE_FRAMES) {
    s_adapter_dir = 0;
  }

  // Read whole frames.  Take one more than we output when dropping, one
  // fewer when duplicating, so the output block length stays what the
  // render task asked for.
  size_t want_out = max_frames;
  if (depth < want_out) {
    want_out = depth;
  }
  size_t want_in = want_out;
  if (s_adapter_dir < 0 && depth > want_out) {
    want_in = want_out + 1;
  } else if (s_adapter_dir > 0 && want_out > 1) {
    want_in = want_out - 1;
  }

  // The ring may hand the request back in two pieces (wrap-around).  Static
  // scratch: 4 KB would not fit on the render task's stack, and only the
  // render task ever calls this.
  static int16_t tmp[2 * 1026];
  if (want_in > 1025) {
    want_in = 1025;
    want_out = s_adapter_dir < 0 ? 1024 : (s_adapter_dir > 0 ? 1026 : 1025);
    if (want_out > max_frames) {
      want_out = max_frames;
    }
  }
  size_t got_bytes = 0;
  size_t need = want_in * BYTES_PER_FRAME;
  while (got_bytes < need) {
    size_t n = 0;
    void *p = xRingbufferReceiveUpTo(rb, &n, 0, need - got_bytes);
    if (!p) {
      break;
    }
    memcpy((uint8_t *)tmp + got_bytes, p, n);
    vRingbufferReturnItem(rb, p);
    got_bytes += n;
  }
  size_t got = got_bytes / BYTES_PER_FRAME;
  if (got == 0) {
    return 0;
  }
  if (got != want_in) {
    // Short read (should not happen): no correction this block.
    memcpy(pcm, tmp, got * BYTES_PER_FRAME);
    return got;
  }

  if (s_adapter_dir < 0) {
    // drop the quietest frame
    size_t m = quietest_frame(tmp, got);
    size_t o = 0;
    for (size_t i = 0; i < got; i++) {
      if (i == m) {
        continue;
      }
      pcm[2 * o] = tmp[2 * i];
      pcm[2 * o + 1] = tmp[2 * i + 1];
      o++;
    }
    s_stats.trims++;
    return o;
  }
  if (s_adapter_dir > 0) {
    // duplicate the quietest frame
    size_t m = quietest_frame(tmp, got);
    size_t o = 0;
    for (size_t i = 0; i < got; i++) {
      pcm[2 * o] = tmp[2 * i];
      pcm[2 * o + 1] = tmp[2 * i + 1];
      o++;
      if (i == m) {
        pcm[2 * o] = tmp[2 * i];
        pcm[2 * o + 1] = tmp[2 * i + 1];
        o++;
      }
    }
    s_stats.trims++;
    return o;
  }
  memcpy(pcm, tmp, got * BYTES_PER_FRAME);
  return got;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

// Log every change of the bus-level state.  tud_connected() flips as soon as
// the host sends its first SETUP packet, long before tud_mount_cb(): if it
// never flips, the host is not talking to this port at all (cable, wrong
// port, PHY); if it flips but mount never follows, the descriptors are being
// rejected.
static esp_timer_handle_t s_state_timer;
static void usb_state_poll_cb(void *arg) {
  (void)arg;
  static int last = -1;
  bool streaming =
      (esp_timer_get_time() - s_last_data_us) < STREAMING_TIMEOUT_US;
  int now = (tud_connected() ? 1 : 0) | (tud_mounted() ? 2 : 0) |
            (tud_suspended() ? 4 : 0) | (streaming ? 8 : 0);
  if (now != last) {
    ESP_LOGI(TAG,
             "Bus: connected=%d mounted=%d suspended=%d streaming=%d "
             "speed=%s packets=%" PRIu32 " ring=%" PRIu32 " under=%" PRIu32
             " over=%" PRIu32,
             !!(now & 1), !!(now & 2), !!(now & 4), !!(now & 8),
             tud_speed_get() == TUSB_SPEED_HIGH ? "high"
             : tud_speed_get() == TUSB_SPEED_FULL ? "full"
                                                  : "none",
             s_stats.packets, s_stats.ring_frames, s_stats.underruns,
             s_stats.overruns);
    last = now;
  }
}

esp_err_t usb_audio_source_init(void) {
  if (s_ring) {
    return ESP_OK;
  }
  s_ring = xRingbufferCreate(RING_BYTES, RINGBUF_TYPE_BYTEBUF);
  if (!s_ring) {
    return ESP_ERR_NO_MEM;
  }
  s_stats.ring_target = RING_TARGET_FRAMES;
  s_stats.volume_q15 = s_volume_q15;

  uac_device_config_t config = {
      .skip_tinyusb_init = false, // the component brings up the PHY + stack
      .output_cb = uac_output_cb,
      .input_cb = NULL,
      .set_mute_cb = uac_set_mute_cb,
      .set_volume_cb = uac_set_volume_cb,
      .cb_ctx = NULL,
      .spk_itf_num = USB_ITF_AUDIO_STREAMING_SPK,
      .mic_itf_num = -1,
  };
  esp_err_t err = uac_device_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uac_device_init failed: %s", esp_err_to_name(err));
    return err;
  }
  audio_output_register_external_source(usb_pull, NULL);
  const esp_timer_create_args_t poll = {.callback = usb_state_poll_cb,
                                        .name = "usb_state"};
  if (esp_timer_create(&poll, &s_state_timer) == ESP_OK) {
    esp_timer_start_periodic(s_state_timer, 500000);
  }
  ESP_LOGI(TAG,
           "USB speaker ready: UAC2 %d Hz stereo 16-bit + HID wake, ring=%d ms "
           "target=%d ms",
           OUTPUT_RATE, RING_BYTES / BYTES_PER_FRAME * 1000 / OUTPUT_RATE,
           RING_TARGET_FRAMES * 1000 / OUTPUT_RATE);
  return ESP_OK;
}

void usb_audio_source_set_active(bool active) {
  if (active == s_active) {
    return;
  }
  s_active = active;
  s_depth_filtered = 0;
  s_adapter_dir = 0;
  if (s_ring) {
    // Start from an empty ring so the prefill sets the depth, not stale
    // data from before AirPlay took over.
    size_t n = 0;
    void *p;
    while ((p = xRingbufferReceiveUpTo(s_ring, &n, 0, RING_BYTES)) != NULL) {
      vRingbufferReturnItem(s_ring, p);
    }
  }
  ESP_LOGI(TAG, "USB source %s", active ? "active" : "inactive (discarding)");
}

usb_host_state_t usb_audio_source_host_state(void) {
  return s_host_state;
}

void usb_audio_source_get_stats(usb_audio_stats_t *out) {
  if (!out) {
    return;
  }
  *out = s_stats;
  out->host_state = s_host_state;
  out->bus_connected = tud_connected();
  out->mounted = tud_mounted();
  out->remote_wakeup_armed = s_remote_wakeup_armed;
  out->muted = s_muted;
  out->streaming =
      (esp_timer_get_time() - s_last_data_us) < STREAMING_TIMEOUT_US;
}

esp_err_t usb_audio_source_wake_host(void) {
  if (!tud_suspended()) {
    ESP_LOGI(TAG, "Wake: host not suspended (state=%d)", s_host_state);
    return ESP_ERR_INVALID_STATE;
  }
  if (!s_remote_wakeup_armed) {
    ESP_LOGW(TAG, "Wake: host did not allow remote wakeup");
    return ESP_ERR_NOT_ALLOWED;
  }
  ESP_LOGI(TAG, "Wake: sending USB remote-wakeup resume signal");
  if (!tud_remote_wakeup()) {
    return ESP_FAIL;
  }
  // The bus needs a few ms to resume; then nudge the host with a keypress
  // some BIOS/OS wake policies want to see.  Left Ctrl alone is inert in
  // every application.
  for (int i = 0; i < 40 && tud_suspended(); i++) {
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  if (tud_hid_ready()) {
    tud_hid_keyboard_report(0, KEYBOARD_MODIFIER_LEFTCTRL, NULL);
    vTaskDelay(pdMS_TO_TICKS(30));
    tud_hid_keyboard_report(0, 0, NULL);
    ESP_LOGI(TAG, "Wake: HID nudge sent");
  } else {
    ESP_LOGI(TAG, "Wake: bus resumed=%d, HID not ready for nudge",
             !tud_suspended());
  }
  return ESP_OK;
}
