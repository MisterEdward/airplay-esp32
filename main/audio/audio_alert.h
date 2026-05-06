#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  AUDIO_ALERT_ALARM = 0,
  AUDIO_ALERT_BEEP,
  AUDIO_ALERT_CHIME,
  AUDIO_ALERT_BELL,
} audio_alert_id_t;

typedef struct {
  audio_alert_id_t id;
  uint8_t volume_percent;
  uint8_t repeat;
} audio_alert_config_t;

esp_err_t audio_alert_init(void);
esp_err_t audio_alert_play(const audio_alert_config_t *config);
esp_err_t audio_alert_play_name(const char *name, uint8_t volume_percent,
                                uint8_t repeat);
void audio_alert_stop(void);
bool audio_alert_is_active(void);
const char *audio_alert_name(audio_alert_id_t id);
const char *audio_alert_names_json(void);
void audio_alert_mix(int16_t *pcm, size_t stereo_frames, uint32_t sample_rate);
