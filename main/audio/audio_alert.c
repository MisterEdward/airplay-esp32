#include "audio_alert.h"

#include "dac.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "rtsp_events.h"
#include "sdkconfig.h"
#include "settings.h"
#include <math.h>
#include <strings.h>

#define TAG "audio_alert"

#define ALERT_DEFAULT_VOLUME 20
#define ALERT_MAX_REPEAT     20
#define PHASE_BITS           32
#define SINE_TABLE_SIZE      256
#define SINE_TABLE_SCALE     26000
#define ALERT_IDLE_DAC_DB    (-12.0f)
#define CHIME_DURATION_MS    920
#define ALERT_PI            3.14159265358979323846f

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
  uint32_t phase3;
  uint32_t phase_inc1;
  uint32_t phase_inc2;
  uint32_t phase_inc3;
  uint32_t freq1;
  uint32_t freq2;
  uint32_t freq3;
  int32_t volume_q15;
  uint32_t generation;
} audio_alert_state_t;

static portMUX_TYPE s_alert_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_alert_state_t s_alert = {0};
static int16_t s_sine_table[SINE_TABLE_SIZE];

static void init_sine_table(void) {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  for (int i = 0; i < SINE_TABLE_SIZE; i++) {
    float phase = (2.0f * ALERT_PI * (float)i) / (float)SINE_TABLE_SIZE;
    s_sine_table[i] = (int16_t)(sinf(phase) * SINE_TABLE_SCALE);
  }
  initialized = true;
}

static void restore_saved_volume(void) {
  float volume_db = -15.0f;
  if (settings_get_volume(&volume_db) != ESP_OK) {
    volume_db = -15.0f;
  }
  dac_set_volume(volume_db);
}

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

static void set_freq3(audio_alert_state_t *st, uint32_t hz) {
  if (st->freq3 != hz) {
    st->freq3 = hz;
    st->phase_inc3 = phase_inc(hz, st->sample_rate);
  }
}

static int32_t sine_sample(uint32_t phase) {
  return s_sine_table[phase >> 24];
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

static uint32_t base_duration_ms(void) {
  return CHIME_DURATION_MS;
}

static int32_t note_envelope_q15(uint32_t pos, uint32_t sample_rate,
                                 uint32_t start_ms, uint32_t end_ms,
                                 uint32_t attack_ms, uint32_t release_ms) {
  uint32_t start = ms_to_frames(sample_rate, start_ms);
  uint32_t end = ms_to_frames(sample_rate, end_ms);
  if (end <= start || pos < start || pos >= end) {
    return 0;
  }

  uint32_t note_pos = pos - start;
  uint32_t note_len = end - start;
  uint32_t attack = ms_to_frames(sample_rate, attack_ms);
  uint32_t release = ms_to_frames(sample_rate, release_ms);
  if (attack == 0) {
    attack = 1;
  }
  if (release == 0) {
    release = 1;
  }

  if (note_pos < attack) {
    return (int32_t)((note_pos * 32768U) / attack);
  }
  if (note_pos + release >= note_len) {
    uint32_t left = note_len > note_pos ? note_len - note_pos : 0;
    return (int32_t)((left * 32768U) / release);
  }
  return 32768;
}

static int32_t chime_voice(uint32_t *phase, uint32_t phase_inc, int32_t sample,
                           int32_t envelope, int32_t gain_q15) {
  if (envelope <= 0) {
    return 0;
  }

  *phase += phase_inc;
  int32_t voice = (sample * envelope) >> 15;
  return (voice * gain_q15) >> 15;
}

static int32_t synth_sample(audio_alert_state_t *st) {
  uint32_t sr = st->sample_rate ? st->sample_rate : 44100;
  uint32_t chime_frames = ms_to_frames(sr, CHIME_DURATION_MS);
  uint32_t pos = chime_frames > 0 ? st->position % chime_frames : st->position;
  int32_t sample = 0;

  set_freq1(st, 988);
  set_freq2(st, 1319);
  set_freq3(st, 1760);

  int32_t env1 = note_envelope_q15(pos, sr, 0, 260, 18, 120);
  sample += chime_voice(&st->phase1, st->phase_inc1, sine_sample(st->phase1),
                        env1, 19000);

  int32_t env2 = note_envelope_q15(pos, sr, 115, 520, 20, 210);
  sample += chime_voice(&st->phase2, st->phase_inc2, sine_sample(st->phase2),
                        env2, 16500);

  int32_t env3 = note_envelope_q15(pos, sr, 300, 900, 28, 360);
  sample += chime_voice(&st->phase3, st->phase_inc3, sine_sample(st->phase3),
                        env3, 10500);

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
  init_sine_table();
  if (rtsp_events_register(alert_event_cb, NULL) != 0) {
    return ESP_FAIL;
  }
  initialized = true;
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
  uint32_t frames = ms_to_frames(sample_rate, base_duration_ms());
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
  s_alert.phase3 = 0;
  s_alert.phase_inc1 = 0;
  s_alert.phase_inc2 = 0;
  s_alert.phase_inc3 = 0;
  s_alert.freq1 = 0;
  s_alert.freq2 = 0;
  s_alert.freq3 = 0;
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
    s_alert.phase3 = local.phase3;
    s_alert.phase_inc1 = local.phase_inc1;
    s_alert.phase_inc2 = local.phase_inc2;
    s_alert.phase_inc3 = local.phase_inc3;
    s_alert.freq1 = local.freq1;
    s_alert.freq2 = local.freq2;
    s_alert.freq3 = local.freq3;
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
