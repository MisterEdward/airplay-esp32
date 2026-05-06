#include "audio_alert.h"

#include "dac.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "rtsp_events.h"
#include "sdkconfig.h"
#include "settings.h"
#include <strings.h>

#define TAG "audio_alert"

#define ALERT_DEFAULT_VOLUME 10
#define ALERT_MAX_REPEAT     20
#define ALERT_IDLE_DAC_DB    (-12.0f)
#define CHIME_SAMPLE_RATE    44100

typedef struct {
  bool active;
  bool airplay_playing;
  bool owns_dac;
  audio_alert_id_t id;
  uint32_t sample_rate;
  uint32_t total_frames;
  uint32_t frames_left;
  uint32_t position;
  int32_t volume_q15;
  uint32_t generation;
} audio_alert_state_t;

extern const uint8_t chime_pcm[];
extern const size_t chime_pcm_len;

static portMUX_TYPE s_alert_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_alert_state_t s_alert = {0};

static void restore_saved_volume(void) {
  float volume_db = -15.0f;
  if (settings_get_volume(&volume_db) != ESP_OK) {
    volume_db = -15.0f;
  }
  dac_set_volume(volume_db);
}

static int16_t clamp16(int32_t sample) {
  if (sample > INT16_MAX) {
    return INT16_MAX;
  }
  if (sample < INT16_MIN) {
    return INT16_MIN;
  }
  return (int16_t)sample;
}

static size_t chime_sample_count(void) {
  return chime_pcm_len / sizeof(int16_t);
}

static int16_t chime_sample_at(size_t index) {
  const uint8_t *sample = chime_pcm + (index * sizeof(int16_t));
  return (int16_t)((uint16_t)sample[0] | ((uint16_t)sample[1] << 8));
}

static uint32_t chime_output_frames(uint32_t sample_rate) {
  size_t samples = chime_sample_count();
  if (samples == 0 || sample_rate == 0) {
    return 0;
  }
  return (uint32_t)(((uint64_t)samples * sample_rate) / CHIME_SAMPLE_RATE);
}

static int32_t sample_chime(audio_alert_state_t *st) {
  uint32_t sr = st->sample_rate ? st->sample_rate : CHIME_SAMPLE_RATE;
  uint32_t frames = chime_output_frames(sr);
  size_t samples = chime_sample_count();
  if (frames == 0 || samples == 0) {
    return 0;
  }
  uint32_t pos = st->position % frames;
  size_t index = (size_t)(((uint64_t)pos * CHIME_SAMPLE_RATE) / sr);
  if (index >= samples) {
    index = samples - 1;
  }
  int32_t sample = chime_sample_at(index);
  sample = (sample * st->volume_q15) >> 15;
  return sample;
}

static void alert_event_cb(rtsp_event_t event, const rtsp_event_data_t *data,
                           void *user_data) {
  (void)data;
  (void)user_data;

  portENTER_CRITICAL(&s_alert_lock);
  switch (event) {
  case RTSP_EVENT_PLAYING:
    s_alert.airplay_playing = true;
    s_alert.owns_dac = false;
    break;
  case RTSP_EVENT_CLIENT_CONNECTED:
  case RTSP_EVENT_PAUSED:
  case RTSP_EVENT_DISCONNECTED:
    s_alert.airplay_playing = false;
    break;
  case RTSP_EVENT_METADATA:
    break;
  }
  portEXIT_CRITICAL(&s_alert_lock);
}

esp_err_t audio_alert_init(void) {
  static bool initialized = false;
  if (initialized) {
    return ESP_OK;
  }
  if (rtsp_events_register(alert_event_cb, NULL) != 0) {
    return ESP_FAIL;
  }
  initialized = true;
  ESP_LOGI(TAG, "Loaded chime sample: %u frames", (unsigned)chime_sample_count());
  return ESP_OK;
}

const char *audio_alert_name(audio_alert_id_t id) {
  (void)id;
  return "chime";
}

const char *audio_alert_names_json(void) {
  return "[\"chime\"]";
}

static bool parse_name(const char *name, audio_alert_id_t *id) {
  if (!name || !id) {
    return false;
  }
  if (strcasecmp(name, "chime") == 0) {
    *id = AUDIO_ALERT_CHIME;
    return true;
  }
  return false;
}

esp_err_t audio_alert_play_name(const char *name, uint8_t volume_percent,
                                uint8_t repeat) {
  audio_alert_id_t id;
  if (!parse_name(name, &id)) {
    return ESP_ERR_NOT_FOUND;
  }
  audio_alert_config_t cfg = {
      .id = id,
      .volume_percent = volume_percent,
      .repeat = repeat,
  };
  return audio_alert_play(&cfg);
}

esp_err_t audio_alert_play(const audio_alert_config_t *config) {
  if (!config) {
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t volume = config->volume_percent;
  uint8_t repeat = config->repeat;
  if (volume == 0) {
    volume = ALERT_DEFAULT_VOLUME;
  }
  if (volume > 100) {
    volume = 100;
  }
  if (repeat == 0) {
    repeat = 1;
  }
  if (repeat > ALERT_MAX_REPEAT) {
    repeat = ALERT_MAX_REPEAT;
  }

  uint32_t sample_rate = CONFIG_OUTPUT_SAMPLE_RATE_HZ;
  bool should_power_dac = false;
  portENTER_CRITICAL(&s_alert_lock);
  should_power_dac = !s_alert.airplay_playing;
  uint32_t frames = chime_output_frames(sample_rate);
  frames *= repeat;
  s_alert.generation++;
  s_alert.active = true;
  s_alert.owns_dac = should_power_dac;
  s_alert.id = config->id;
  s_alert.sample_rate = sample_rate;
  s_alert.total_frames = frames;
  s_alert.frames_left = frames;
  s_alert.position = 0;
  s_alert.volume_q15 = ((int32_t)volume * 32768) / 100;
  portEXIT_CRITICAL(&s_alert_lock);

  if (should_power_dac) {
    dac_set_volume(ALERT_IDLE_DAC_DB);
    dac_set_power_mode(DAC_POWER_ON);
  }

  ESP_LOGI(TAG, "Playing alert '%s' volume=%u repeat=%u",
           audio_alert_name(config->id), volume, repeat);
  return ESP_OK;
}

void audio_alert_stop(void) {
  bool should_power_off = false;
  portENTER_CRITICAL(&s_alert_lock);
  should_power_off = s_alert.active && s_alert.owns_dac;
  s_alert.generation++;
  s_alert.active = false;
  s_alert.frames_left = 0;
  s_alert.owns_dac = false;
  portEXIT_CRITICAL(&s_alert_lock);

  if (should_power_off) {
    dac_set_power_mode(DAC_POWER_OFF);
    restore_saved_volume();
  }
}

bool audio_alert_is_active(void) {
  bool active;
  portENTER_CRITICAL(&s_alert_lock);
  active = s_alert.active;
  portEXIT_CRITICAL(&s_alert_lock);
  return active;
}

void audio_alert_mix(int16_t *pcm, size_t stereo_frames, uint32_t sample_rate) {
  if (!pcm || stereo_frames == 0) {
    return;
  }

  audio_alert_state_t local;
  bool finished = false;
  bool should_power_off = false;

  portENTER_CRITICAL(&s_alert_lock);
  if (!s_alert.active || s_alert.frames_left == 0) {
    portEXIT_CRITICAL(&s_alert_lock);
    return;
  }
  if (s_alert.sample_rate != sample_rate) {
    s_alert.sample_rate = sample_rate;
  }
  local = s_alert;
  portEXIT_CRITICAL(&s_alert_lock);

  for (size_t i = 0; i < stereo_frames && local.frames_left > 0; i++) {
    int32_t alert = sample_chime(&local);
    pcm[i * 2] = clamp16((int32_t)pcm[i * 2] + alert);
    pcm[i * 2 + 1] = clamp16((int32_t)pcm[i * 2 + 1] + alert);
    local.position++;
    local.frames_left--;
  }

  portENTER_CRITICAL(&s_alert_lock);
  if (s_alert.generation == local.generation && s_alert.active) {
    s_alert.sample_rate = local.sample_rate;
    s_alert.total_frames = local.total_frames;
    s_alert.frames_left = local.frames_left;
    s_alert.position = local.position;
    if (s_alert.frames_left == 0) {
      finished = true;
      should_power_off = s_alert.owns_dac && !s_alert.airplay_playing;
      s_alert.generation++;
      s_alert.active = false;
      s_alert.owns_dac = false;
    }
  }
  portEXIT_CRITICAL(&s_alert_lock);

  if (finished) {
    ESP_LOGI(TAG, "Alert finished");
    if (should_power_off) {
      dac_set_power_mode(DAC_POWER_OFF);
      restore_saved_volume();
    }
  }
}
