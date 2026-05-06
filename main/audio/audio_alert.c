#include "audio_alert.h"

#include "dac.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "rtsp_events.h"
#include "sdkconfig.h"
#include <strings.h>

#define TAG "audio_alert"

#define ALERT_DEFAULT_VOLUME 85
#define ALERT_MAX_REPEAT     20
#define PHASE_BITS           32
#define TRIANGLE_PEAK        32767

typedef struct {
  bool active;
  bool airplay_playing;
  bool owns_dac;
  audio_alert_id_t id;
  uint32_t sample_rate;
  uint32_t total_frames;
  uint32_t frames_left;
  uint32_t position;
  uint32_t phase1;
  uint32_t phase2;
  uint32_t phase_inc1;
  uint32_t phase_inc2;
  uint32_t freq1;
  uint32_t freq2;
  int32_t volume_q15;
  uint32_t generation;
} audio_alert_state_t;

static portMUX_TYPE s_alert_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_alert_state_t s_alert = {0};

static uint32_t ms_to_frames(uint32_t sample_rate, uint32_t ms) {
  return (sample_rate * ms) / 1000;
}

static uint32_t phase_inc(uint32_t hz, uint32_t sample_rate) {
  return (uint32_t)(((uint64_t)hz << PHASE_BITS) / sample_rate);
}

static void set_freq1(audio_alert_state_t *st, uint32_t hz) {
  if (st->freq1 != hz) {
    st->freq1 = hz;
    st->phase_inc1 = phase_inc(hz, st->sample_rate);
  }
}

static void set_freq2(audio_alert_state_t *st, uint32_t hz) {
  if (st->freq2 != hz) {
    st->freq2 = hz;
    st->phase_inc2 = phase_inc(hz, st->sample_rate);
  }
}

static int32_t triangle_sample(uint32_t phase) {
  uint32_t p = phase >> 16;
  int32_t v = (p < 32768) ? (int32_t)p : (int32_t)(65535 - p);
  return (v * 2) - TRIANGLE_PEAK;
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

static uint32_t base_duration_ms(audio_alert_id_t id) {
  switch (id) {
  case AUDIO_ALERT_BEEP:
    return 700;
  case AUDIO_ALERT_CHIME:
    return 1300;
  case AUDIO_ALERT_BELL:
    return 1800;
  case AUDIO_ALERT_ALARM:
  default:
    return 2400;
  }
}

static int32_t envelope_q15(uint32_t pos, uint32_t total) {
  if (total == 0) {
    return 0;
  }
  uint32_t attack = total / 80;
  uint32_t release = total / 16;
  if (attack < 64) {
    attack = 64;
  }
  if (release < 256) {
    release = 256;
  }
  if (pos < attack) {
    return (int32_t)((pos * 32768U) / attack);
  }
  if (pos + release >= total) {
    uint32_t left = total > pos ? total - pos : 0;
    return (int32_t)((left * 32768U) / release);
  }
  return 32768;
}

static bool pulse_on(uint32_t pos, uint32_t sample_rate, uint32_t on_ms,
                     uint32_t off_ms) {
  uint32_t period = ms_to_frames(sample_rate, on_ms + off_ms);
  uint32_t on = ms_to_frames(sample_rate, on_ms);
  if (period == 0) {
    return true;
  }
  return (pos % period) < on;
}

static int32_t synth_sample(audio_alert_state_t *st) {
  uint32_t pos = st->position;
  uint32_t sr = st->sample_rate ? st->sample_rate : 44100;
  int32_t sample = 0;

  switch (st->id) {
  case AUDIO_ALERT_BEEP:
    if (pulse_on(pos, sr, 120, 80)) {
      set_freq1(st, 1040);
      st->phase1 += st->phase_inc1;
      sample = triangle_sample(st->phase1);
    }
    break;
  case AUDIO_ALERT_CHIME: {
    uint32_t split = ms_to_frames(sr, 420);
    uint32_t freq = pos < split ? 880 : 1320;
    set_freq1(st, freq);
    st->phase1 += st->phase_inc1;
    sample = triangle_sample(st->phase1);
    break;
  }
  case AUDIO_ALERT_BELL:
    set_freq1(st, 740);
    set_freq2(st, 1110);
    st->phase1 += st->phase_inc1;
    st->phase2 += st->phase_inc2;
    sample = (triangle_sample(st->phase1) + triangle_sample(st->phase2)) / 2;
    break;
  case AUDIO_ALERT_ALARM:
  default: {
    uint32_t segment = ms_to_frames(sr, 240);
    uint32_t freq = ((segment > 0) && ((pos / segment) & 1)) ? 880 : 660;
    if (pulse_on(pos, sr, 300, 80)) {
      set_freq1(st, freq);
      st->phase1 += st->phase_inc1;
      sample = triangle_sample(st->phase1);
    }
    break;
  }
  }

  int32_t env = envelope_q15(pos, st->total_frames);
  sample = (sample * env) >> 15;
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
  return ESP_OK;
}

const char *audio_alert_name(audio_alert_id_t id) {
  switch (id) {
  case AUDIO_ALERT_BEEP:
    return "beep";
  case AUDIO_ALERT_CHIME:
    return "chime";
  case AUDIO_ALERT_BELL:
    return "bell";
  case AUDIO_ALERT_ALARM:
  default:
    return "alarm";
  }
}

const char *audio_alert_names_json(void) {
  return "[\"alarm\",\"beep\",\"chime\",\"bell\"]";
}

static bool parse_name(const char *name, audio_alert_id_t *id) {
  if (!name || !id) {
    return false;
  }
  if (strcasecmp(name, "alarm") == 0 || strcasecmp(name, "siren") == 0) {
    *id = AUDIO_ALERT_ALARM;
    return true;
  }
  if (strcasecmp(name, "beep") == 0 || strcasecmp(name, "ping") == 0) {
    *id = AUDIO_ALERT_BEEP;
    return true;
  }
  if (strcasecmp(name, "chime") == 0) {
    *id = AUDIO_ALERT_CHIME;
    return true;
  }
  if (strcasecmp(name, "bell") == 0) {
    *id = AUDIO_ALERT_BELL;
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
  uint32_t frames = ms_to_frames(sample_rate, base_duration_ms(config->id));
  frames *= repeat;

  bool should_power_dac = false;
  portENTER_CRITICAL(&s_alert_lock);
  should_power_dac = !s_alert.airplay_playing;
  s_alert.generation++;
  s_alert.active = true;
  s_alert.owns_dac = should_power_dac;
  s_alert.id = config->id;
  s_alert.sample_rate = sample_rate;
  s_alert.total_frames = frames;
  s_alert.frames_left = frames;
  s_alert.position = 0;
  s_alert.phase1 = 0;
  s_alert.phase2 = 0;
  s_alert.phase_inc1 = 0;
  s_alert.phase_inc2 = 0;
  s_alert.freq1 = 0;
  s_alert.freq2 = 0;
  s_alert.volume_q15 = ((int32_t)volume * 32768) / 100;
  portEXIT_CRITICAL(&s_alert_lock);

  if (should_power_dac) {
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
    int32_t alert = synth_sample(&local);
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
    s_alert.phase1 = local.phase1;
    s_alert.phase2 = local.phase2;
    s_alert.phase_inc1 = local.phase_inc1;
    s_alert.phase_inc2 = local.phase_inc2;
    s_alert.freq1 = local.freq1;
    s_alert.freq2 = local.freq2;
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
    }
  }
}
