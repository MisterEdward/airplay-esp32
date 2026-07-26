#include "audio_eq.h"

#include "eq_events.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "settings.h"
#include <inttypes.h>
#include <math.h>
#include <string.h>

static const char *TAG = "audio_eq";

#ifdef CONFIG_AUDIO_EQ_SOFTWARE

/* ------------------------------------------------------------------ */
/*  Fixed-point format                                                 */
/* ------------------------------------------------------------------ */

/* Coefficients live in Q27.  The largest normalised coefficient this band
   table can produce is ~4.6 (band 14, +15 dB), so Q27's +-15.99 range keeps
   comfortable margin while resolving 7.5e-9 — enough for the near-unity
   poles of the 20 Hz band at 44.1 kHz. */
#define COEFF_SHIFT 27

/* Samples are carried through the cascade in Q12 rather than as plain
   int16.  Twelve fractional bits keep rounding noise from accumulating
   across fifteen chained biquads, and still leave 16x (24 dB) of headroom
   above full scale inside an int32 before the clamp below engages. */
#define SAMPLE_SHIFT 12

/* Hard bound on biquad state.  Never reached in normal use thanks to the
   automatic preamp, but an int32 wraparound here would be catastrophically
   loud, so saturate instead — a far kinder failure mode than wrapping. */
#define STATE_CLAMP ((int32_t)1 << 30)

/* ------------------------------------------------------------------ */
/*  Band table — identical to the TAS58xx backend                      */
/* ------------------------------------------------------------------ */

static const float k_band_freq[AUDIO_EQ_BANDS] = {
    20.0f,  31.5f,   50.0f,   80.0f,   125.0f,  200.0f,  315.0f,  500.0f,
    800.0f, 1250.0f, 2000.0f, 3150.0f, 5000.0f, 8000.0f, 16000.0f};

static const float k_band_q[AUDIO_EQ_BANDS] = {2.0f, 2.0f, 1.5f, 1.5f, 1.0f,
                                               1.0f, 0.9f, 0.9f, 0.8f, 0.8f,
                                               0.7f, 0.7f, 0.6f, 0.6f, 0.5f};

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
  int32_t b0, b1, b2; /* Q27, already normalised by a0 */
  int32_t a1, a2;     /* Q27, already normalised by a0 */
} eq_coeffs_t;

typedef struct {
  int32_t x1, x2; /* Q12 input history */
  int32_t y1, y2; /* Q12 output history */
} eq_state_t;

/* Coefficients in use by the playback task.  Only ever touched by that
   task, so it needs no locking to read them. */
static eq_coeffs_t s_active_coeffs[AUDIO_EQ_BANDS];
static bool s_active = false;

/* Hand-off buffer written by whoever calls audio_eq_set_*.  The playback
   task picks it up at a chunk boundary.  Guarded by s_lock, which is held
   only for the memcpy — never across coefficient maths. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static eq_coeffs_t s_pending_coeffs[AUDIO_EQ_BANDS];
static bool s_pending_active = false;
static volatile bool s_pending_valid = false;

/* [band][channel] */
static eq_state_t s_state[AUDIO_EQ_BANDS][2];

static float s_gains_db[AUDIO_EQ_BANDS];
static uint32_t s_sample_rate = 0;
static bool s_initialised = false;

/* ------------------------------------------------------------------ */
/*  Coefficient computation (cold path — runs on gain change only)     */
/* ------------------------------------------------------------------ */

static int32_t to_q27(double v) {
  double scaled = v * (double)(1 << COEFF_SHIFT);
  /* Saturate rather than wrap; the band table cannot reach this, but a
     future band definition with an extreme Q might. */
  if (scaled > 2147483647.0) {
    return INT32_MAX;
  }
  if (scaled < -2147483648.0) {
    return INT32_MIN;
  }
  return (int32_t)(scaled >= 0 ? scaled + 0.5 : scaled - 0.5);
}

/**
 * Recompute the whole cascade from s_gains_db and publish it.
 *
 * Uses the RBJ audio-EQ cookbook peaking filter.  The automatic preamp is
 * folded into band 0's feed-forward coefficients so that boosting costs
 * nothing extra in the per-sample loop.
 */
static void rebuild_coeffs(void) {
  /* Normalised double-precision coefficients, before the preamp is folded
     in.  Kept separately from the Q27 output because the preamp can only
     be computed once the whole cascade is known. */
  double nb0[AUDIO_EQ_BANDS], nb1[AUDIO_EQ_BANDS], nb2[AUDIO_EQ_BANDS];
  double na1[AUDIO_EQ_BANDS], na2[AUDIO_EQ_BANDS];
  eq_coeffs_t built[AUDIO_EQ_BANDS];
  bool any_nonzero = false;

  for (int i = 0; i < AUDIO_EQ_BANDS; i++) {
    if (s_gains_db[i] != 0.0f) {
      any_nonzero = true;
    }

    double a_gain = pow(10.0, (double)s_gains_db[i] / 40.0);
    double w0 = 2.0 * M_PI * (double)k_band_freq[i] / (double)s_sample_rate;
    double cos_w0 = cos(w0);
    double alpha = sin(w0) / (2.0 * (double)k_band_q[i]);

    double a0 = 1.0 + alpha / a_gain;
    nb0[i] = (1.0 + alpha * a_gain) / a0;
    nb1[i] = (-2.0 * cos_w0) / a0;
    nb2[i] = (1.0 - alpha * a_gain) / a0;
    na1[i] = (-2.0 * cos_w0) / a0;
    na2[i] = (1.0 - alpha / a_gain) / a0;
  }

  /* Preamp.
   *
   * Boosted bands must not drive the output into the rails, so the whole
   * cascade is attenuated by its own peak gain.  Note this cannot be
   * approximated as -max(band gain): neighbouring bands overlap and their
   * responses compound, so e.g. the UI's "Bass Boost" preset
   * (+8/+6/+5/+4/+2 on adjacent bands) peaks well above its +8 dB maximum
   * and clipped audibly when that shortcut was used.
   *
   * Evaluate |H(e^jw)| for the full cascade on a log-spaced grid and take
   * the maximum.  Cold path — this runs on a web request, never per sample. */
  double peak = 1.0;
  const int kGridPoints = 512;
  double f_lo = 10.0;
  double f_hi = (double)s_sample_rate * 0.499;
  for (int p = 0; p < kGridPoints; p++) {
    double frac = (double)p / (double)(kGridPoints - 1);
    double f = f_lo * pow(f_hi / f_lo, frac);
    double w = 2.0 * M_PI * f / (double)s_sample_rate;
    double cw = cos(w), sw = sin(w);
    double c2w = cos(2.0 * w), s2w = sin(2.0 * w);

    double mag = 1.0;
    for (int i = 0; i < AUDIO_EQ_BANDS; i++) {
      double nr = nb0[i] + nb1[i] * cw + nb2[i] * c2w;
      double ni = nb1[i] * sw + nb2[i] * s2w;
      double dr = 1.0 + na1[i] * cw + na2[i] * c2w;
      double di = na1[i] * sw + na2[i] * s2w;
      double den = dr * dr + di * di;
      if (den > 1e-20) {
        mag *= sqrt((nr * nr + ni * ni) / den);
      }
    }
    if (mag > peak) {
      peak = mag;
    }
  }

  /* Small extra margin: the grid can land just off a narrow peak, and a
     steady-state magnitude bound does not capture the overshoot a filter
     shows on transients. */
  double preamp = 1.0 / (peak * 1.06); /* ~0.5 dB */
  if (preamp > 1.0) {
    preamp = 1.0; /* pure cut — no need to attenuate further */
  }

  for (int i = 0; i < AUDIO_EQ_BANDS; i++) {
    /* Fold the preamp into the first stage only, so it costs nothing in
       the per-sample loop. */
    double pre = (i == 0) ? preamp : 1.0;

    built[i].b0 = to_q27(pre * nb0[i]);
    built[i].b1 = to_q27(pre * nb1[i]);
    built[i].b2 = to_q27(pre * nb2[i]);
    built[i].a1 = to_q27(na1[i]);
    built[i].a2 = to_q27(na2[i]);
  }

  portENTER_CRITICAL(&s_lock);
  memcpy(s_pending_coeffs, built, sizeof(built));
  s_pending_active = any_nonzero;
  s_pending_valid = true;
  portEXIT_CRITICAL(&s_lock);
}

/* ------------------------------------------------------------------ */
/*  Hot path                                                           */
/* ------------------------------------------------------------------ */

static inline int32_t biquad_step(const eq_coeffs_t *c, eq_state_t *s,
                                  int32_t x0) {
  int64_t acc = (int64_t)c->b0 * x0;
  acc += (int64_t)c->b1 * s->x1;
  acc += (int64_t)c->b2 * s->x2;
  acc -= (int64_t)c->a1 * s->y1;
  acc -= (int64_t)c->a2 * s->y2;

  int32_t y0 = (int32_t)(acc >> COEFF_SHIFT);
  if (y0 > STATE_CLAMP) {
    y0 = STATE_CLAMP;
  } else if (y0 < -STATE_CLAMP) {
    y0 = -STATE_CLAMP;
  }

  s->x2 = s->x1;
  s->x1 = x0;
  s->y2 = s->y1;
  s->y1 = y0;
  return y0;
}

void audio_eq_process(int16_t *pcm, size_t frames) {
  if (!s_initialised || pcm == NULL || frames == 0) {
    return;
  }

  /* Pick up coefficients published by the web task.  Filter state is kept
     across the swap on purpose: continuous history through a small
     coefficient change is smoother than zeroing it, which would step the
     output and click. */
  if (s_pending_valid) {
    portENTER_CRITICAL(&s_lock);
    memcpy(s_active_coeffs, s_pending_coeffs, sizeof(s_active_coeffs));
    s_active = s_pending_active;
    s_pending_valid = false;
    portEXIT_CRITICAL(&s_lock);
  }

  if (!s_active) {
    return;
  }

  for (size_t i = 0; i < frames; i++) {
    for (int ch = 0; ch < 2; ch++) {
      int32_t x = (int32_t)pcm[i * 2 + (size_t)ch] << SAMPLE_SHIFT;

      for (int b = 0; b < AUDIO_EQ_BANDS; b++) {
        x = biquad_step(&s_active_coeffs[b], &s_state[b][ch], x);
      }

      /* Round back to int16 and saturate. */
      int32_t out = (x + (1 << (SAMPLE_SHIFT - 1))) >> SAMPLE_SHIFT;
      if (out > 32767) {
        out = 32767;
      } else if (out < -32768) {
        out = -32768;
      }
      pcm[i * 2 + (size_t)ch] = (int16_t)out;
    }
  }
}

void audio_eq_reset(void) {
  memset(s_state, 0, sizeof(s_state));
}

/* ------------------------------------------------------------------ */
/*  Control                                                            */
/* ------------------------------------------------------------------ */

esp_err_t audio_eq_set_all(const float gains_db[AUDIO_EQ_BANDS]) {
  if (!gains_db) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!s_initialised) {
    return ESP_ERR_INVALID_STATE;
  }

  for (int i = 0; i < AUDIO_EQ_BANDS; i++) {
    float g = gains_db[i];
    if (g > AUDIO_EQ_MAX_GAIN_DB) {
      g = AUDIO_EQ_MAX_GAIN_DB;
    } else if (g < AUDIO_EQ_MIN_GAIN_DB) {
      g = AUDIO_EQ_MIN_GAIN_DB;
    }
    s_gains_db[i] = g;
  }

  rebuild_coeffs();
  return ESP_OK;
}

esp_err_t audio_eq_set_band(int band, float gain_db) {
  if (band < 0 || band >= AUDIO_EQ_BANDS) {
    return ESP_ERR_INVALID_ARG;
  }
  float gains[AUDIO_EQ_BANDS];
  memcpy(gains, s_gains_db, sizeof(gains));
  gains[band] = gain_db;
  return audio_eq_set_all(gains);
}

esp_err_t audio_eq_flat(void) {
  float gains[AUDIO_EQ_BANDS] = {0};
  return audio_eq_set_all(gains);
}

void audio_eq_get_gains(float out[AUDIO_EQ_BANDS]) {
  if (out) {
    memcpy(out, s_gains_db, sizeof(s_gains_db));
  }
}

bool audio_eq_is_active(void) {
  return s_active || s_pending_active;
}

float audio_eq_get_center_freq(int band) {
  if (band < 0 || band >= AUDIO_EQ_BANDS) {
    return 0.0f;
  }
  return k_band_freq[band];
}

/* ------------------------------------------------------------------ */
/*  eq_events listener + init                                          */
/* ------------------------------------------------------------------ */

/**
 * Mirrors the TAS58xx listener in the Esparagus board file: apply to the
 * backend and persist.  Only one of the two is ever compiled in.
 */
static void on_eq_event(eq_event_t event, const eq_event_data_t *data,
                        void *user_data) {
  (void)user_data;

  switch (event) {
  case EQ_EVENT_ALL_BANDS_SET:
    if (data) {
      audio_eq_set_all(data->all_bands.gains_db);
      settings_set_eq_gains(data->all_bands.gains_db);
      ESP_LOGI(TAG, "EQ: all bands updated and saved");
    }
    break;

  case EQ_EVENT_BAND_CHANGED:
    if (data) {
      audio_eq_set_band(data->band_changed.band, data->band_changed.gain_db);
      float gains[AUDIO_EQ_BANDS];
      audio_eq_get_gains(gains);
      settings_set_eq_gains(gains);
    }
    break;

  case EQ_EVENT_FLAT:
    audio_eq_flat();
    settings_clear_eq();
    ESP_LOGI(TAG, "EQ: reset to flat");
    break;
  }
}

esp_err_t audio_eq_init(uint32_t sample_rate) {
  if (sample_rate == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  s_sample_rate = sample_rate;
  audio_eq_reset();

  if (!s_initialised) {
    memset(s_gains_db, 0, sizeof(s_gains_db));
    float saved[SETTINGS_EQ_BANDS];
    if (settings_get_eq_gains(saved) == ESP_OK) {
      memcpy(s_gains_db, saved, sizeof(s_gains_db));
    }
    s_initialised = true;
    eq_events_register(on_eq_event, NULL);
  }

  rebuild_coeffs();

  ESP_LOGI(TAG, "Software EQ ready: %d bands @ %" PRIu32 " Hz (%s)",
           AUDIO_EQ_BANDS, sample_rate,
           s_pending_active ? "active" : "flat/bypassed");
  return ESP_OK;
}

#endif /* CONFIG_AUDIO_EQ_SOFTWARE */
