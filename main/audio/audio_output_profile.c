#include "audio_output_profile.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "audio_output_profile_config.h"
#include "esp_log.h"

static const char *TAG = "audio_profile";

typedef struct {
  float b0;
  float b1;
  float b2;
  float a1;
  float a2;
  float x1[2];
  float x2[2];
  float y1[2];
  float y2[2];
  bool enabled;
} stereo_biquad_t;

static stereo_biquad_t bass_shelf;

static float map_output_db(float airplay_db) {
  if (airplay_db <= AUDIO_PROFILE_AIRPLAY_MIN_DB) {
    return -INFINITY;
  }
  if (airplay_db >= AUDIO_PROFILE_AIRPLAY_MAX_DB) {
    return AUDIO_PROFILE_OUTPUT_MAX_DB;
  }
  if (airplay_db <= AUDIO_PROFILE_AIRPLAY_MID_DB) {
    float position =
        (airplay_db - AUDIO_PROFILE_AIRPLAY_MIN_DB) /
        (AUDIO_PROFILE_AIRPLAY_MID_DB - AUDIO_PROFILE_AIRPLAY_MIN_DB);
    return AUDIO_PROFILE_OUTPUT_MIN_DB +
           position *
               (AUDIO_PROFILE_OUTPUT_MID_DB - AUDIO_PROFILE_OUTPUT_MIN_DB);
  }
  float position =
      (airplay_db - AUDIO_PROFILE_AIRPLAY_MID_DB) /
      (AUDIO_PROFILE_AIRPLAY_MAX_DB - AUDIO_PROFILE_AIRPLAY_MID_DB);
  return AUDIO_PROFILE_OUTPUT_MID_DB +
         position *
             (AUDIO_PROFILE_OUTPUT_MAX_DB - AUDIO_PROFILE_OUTPUT_MID_DB);
}

int32_t audio_output_profile_volume_q15(float airplay_db) {
  float output_db = map_output_db(airplay_db);
  if (!isfinite(output_db)) {
    return 0;
  }
  float gain = powf(10.0f, output_db / 20.0f);
  int32_t q15 = (int32_t)lrintf(gain * 32768.0f);
  if (q15 < 0) {
    return 0;
  }
  if (q15 > 32768) {
    return 32768;
  }
  return q15;
}

void audio_output_profile_init(uint32_t sample_rate) {
  bass_shelf = (stereo_biquad_t){0};
  if (sample_rate == 0 || AUDIO_PROFILE_BASS_SHELF_DB == 0.0f ||
      AUDIO_PROFILE_BASS_SHELF_HZ <= 0.0f) {
    ESP_LOGI(TAG, "Profile %s: tone correction disabled", AUDIO_PROFILE_NAME);
    return;
  }

  // RBJ low-shelf filter, slope S=1. A negative gain gently trims bass
  // without moving vocals or treble.
  const float pi = 3.14159265358979323846f;
  float amplitude = powf(10.0f, AUDIO_PROFILE_BASS_SHELF_DB / 40.0f);
  float omega = 2.0f * pi * AUDIO_PROFILE_BASS_SHELF_HZ / sample_rate;
  float cosine = cosf(omega);
  float sine = sinf(omega);
  float alpha = sine * 0.5f * sqrtf(2.0f);
  float two_sqrt_a_alpha = 2.0f * sqrtf(amplitude) * alpha;
  float a0 = (amplitude + 1.0f) +
             (amplitude - 1.0f) * cosine + two_sqrt_a_alpha;

  bass_shelf.b0 = amplitude *
                  ((amplitude + 1.0f) -
                   (amplitude - 1.0f) * cosine + two_sqrt_a_alpha) /
                  a0;
  bass_shelf.b1 = 2.0f * amplitude *
                  ((amplitude - 1.0f) -
                   (amplitude + 1.0f) * cosine) /
                  a0;
  bass_shelf.b2 = amplitude *
                  ((amplitude + 1.0f) -
                   (amplitude - 1.0f) * cosine - two_sqrt_a_alpha) /
                  a0;
  bass_shelf.a1 = -2.0f *
                  ((amplitude - 1.0f) +
                   (amplitude + 1.0f) * cosine) /
                  a0;
  bass_shelf.a2 = ((amplitude + 1.0f) +
                   (amplitude - 1.0f) * cosine - two_sqrt_a_alpha) /
                  a0;
  bass_shelf.enabled = true;
  ESP_LOGI(TAG, "Profile %s: max=%.1f dB bass=%.1f dB @ %.0f Hz",
           AUDIO_PROFILE_NAME, AUDIO_PROFILE_OUTPUT_MAX_DB,
           AUDIO_PROFILE_BASS_SHELF_DB, AUDIO_PROFILE_BASS_SHELF_HZ);
}

void audio_output_profile_reset(void) {
  memset(bass_shelf.x1, 0, sizeof(bass_shelf.x1));
  memset(bass_shelf.x2, 0, sizeof(bass_shelf.x2));
  memset(bass_shelf.y1, 0, sizeof(bass_shelf.y1));
  memset(bass_shelf.y2, 0, sizeof(bass_shelf.y2));
}

void audio_output_profile_process(int16_t *pcm, size_t frames) {
  if (!pcm || !bass_shelf.enabled) {
    return;
  }
  for (size_t frame = 0; frame < frames; frame++) {
    for (size_t channel = 0; channel < 2; channel++) {
      float input = pcm[frame * 2 + channel];
      float output = bass_shelf.b0 * input +
                     bass_shelf.b1 * bass_shelf.x1[channel] +
                     bass_shelf.b2 * bass_shelf.x2[channel] -
                     bass_shelf.a1 * bass_shelf.y1[channel] -
                     bass_shelf.a2 * bass_shelf.y2[channel];
      bass_shelf.x2[channel] = bass_shelf.x1[channel];
      bass_shelf.x1[channel] = input;
      bass_shelf.y2[channel] = bass_shelf.y1[channel];
      bass_shelf.y1[channel] = output;
      if (output > 32767.0f) {
        output = 32767.0f;
      } else if (output < -32768.0f) {
        output = -32768.0f;
      }
      pcm[frame * 2 + channel] = (int16_t)lrintf(output);
    }
  }
}
