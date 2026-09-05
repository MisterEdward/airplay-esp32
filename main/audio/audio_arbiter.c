#include "audio_arbiter.h"

#include "audio_output.h"
#include "rtsp_events.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "led.h"

#ifdef CONFIG_USB_AUDIO_SOURCE
#include "usb_audio_source.h"
#endif

static const char *TAG = "arbiter";

static bool s_usb_available;
static audio_arbiter_state_t s_state;
static esp_timer_handle_t s_release_timer;

static void give_to(audio_source_t src, const char *why) {
  audio_output_select_source(src);
#ifdef CONFIG_USB_AUDIO_SOURCE
  usb_audio_source_set_active(src == AUDIO_SOURCE_EXTERNAL);
#endif
  s_state.hand_overs++;
  s_state.last_change_us = esp_timer_get_time();
  ESP_LOGI(TAG, "Output -> %s (%s)",
           src == AUDIO_SOURCE_AIRPLAY    ? "AirPlay"
           : src == AUDIO_SOURCE_EXTERNAL ? "USB"
                                          : "none",
           why);
}

#ifdef CONFIG_USB_AUDIO_SOURCE
// The LED is event-driven for AirPlay; USB has no session events, so poll
// its streaming flag at a human rate.
static esp_timer_handle_t s_led_poll_timer;
static void led_poll_cb(void *arg) {
  (void)arg;
  usb_audio_stats_t st;
  usb_audio_source_get_stats(&st);
  led_set_external_playing(
      audio_output_active_source() == AUDIO_SOURCE_EXTERNAL && st.streaming);
}
#endif

static void release_timer_cb(void *arg) {
  (void)arg;
  s_state.release_pending = false;
  if (s_state.airplay_connected) {
    return; // a sender came back during the grace period
  }
  give_to(s_usb_available ? AUDIO_SOURCE_EXTERNAL : AUDIO_SOURCE_AIRPLAY,
          "AirPlay gone for 3 s");
}

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)data;
  (void)user_data;
  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
  case RTSP_EVENT_PLAYING:
  case RTSP_EVENT_PAUSED:
    s_state.airplay_connected = true;
    s_state.airplay_playing = event == RTSP_EVENT_PLAYING;
    if (s_state.release_pending) {
      esp_timer_stop(s_release_timer);
      s_state.release_pending = false;
      ESP_LOGI(TAG, "AirPlay back within grace period; USB stays parked");
    }
    if (audio_output_active_source() != AUDIO_SOURCE_AIRPLAY) {
      give_to(AUDIO_SOURCE_AIRPLAY, event == RTSP_EVENT_CLIENT_CONNECTED
                                        ? "AirPlay client connected"
                                        : "AirPlay session active");
    }
    break;
  case RTSP_EVENT_DISCONNECTED:
    s_state.airplay_connected = false;
    s_state.airplay_playing = false;
    if (s_usb_available && !s_state.release_pending) {
      s_state.release_pending = true;
      esp_timer_start_once(s_release_timer,
                           (uint64_t)AIRPLAY_RELEASE_GRACE_MS * 1000ULL);
      ESP_LOGI(TAG, "AirPlay disconnected; releasing to USB in %d ms",
               AIRPLAY_RELEASE_GRACE_MS);
    }
    break;
  default:
    break;
  }
}

esp_err_t audio_arbiter_init(bool usb_available) {
  s_usb_available = usb_available;
  const esp_timer_create_args_t args = {
      .callback = release_timer_cb,
      .name = "arb_release",
  };
  esp_err_t err = esp_timer_create(&args, &s_release_timer);
  if (err != ESP_OK) {
    return err;
  }
  rtsp_events_register(on_rtsp_event, NULL);
#ifdef CONFIG_USB_AUDIO_SOURCE
  if (usb_available) {
    const esp_timer_create_args_t poll = {.callback = led_poll_cb,
                                          .name = "arb_led"};
    if (esp_timer_create(&poll, &s_led_poll_timer) == ESP_OK) {
      esp_timer_start_periodic(s_led_poll_timer, 500000);
    }
  }
#endif
  // Nobody is connected at boot: USB (if present) may have the speaker.
  give_to(usb_available ? AUDIO_SOURCE_EXTERNAL : AUDIO_SOURCE_AIRPLAY, "boot");
  return ESP_OK;
}

void audio_arbiter_get_state(audio_arbiter_state_t *out) {
  if (out) {
    *out = s_state;
  }
}
