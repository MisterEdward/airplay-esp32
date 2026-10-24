#include "audio_peq.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PEQ_FC_MIN_HZ   10.0f
#define PEQ_GAIN_MIN_DB -24.0f
#define PEQ_GAIN_MAX_DB 12.0f
#define PEQ_Q_MIN       0.1f
#define PEQ_Q_MAX       20.0f
#define PEQ_Q_DEFAULT   0.7071f

// Preamp margin over the grid peak: a 1024-point log grid can land just off
// a narrow peak, and steady-state magnitude says nothing about overshoot.
#define PEQ_HEADROOM_DB 0.3f
#define PEQ_GRID_POINTS 1024

static float clampf(float v, float lo, float hi) {
  if (!(v >= lo)) { // also catches NaN
    return lo;
  }
  return v > hi ? hi : v;
}

/* ------------------------------------------------------------------ */
/*  Configuration                                                      */
/* ------------------------------------------------------------------ */

void peq_config_default(peq_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->version = PEQ_CONFIG_VERSION;
  cfg->enabled = true;
}

void peq_config_sanitize(peq_config_t *cfg, uint32_t sample_rate) {
  float fc_max = 0.45f * (float)sample_rate;
  cfg->version = PEQ_CONFIG_VERSION;
  for (int c = 0; c < PEQ_CHANNELS; c++) {
    peq_channel_t *ch = &cfg->ch[c];
    int n = ch->count > PEQ_MAX_BANDS ? PEQ_MAX_BANDS : ch->count;
    int kept = 0;
    for (int i = 0; i < n; i++) {
      peq_band_t b = ch->band[i];
      if (b.type == PEQ_OFF || b.type >= PEQ_TYPE_COUNT) {
        continue;
      }
      b.fc_hz = clampf(b.fc_hz, PEQ_FC_MIN_HZ, fc_max);
      b.gain_db = clampf(b.gain_db, PEQ_GAIN_MIN_DB, PEQ_GAIN_MAX_DB);
      b.q = clampf(b.q, PEQ_Q_MIN, PEQ_Q_MAX);
      ch->band[kept++] = b;
    }
    for (int i = kept; i < PEQ_MAX_BANDS; i++) {
      memset(&ch->band[i], 0, sizeof(ch->band[i]));
    }
    ch->count = (uint8_t)kept;
    ch->preamp_db = clampf(ch->preamp_db, PEQ_MIN_PREAMP_DB, PEQ_MAX_PREAMP_DB);
  }
}

/* ------------------------------------------------------------------ */
/*  Design (cold path: runs on a web request, never per sample)        */
/* ------------------------------------------------------------------ */

typedef struct {
  double b0, b1, b2, a1, a2;
} dbiquad_t;

// RBJ audio-EQ cookbook, normalised by a0.
static dbiquad_t design_band(const peq_band_t *b, double fs) {
  double w0 = 2.0 * M_PI * (double)b->fc_hz / fs;
  double cw = cos(w0);
  double alpha = sin(w0) / (2.0 * (double)b->q);
  double A = pow(10.0, (double)b->gain_db / 40.0);
  double sa = 2.0 * sqrt(A) * alpha;
  double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

  switch ((peq_type_t)b->type) {
  case PEQ_PK:
    b0 = 1.0 + alpha * A;
    b1 = -2.0 * cw;
    b2 = 1.0 - alpha * A;
    a0 = 1.0 + alpha / A;
    a1 = -2.0 * cw;
    a2 = 1.0 - alpha / A;
    break;
  case PEQ_LS:
    b0 = A * ((A + 1) - (A - 1) * cw + sa);
    b1 = 2 * A * ((A - 1) - (A + 1) * cw);
    b2 = A * ((A + 1) - (A - 1) * cw - sa);
    a0 = (A + 1) + (A - 1) * cw + sa;
    a1 = -2 * ((A - 1) + (A + 1) * cw);
    a2 = (A + 1) + (A - 1) * cw - sa;
    break;
  case PEQ_HS:
    b0 = A * ((A + 1) + (A - 1) * cw + sa);
    b1 = -2 * A * ((A - 1) + (A + 1) * cw);
    b2 = A * ((A + 1) + (A - 1) * cw - sa);
    a0 = (A + 1) - (A - 1) * cw + sa;
    a1 = 2 * ((A - 1) - (A + 1) * cw);
    a2 = (A + 1) - (A - 1) * cw - sa;
    break;
  case PEQ_LP:
    b0 = (1 - cw) / 2;
    b1 = 1 - cw;
    b2 = (1 - cw) / 2;
    a0 = 1 + alpha;
    a1 = -2 * cw;
    a2 = 1 - alpha;
    break;
  case PEQ_HP:
    b0 = (1 + cw) / 2;
    b1 = -(1 + cw);
    b2 = (1 + cw) / 2;
    a0 = 1 + alpha;
    a1 = -2 * cw;
    a2 = 1 - alpha;
    break;
  default:
    break;
  }
  dbiquad_t q = {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
  return q;
}

static double cascade_mag2(const dbiquad_t *bq, int n, double w) {
  double cw = cos(w), sw = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
  double mag2 = 1.0;
  for (int i = 0; i < n; i++) {
    double nr = bq[i].b0 + bq[i].b1 * cw + bq[i].b2 * c2;
    double ni = -(bq[i].b1 * sw + bq[i].b2 * s2);
    double dr = 1.0 + bq[i].a1 * cw + bq[i].a2 * c2;
    double di = -(bq[i].a1 * sw + bq[i].a2 * s2);
    double den = dr * dr + di * di;
    if (den > 1e-30) {
      mag2 *= (nr * nr + ni * ni) / den;
    }
  }
  return mag2;
}

void peq_design(const peq_config_t *cfg, uint32_t sample_rate,
                peq_design_t *out) {
  peq_config_t c = *cfg;
  peq_config_sanitize(&c, sample_rate);
  memset(out, 0, sizeof(*out));
  double fs = (double)sample_rate;
  bool any = false;

  for (int ch = 0; ch < PEQ_CHANNELS; ch++) {
    dbiquad_t d[PEQ_MAX_BANDS];
    int n = c.ch[ch].count;
    for (int i = 0; i < n; i++) {
      d[i] = design_band(&c.ch[ch].band[i], fs);
      out->bq[ch][i] =
          (peq_biquad_t){(float)d[i].b0, (float)d[i].b1, (float)d[i].b2,
                         (float)d[i].a1, (float)d[i].a2};
    }
    out->count[ch] = (uint8_t)n;

    double peak2 = 1.0;
    if (n > 0) {
      double f_lo = 10.0, f_hi = fs * 0.4995;
      double ratio = log(f_hi / f_lo);
      for (int p = 0; p < PEQ_GRID_POINTS; p++) {
        double f = f_lo * exp(ratio * p / (PEQ_GRID_POINTS - 1));
        double m2 = cascade_mag2(d, n, 2.0 * M_PI * f / fs);
        if (m2 > peak2) {
          peak2 = m2;
        }
      }
    }
    float peak_db = (float)(10.0 * log10(peak2));
    float pre = c.ch[ch].preamp_db;
    float needed = peak_db > 0.0f ? -(peak_db + PEQ_HEADROOM_DB) : 0.0f;
    if (needed < pre) {
      pre = needed;
    }
    out->peak_db[ch] = peak_db;
    out->preamp_db[ch] = pre;
    out->gain[ch] = powf(10.0f, pre / 20.0f);
    if (n > 0 || pre < 0.0f) {
      any = true;
    }
  }
  out->active = c.enabled && any;
  if (!out->active) {
    for (int ch = 0; ch < PEQ_CHANNELS; ch++) {
      out->count[ch] = 0;
      out->gain[ch] = 1.0f;
    }
  }
}

float peq_design_response_db(const peq_design_t *d, int ch, float f_hz,
                             uint32_t sample_rate) {
  if (!d->active || ch < 0 || ch >= PEQ_CHANNELS) {
    return 0.0f;
  }
  dbiquad_t bq[PEQ_MAX_BANDS];
  for (int i = 0; i < d->count[ch]; i++) {
    const peq_biquad_t *s = &d->bq[ch][i];
    bq[i] = (dbiquad_t){s->b0, s->b1, s->b2, s->a1, s->a2};
  }
  double m2 = cascade_mag2(bq, d->count[ch],
                           2.0 * M_PI * (double)f_hz / (double)sample_rate);
  return (float)(10.0 * log10(m2)) + 20.0f * log10f(d->gain[ch]);
}

/* ------------------------------------------------------------------ */
/*  REW / Equalizer APO filter lists                                   */
/* ------------------------------------------------------------------ */

static const char *const k_type_names[PEQ_TYPE_COUNT] = {"OFF", "PK", "LS",
                                                         "HS",  "LP", "HP"};

const char *peq_type_name(peq_type_t t) {
  return t < PEQ_TYPE_COUNT ? k_type_names[t] : "OFF";
}

static bool ieq(const char *a, const char *b) {
  while (*a && *b) {
    if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
      return false;
    }
    a++;
    b++;
  }
  return *a == *b;
}

peq_type_t peq_type_from_name(const char *name) {
  static const struct {
    const char *name;
    peq_type_t type;
  } k_alias[] = {
      {"PK", PEQ_PK},  {"PEQ", PEQ_PK}, {"Modal", PEQ_PK}, {"LS", PEQ_LS},
      {"LSC", PEQ_LS}, {"LSQ", PEQ_LS}, {"HS", PEQ_HS},    {"HSC", PEQ_HS},
      {"HSQ", PEQ_HS}, {"LP", PEQ_LP},  {"LPQ", PEQ_LP},   {"HP", PEQ_HP},
      {"HPQ", PEQ_HP},
  };
  for (size_t i = 0; i < sizeof(k_alias) / sizeof(k_alias[0]); i++) {
    if (ieq(name, k_alias[i].name)) {
      return k_alias[i].type;
    }
  }
  return PEQ_OFF;
}

#define MAX_TOKENS 24
#define TOKEN_LEN  24

// Split a line on blanks and ':' (REW writes "Filter  1: ON").
static int tokenize(const char *line, size_t len, char tok[][TOKEN_LEN]) {
  int n = 0;
  size_t i = 0;
  while (i < len && n < MAX_TOKENS) {
    while (i < len && (isspace((unsigned char)line[i]) || line[i] == ':')) {
      i++;
    }
    size_t k = 0;
    while (i < len && !isspace((unsigned char)line[i]) && line[i] != ':') {
      if (k < TOKEN_LEN - 1) {
        tok[n][k++] = line[i];
      }
      i++;
    }
    if (k) {
      tok[n][k] = '\0';
      n++;
    }
  }
  return n;
}

static bool to_float(const char *s, float *v) {
  char *end = NULL;
  double d = strtod(s, &end);
  if (end == s || !isfinite(d)) {
    return false;
  }
  *v = (float)d;
  return true;
}

// One "Filter N: ON TYPE Fc .. Gain .. Q .." line.  Returns 1 parsed,
// 0 ignored (off / none), -1 not understood.
static int parse_filter(char tok[][TOKEN_LEN], int n, peq_band_t *b) {
  int i = 1;
  if (i < n && isdigit((unsigned char)tok[i][0])) {
    i++; // filter number
  }
  if (i >= n || !ieq(tok[i], "ON")) {
    return 0;
  }
  i++;
  if (i >= n || ieq(tok[i], "None")) {
    return 0;
  }
  peq_type_t type = peq_type_from_name(tok[i]);
  if (type == PEQ_OFF) {
    return -1;
  }
  i++;
  // "LS 6dB" is a first-order shelf, which this cascade does not do;
  // "LS 12dB" is the default second-order one.
  if (i < n && (ieq(tok[i], "6dB") || ieq(tok[i], "12dB"))) {
    if (ieq(tok[i], "6dB")) {
      return -1;
    }
    i++;
  }
  memset(b, 0, sizeof(*b));
  b->type = (uint8_t)type;
  b->q = PEQ_Q_DEFAULT;
  bool have_fc = false, have_gain = false;
  for (; i < n; i++) {
    float v;
    if (ieq(tok[i], "Fc") && i + 1 < n && to_float(tok[i + 1], &v)) {
      b->fc_hz = v;
      have_fc = true;
      i++;
    } else if (ieq(tok[i], "Gain") && i + 1 < n && to_float(tok[i + 1], &v)) {
      b->gain_db = v;
      have_gain = true;
      i++;
    } else if (ieq(tok[i], "Q") && i + 1 < n && to_float(tok[i + 1], &v)) {
      b->q = v;
      i++;
    } else if (ieq(tok[i], "BW") && i + 2 < n && ieq(tok[i + 1], "Oct") &&
               to_float(tok[i + 2], &v) && v > 0.0f) {
      double p = pow(2.0, (double)v);
      b->q = (float)(sqrt(p) / (p - 1.0));
      i += 2;
    }
  }
  bool needs_gain = type == PEQ_PK || type == PEQ_LS || type == PEQ_HS;
  if (!have_fc || (needs_gain && !have_gain)) {
    return -1;
  }
  return 1;
}

int peq_parse_rew(const char *text, size_t len, peq_channel_t *out,
                  int *skipped) {
  memset(out, 0, sizeof(*out));
  int bad = 0;
  int count = 0;
  bool overflow = false;
  size_t pos = 0;
  while (pos < len) {
    size_t end = pos;
    while (end < len && text[end] != '\n' && text[end] != '\r') {
      end++;
    }
    char tok[MAX_TOKENS][TOKEN_LEN];
    int n = tokenize(text + pos, end - pos, tok);
    if (n >= 2 && ieq(tok[0], "Preamp")) {
      float v;
      if (to_float(tok[1], &v)) {
        out->preamp_db = v;
      }
    } else if (n >= 3 && ieq(tok[0], "Filter")) {
      peq_band_t b;
      int r = parse_filter(tok, n, &b);
      if (r < 0) {
        bad++;
      } else if (r > 0) {
        if (count < PEQ_MAX_BANDS) {
          out->band[count++] = b;
        } else {
          overflow = true;
        }
      }
    }
    pos = end + 1;
  }
  out->count = (uint8_t)count;
  if (skipped) {
    *skipped = bad;
  }
  return overflow ? -1 : count;
}

/* ------------------------------------------------------------------ */
/*  Render path                                                        */
/* ------------------------------------------------------------------ */

void peq_runtime_init(peq_runtime_t *rt) {
  memset(rt, 0, sizeof(*rt));
  rt->cur.gain[0] = rt->cur.gain[1] = 1.0f;
  rt->rng = 0x9E3779B9u;
}

void peq_runtime_reset(peq_runtime_t *rt) {
  memset(rt->st, 0, sizeof(rt->st));
}

// Run one channel of a design over a chunk, in place.
static void run_cascade(const peq_design_t *d, int ch, peq_state_t *st,
                        float *buf, int n) {
  if (!d->active) {
    return;
  }
  float g = d->gain[ch];
  for (int k = 0; k < n; k++) {
    buf[k] *= g;
  }
  for (int i = 0; i < d->count[ch]; i++) {
    const peq_biquad_t c = d->bq[ch][i];
    float x1 = st[i].x1, x2 = st[i].x2, y1 = st[i].y1, y2 = st[i].y2;
    for (int k = 0; k < n; k++) {
      float x = buf[k];
      float y = c.b0 * x + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
      x2 = x1;
      x1 = x;
      y2 = y1;
      y1 = y;
      buf[k] = y;
    }
    // Decaying state would otherwise sink into denormals on silence.
    if (fabsf(y1) < 1e-20f && fabsf(y2) < 1e-20f) {
      y1 = y2 = 0.0f;
    }
    if (fabsf(x1) < 1e-20f && fabsf(x2) < 1e-20f) {
      x1 = x2 = 0.0f;
    }
    st[i].x1 = x1;
    st[i].x2 = x2;
    st[i].y1 = y1;
    st[i].y2 = y2;
  }
}

static inline uint32_t xorshift(uint32_t *s) {
  uint32_t x = *s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *s = x;
  return x;
}

// TPDF dither, +-1 LSB triangular.
static inline float tpdf(uint32_t *s) {
  const float k = 1.0f / 4294967296.0f;
  return ((float)xorshift(s) - (float)xorshift(s)) * k;
}

void peq_process(peq_runtime_t *rt, const peq_design_t *next, int16_t *pcm,
                 size_t frames) {
  if (next && !next->active && !rt->cur.active) {
    rt->cur = *next; // bypass to bypass: nothing to fade
    next = NULL;
  }
  bool fade = next != NULL;
  if (!fade && !rt->cur.active) {
    return; // bypass: bit-exact
  }
  if (fade && !rt->cur.active) {
    // Coming out of bypass: the history is stale.  The crossfade starts at
    // the dry signal, so the cascade's start-up transient is faded in.
    peq_runtime_reset(rt);
  }

  float *in = rt->in, *wet = rt->wet, *dry = rt->dry;
  for (int ch = 0; ch < PEQ_CHANNELS; ch++) {
    peq_state_t old_st[PEQ_MAX_BANDS];
    if (fade) {
      memcpy(old_st, rt->st[ch], sizeof(old_st));
    }
    for (size_t base = 0; base < frames; base += PEQ_CHUNK) {
      int n = (int)(frames - base < PEQ_CHUNK ? frames - base : PEQ_CHUNK);
      bool signal = false;
      for (int k = 0; k < n; k++) {
        int16_t s = pcm[(base + (size_t)k) * 2 + (size_t)ch];
        in[k] = (float)s;
        signal |= s != 0;
      }
      memcpy(wet, in, sizeof(float) * (size_t)n);
      if (fade) {
        memcpy(dry, in, sizeof(float) * (size_t)n);
        run_cascade(&rt->cur, ch, old_st, dry, n);
        run_cascade(next, ch, rt->st[ch], wet, n);
        for (int k = 0; k < n; k++) {
          float t = (float)(base + (size_t)k + 1) / (float)frames;
          wet[k] = dry[k] + (wet[k] - dry[k]) * t;
        }
      } else {
        run_cascade(&rt->cur, ch, rt->st[ch], wet, n);
      }
      for (int k = 0; k < n; k++) {
        float y = wet[k];
        // Silence in, silence out: a ringing tail below half an LSB, on a
        // zero input, is not worth turning into dither noise.
        if (signal || fabsf(y) >= 0.5f) {
          y += tpdf(&rt->rng);
        }
        // Round half away from zero: one trunc.s, where lrintf() is a call.
        long q = (long)(y >= 0.0f ? y + 0.5f : y - 0.5f);
        if (q > 32767) {
          q = 32767;
        } else if (q < -32768) {
          q = -32768;
        }
        pcm[(base + (size_t)k) * 2 + (size_t)ch] = (int16_t)q;
      }
    }
  }
  if (fade) {
    rt->cur = *next;
  }
}
