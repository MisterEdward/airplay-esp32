// Passive level meter for a stereo int16 stream (host-tested, no IDF).
//
// Per second it publishes, for each channel, the RMS level and the share of
// energy below ~bass_hz, plus the left/right correlation.  Used to locate a
// "bass disappears after hours" report: one meter on the decoded stream, one
// on what goes to the DAC (audio_output.c).  It only reads samples.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
  float rms_l, rms_r;   // dBFS
  float bass_l, bass_r; // share of energy below bass_hz, 0..1
  float corr;           // L/R correlation, -1..1 (-1: one channel inverted)
  uint32_t seconds;     // number of published windows
} audio_meter_reading_t;

typedef struct {
  uint32_t rate;
  float a;              // one-pole coefficient
  float l1, l2, r1, r2; // two cascaded one-pole low-passes per channel
  double ll, rr, lr, bl, br;
  uint32_t n;
  volatile uint32_t seq; // odd while publishing
  audio_meter_reading_t out;
} audio_meter_t;

void audio_meter_init(audio_meter_t *m, uint32_t rate, float bass_hz);
void audio_meter_feed(audio_meter_t *m, const int16_t *lr, size_t frames);
// Consistent copy of the last published window (any task).
void audio_meter_read(const audio_meter_t *m, audio_meter_reading_t *r);
