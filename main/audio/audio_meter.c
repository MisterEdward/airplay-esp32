#include "audio_meter.h"

#include <math.h>
#include <string.h>

void audio_meter_init(audio_meter_t *m, uint32_t rate, float bass_hz) {
  memset(m, 0, sizeof(*m));
  m->rate = rate;
  m->a = 1.0f - expf(-2.0f * 3.14159265f * bass_hz / (float)rate);
  m->out.rms_l = m->out.rms_r = -120.0f;
}

static float dbfs(double sum_sq, uint32_t n) {
  if (n == 0 || sum_sq <= 0.0) {
    return -120.0f;
  }
  return (float)(10.0 * log10(sum_sq / n / (32768.0 * 32768.0)));
}

static void publish(audio_meter_t *m) {
  audio_meter_reading_t r;
  r.rms_l = dbfs(m->ll, m->n);
  r.rms_r = dbfs(m->rr, m->n);
  r.bass_l = m->ll > 0 ? (float)(m->bl / m->ll) : 0.0f;
  r.bass_r = m->rr > 0 ? (float)(m->br / m->rr) : 0.0f;
  double d = sqrt(m->ll * m->rr);
  r.corr = d > 0 ? (float)(m->lr / d) : 0.0f;
  r.seconds = m->out.seconds + 1;
  m->seq++;
  m->out = r;
  m->seq++;
  m->ll = m->rr = m->lr = m->bl = m->br = 0;
  m->n = 0;
}

void audio_meter_feed(audio_meter_t *m, const int16_t *lr, size_t frames) {
  float a = m->a;
  for (size_t i = 0; i < frames; i++) {
    float l = lr[2 * i], r = lr[2 * i + 1];
    m->l1 += a * (l - m->l1);
    m->l2 += a * (m->l1 - m->l2);
    m->r1 += a * (r - m->r1);
    m->r2 += a * (m->r1 - m->r2);
    m->ll += (double)(l * l);
    m->rr += (double)(r * r);
    m->lr += (double)(l * r);
    m->bl += (double)(m->l2 * m->l2);
    m->br += (double)(m->r2 * m->r2);
    if (++m->n >= m->rate) {
      publish(m);
    }
  }
}

void audio_meter_read(const audio_meter_t *m, audio_meter_reading_t *r) {
  uint32_t s;
  do {
    s = m->seq;
    *r = m->out;
  } while ((s & 1) || s != m->seq);
}
