// Host tests for the parametric EQ (main/audio/audio_peq.c): REW parsing,
// filter shapes, the clip-proof preamp, bypass bit-exactness and the
// click-free switch between designs.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_peq.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS 48000

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while (0)

#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

static const char k_rew[] =
    "Filter Settings file\r\n"
    "\r\n"
    "Room EQ V5.31\r\n"
    "Dated: 24 Oct 2026 16:10:02\r\n"
    "\r\n"
    "Notes:\r\n"
    "\r\n"
    "Equaliser: Generic\r\n"
    "Bedroom L\r\n"
    "Preamp: -3.5 dB\r\n"
    "Filter  1: ON  PK       Fc   63.0 Hz  Gain  -5.0 dB  Q  4.00\r\n"
    "Filter  2: ON  LSC      Fc   105 Hz   Gain   3.5 dB  Q  0.71\r\n"
    "Filter  3: ON  HS       Fc   8000 Hz  Gain   2.0 dB\r\n"
    "Filter  4: ON  PK       Fc   1000 Hz  Gain  -3.0 dB  BW Oct 1.0\r\n"
    "Filter  5: ON  LS 6dB   Fc   100 Hz   Gain   3.0 dB\r\n"
    "Filter  6: OFF PK       Fc   200 Hz   Gain   3.0 dB  Q 1\r\n"
    "Filter  7: ON  None\r\n"
    "Filter  8: ON  HP       Fc   25 Hz    Q 0.5\r\n"
    "Filter  9: ON  HS 12dB  Fc   10000 Hz Gain 1.5 dB\r\n";

static void test_parse(void) {
  peq_channel_t ch;
  int skipped = -1;
  int n = peq_parse_rew(k_rew, strlen(k_rew), &ch, &skipped);
  CHECK(n == 6);
  CHECK(ch.count == 6);
  CHECK(skipped == 1); // LS 6dB
  NEAR(ch.preamp_db, -3.5, 1e-6);
  CHECK(ch.band[0].type == PEQ_PK);
  NEAR(ch.band[0].fc_hz, 63.0, 1e-4);
  NEAR(ch.band[0].gain_db, -5.0, 1e-6);
  NEAR(ch.band[0].q, 4.0, 1e-6);
  CHECK(ch.band[1].type == PEQ_LS);
  NEAR(ch.band[1].q, 0.71, 1e-6);
  CHECK(ch.band[2].type == PEQ_HS);
  NEAR(ch.band[2].q, 0.7071, 1e-4); // default when the file gives none
  CHECK(ch.band[3].type == PEQ_PK);
  NEAR(ch.band[3].q, 1.4142, 1e-3); // 1 octave
  CHECK(ch.band[4].type == PEQ_HP);
  NEAR(ch.band[4].q, 0.5, 1e-6);
  CHECK(ch.band[5].type == PEQ_HS);
  NEAR(ch.band[5].fc_hz, 10000.0, 1e-3);

  // Too many filters is an error, not a silent truncation.
  char big[4096] = "";
  for (int i = 0; i < PEQ_MAX_BANDS + 1; i++) {
    char line[96];
    snprintf(line, sizeof(line), "Filter %d: ON PK Fc %d Hz Gain -1 dB Q 1\n",
             i + 1, 100 + i * 50);
    strcat(big, line);
  }
  CHECK(peq_parse_rew(big, strlen(big), &ch, NULL) == -1);

  CHECK(peq_parse_rew("", 0, &ch, &skipped) == 0 && skipped == 0);
}

static peq_config_t one_band(peq_type_t t, float fc, float gain, float q) {
  peq_config_t cfg;
  peq_config_default(&cfg);
  for (int c = 0; c < PEQ_CHANNELS; c++) {
    cfg.ch[c].count = 1;
    cfg.ch[c].band[0] = (peq_band_t){(uint8_t)t, fc, gain, q};
  }
  return cfg;
}

static void test_shapes(void) {
  peq_design_t d;
  peq_config_t cfg = one_band(PEQ_PK, 1000, 6, 1.0f);
  peq_design(&cfg, FS, &d);
  CHECK(d.active);
  NEAR(d.peak_db[0], 6.0, 0.05);
  NEAR(d.preamp_db[0], -6.3, 0.05);
  NEAR(peq_design_response_db(&d, 0, 1000, FS), -0.3, 0.05);
  NEAR(peq_design_response_db(&d, 1, 20, FS), -6.3, 0.1);

  // A cut needs no preamp.
  cfg = one_band(PEQ_PK, 60, -8, 4.0f);
  peq_design(&cfg, FS, &d);
  NEAR(d.preamp_db[0], 0.0, 1e-6);
  NEAR(peq_design_response_db(&d, 0, 60, FS), -8.0, 0.05);
  NEAR(peq_design_response_db(&d, 0, 5000, FS), 0.0, 0.05);

  cfg = one_band(PEQ_LS, 100, 6, 0.7071f);
  peq_design(&cfg, FS, &d);
  NEAR(peq_design_response_db(&d, 0, 15, FS) - d.preamp_db[0], 6.0, 0.2);
  NEAR(peq_design_response_db(&d, 0, 100, FS) - d.preamp_db[0], 3.0, 0.1);
  NEAR(peq_design_response_db(&d, 0, 5000, FS) - d.preamp_db[0], 0.0, 0.1);

  cfg = one_band(PEQ_HS, 8000, 4, 0.7071f);
  peq_design(&cfg, FS, &d);
  NEAR(peq_design_response_db(&d, 0, 8000, FS) - d.preamp_db[0], 2.0, 0.1);
  NEAR(peq_design_response_db(&d, 0, 100, FS) - d.preamp_db[0], 0.0, 0.05);
  NEAR(peq_design_response_db(&d, 0, 20000, FS) - d.preamp_db[0], 4.0, 0.3);

  cfg = one_band(PEQ_HP, 40, 0, 0.7071f);
  peq_design(&cfg, FS, &d);
  NEAR(peq_design_response_db(&d, 0, 40, FS), -3.01, 0.05);
  NEAR(peq_design_response_db(&d, 0, 1000, FS), 0.0, 0.05);
  CHECK(peq_design_response_db(&d, 0, 10, FS) < -20.0);

  cfg = one_band(PEQ_LP, 12000, 0, 0.7071f);
  peq_design(&cfg, FS, &d);
  NEAR(peq_design_response_db(&d, 0, 12000, FS), -3.01, 0.05);

  // The user preamp is kept when it is already lower than needed.
  cfg = one_band(PEQ_PK, 1000, 3, 1.0f);
  cfg.ch[0].preamp_db = -10.0f;
  peq_design(&cfg, FS, &d);
  NEAR(d.preamp_db[0], -10.0, 1e-5);
  NEAR(d.preamp_db[1], -3.3, 0.05);
}

static void test_sanitize(void) {
  peq_config_t cfg;
  peq_config_default(&cfg);
  cfg.ch[0].count = 3;
  cfg.ch[0].band[0] = (peq_band_t){PEQ_PK, 1, 99, 0};
  cfg.ch[0].band[1] = (peq_band_t){PEQ_OFF, 100, 1, 1};
  cfg.ch[0].band[2] = (peq_band_t){PEQ_HS, 40000, -99, NAN};
  cfg.ch[0].preamp_db = 5.0f;
  cfg.ch[1].count = 200;
  peq_config_sanitize(&cfg, FS);
  CHECK(cfg.ch[0].count == 2);
  NEAR(cfg.ch[0].band[0].fc_hz, 10.0, 1e-6);
  NEAR(cfg.ch[0].band[0].gain_db, 12.0, 1e-6);
  NEAR(cfg.ch[0].band[0].q, 0.1, 1e-6);
  NEAR(cfg.ch[0].band[1].fc_hz, 21600.0, 1e-3);
  NEAR(cfg.ch[0].band[1].gain_db, -24.0, 1e-6);
  NEAR(cfg.ch[0].band[1].q, 0.1, 1e-6);
  NEAR(cfg.ch[0].preamp_db, 0.0, 1e-6);
  CHECK(cfg.ch[1].count == 0); // 200 zeroed bands, all OFF
}

#define BLOCK 352
#define SECS  1

static void sine(int16_t *buf, size_t frames, double f, double amp,
                 size_t *phase) {
  for (size_t i = 0; i < frames; i++) {
    double v = amp * sin(2 * M_PI * f * (double)(*phase + i) / FS);
    buf[2 * i] = buf[2 * i + 1] = (int16_t)lrint(v);
  }
  *phase += frames;
}

// RMS of channel 0 over the second half of a 1 s run through the design.
static double run_rms(const peq_design_t *d, double f, double amp, int *clipped,
                      int16_t *peak) {
  peq_runtime_t rt;
  peq_runtime_init(&rt);
  int16_t buf[BLOCK * 2];
  size_t phase = 0;
  double acc = 0;
  size_t n = 0;
  *clipped = 0;
  *peak = 0;
  bool first = true;
  for (int b = 0; b < SECS * FS / BLOCK; b++) {
    sine(buf, BLOCK, f, amp, &phase);
    peq_process(&rt, first ? d : NULL, buf, BLOCK);
    first = false;
    for (int i = 0; i < BLOCK; i++) {
      int16_t s = buf[2 * i];
      if (s == 32767 || s == -32768) {
        (*clipped)++;
      }
      if (abs(s) > *peak) {
        *peak = (int16_t)abs(s);
      }
      if (b > SECS * FS / BLOCK / 2) {
        acc += (double)s * s;
        n++;
      }
    }
  }
  return sqrt(acc / (double)n);
}

static void test_process(void) {
  peq_design_t d;
  peq_config_t cfg = one_band(PEQ_PK, 1000, 6, 1.0f);
  peq_design(&cfg, FS, &d);
  int clipped;
  int16_t peak;
  double amp = 8000;
  double rms = run_rms(&d, 1000, amp, &clipped, &peak);
  NEAR(20 * log10(rms / (amp / sqrt(2))), -0.3, 0.1);
  rms = run_rms(&d, 100, amp, &clipped, &peak);
  NEAR(20 * log10(rms / (amp / sqrt(2))),
       peq_design_response_db(&d, 0, 100, FS), 0.1);

  // Full scale at the centre of a +12 dB boost: the preamp keeps it clean.
  cfg = one_band(PEQ_PK, 1000, 12, 2.0f);
  peq_design(&cfg, FS, &d);
  rms = run_rms(&d, 1000, 32767, &clipped, &peak);
  CHECK(clipped == 0);
  CHECK(peak < 32767);
}

static void test_bypass_and_silence(void) {
  peq_runtime_t rt;
  peq_runtime_init(&rt);
  int16_t buf[BLOCK * 2], ref[BLOCK * 2];
  size_t phase = 0;
  sine(buf, BLOCK, 440, 12345, &phase);
  memcpy(ref, buf, sizeof(buf));
  peq_process(&rt, NULL, buf, BLOCK); // never configured
  CHECK(memcmp(buf, ref, sizeof(buf)) == 0);

  peq_config_t cfg = one_band(PEQ_PK, 1000, 6, 1.0f);
  cfg.enabled = false;
  peq_design_t d;
  peq_design(&cfg, FS, &d);
  CHECK(!d.active);
  peq_process(&rt, &d, buf, BLOCK);
  CHECK(memcmp(buf, ref, sizeof(buf)) == 0);

  // Active design on digital silence: no dither noise.
  cfg.enabled = true;
  peq_design(&cfg, FS, &d);
  peq_process(&rt, &d, buf, BLOCK);
  int16_t zero[BLOCK * 2];
  for (int k = 0; k < 200; k++) {
    memset(buf, 0, sizeof(buf));
    peq_process(&rt, NULL, buf, BLOCK);
  }
  memset(zero, 0, sizeof(zero));
  CHECK(memcmp(buf, zero, sizeof(buf)) == 0);
}

// Toggle between designs every block while a sine plays: the largest step
// between consecutive samples must stay what the sine itself produces.
static void test_switch_is_smooth(void) {
  peq_design_t on, off;
  peq_config_t cfg = one_band(PEQ_PK, 1000, 9, 1.0f);
  peq_design(&cfg, FS, &on);
  cfg.enabled = false;
  peq_design(&cfg, FS, &off);

  peq_runtime_t rt;
  peq_runtime_init(&rt);
  int16_t buf[BLOCK * 2];
  size_t phase = 0;
  double amp = 10000;
  double max_slope = 2 * M_PI * 1000.0 / FS * amp; // sine's own max step
  int16_t last = 0;
  double worst = 0;
  for (int b = 0; b < 60; b++) {
    sine(buf, BLOCK, 1000, amp, &phase);
    const peq_design_t *next = NULL;
    if (b % 4 == 1) {
      next = &on;
    } else if (b % 4 == 3) {
      next = &off;
    }
    peq_process(&rt, next, buf, BLOCK);
    for (int i = 0; i < BLOCK; i++) {
      if (b > 0 || i > 0) {
        double step = fabs((double)buf[2 * i] - last);
        if (step > worst) {
          worst = step;
        }
      }
      last = buf[2 * i];
    }
  }
  CHECK(worst < 1.15 * max_slope);
}

int main(void) {
  test_parse();
  test_shapes();
  test_sanitize();
  test_process();
  test_bypass_and_silence();
  test_switch_is_smooth();
  if (failures) {
    fprintf(stderr, "peq tests: %d failure(s)\n", failures);
    return 1;
  }
  printf("peq tests passed\n");
  return 0;
}
