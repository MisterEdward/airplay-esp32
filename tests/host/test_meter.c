// Host tests for main/audio/audio_meter.c.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio_meter.h"

#define RATE 48000

static void run(float f_hz, float amp, int invert_r, audio_meter_reading_t *r) {
  audio_meter_t m;
  audio_meter_init(&m, RATE, 150.0f);
  int16_t buf[2 * 480];
  for (int b = 0; b < 2 * RATE / 480; b++) { // 2 s
    for (int i = 0; i < 480; i++) {
      double t = (double)(b * 480 + i) / RATE;
      int16_t s = (int16_t)(amp * 32767.0 * sin(2 * M_PI * f_hz * t));
      buf[2 * i] = s;
      buf[2 * i + 1] = invert_r ? (int16_t)-s : s;
    }
    audio_meter_feed(&m, buf, 480);
  }
  audio_meter_read(&m, r);
}

int main(void) {
  audio_meter_reading_t r;
  run(50.0f, 0.5f, 0, &r);
  printf("50 Hz:   rms %.1f dB bass %.2f corr %.2f\n", r.rms_l, r.bass_l,
         r.corr);
  assert(r.seconds == 2 && r.bass_l > 0.7f && r.corr > 0.99f);
  assert(fabsf(r.rms_l - (-9.0f)) < 0.3f); // 0.5 full scale sine: -9.03 dB
  run(3000.0f, 0.5f, 0, &r);
  printf("3 kHz:   bass %.4f\n", r.bass_l);
  assert(r.bass_l < 0.01f);
  run(50.0f, 0.5f, 1, &r);
  printf("50 Hz inverted R: corr %.2f\n", r.corr);
  assert(r.corr < -0.99f);
  printf("test_meter: all passed\n");
  return 0;
}
