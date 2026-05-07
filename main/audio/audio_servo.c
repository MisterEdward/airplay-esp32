#include "audio_servo.h"

#include <stdint.h>
#include <string.h>

// ----- Tunables -----
// Maximum applied correction in parts-per-million.  500 ppm = 0.05% rate
// change = ~0.86 cents pitch shift, well under the 5–10 cent audibility
// threshold for the harshest sustained tones (and totally inaudible on music
// or speech).
#define MAX_CORRECTION_PPM 500

// Drift below this magnitude is treated as zero — prevents the servo from
// hunting on residual EMA noise once it has converged.
#define DRIFT_DEADBAND_US 5000

// Proportional gain expressed as time-to-recover.  target_ppm = -drift_us /
// TIME_CONSTANT_SEC, so 30 ms of drift produces 500 ppm correction at 60 s.
#define TIME_CONSTANT_SEC 60

// Per-update smoothing of the applied correction toward the target.  At ~125
// updates per second (audio_servo_update is called once per ~8 ms playback
// chunk), 0.005 yields a ~1.6 s time constant on rate changes — slow enough
// that the listener never perceives a "pitch glide".
#define SMOOTHING_NUMERATOR   1
#define SMOOTHING_DENOMINATOR 200

// ----- State -----
static struct {
  // Resampler state
  float phase;       // Sub-sample position within prev->curr segment
  float step;        // Phase advance per output sample (= 1 + correction)
  int16_t prev_l, prev_r;
  int16_t curr_l, curr_r;
  bool primed;

  // Controller state (parts-per-million)
  int32_t target_ppm;
  // Applied correction is derived from step on demand; store as int32 ppm
  // to avoid float drift when smoothing.
  int32_t applied_ppm;
} s_servo;

void audio_servo_init(void) {
  memset(&s_servo, 0, sizeof(s_servo));
  s_servo.step = 1.0f;
}

void audio_servo_reset(void) {
  s_servo.phase = 0.0f;
  s_servo.primed = false;
  // Leave applied_ppm/step alone — let the smoother walk it back.  An abrupt
  // reset to 1.0 would be more audible than a gradual return.
}

static inline int32_t clamp_ppm(int32_t v) {
  if (v > MAX_CORRECTION_PPM) {
    return MAX_CORRECTION_PPM;
  }
  if (v < -MAX_CORRECTION_PPM) {
    return -MAX_CORRECTION_PPM;
  }
  return v;
}

void audio_servo_update(int32_t drift_us, bool playing) {
  if (!playing) {
    s_servo.target_ppm = 0;
  } else if (drift_us > -DRIFT_DEADBAND_US && drift_us < DRIFT_DEADBAND_US) {
    s_servo.target_ppm = 0;
  } else {
    // Positive drift -> we are early -> slow down (negative correction).
    // Negative drift -> we are late  -> speed up (positive correction).
    int32_t target = -drift_us / TIME_CONSTANT_SEC;
    s_servo.target_ppm = clamp_ppm(target);
  }

  // Smooth applied correction toward target.  Integer math keeps state
  // deterministic and bit-exact across boots.
  int32_t delta = s_servo.target_ppm - s_servo.applied_ppm;
  int32_t step = (delta * SMOOTHING_NUMERATOR) / SMOOTHING_DENOMINATOR;
  // Ensure progress: never round to zero when delta is non-zero, otherwise
  // the integer divide stalls forever near the target.
  if (step == 0 && delta != 0) {
    step = (delta > 0) ? 1 : -1;
  }
  s_servo.applied_ppm = clamp_ppm(s_servo.applied_ppm + step);

  // Convert to step (input samples per output sample).  step = 1 + ppm/1e6.
  s_servo.step = 1.0f + (float)s_servo.applied_ppm / 1000000.0f;
}

int32_t audio_servo_get_correction_ppm(void) {
  return s_servo.applied_ppm;
}

size_t audio_servo_process(const int16_t *in, size_t in_frames, int16_t *out,
                           size_t out_capacity) {
  if (!in || !out || in_frames == 0 || out_capacity == 0) {
    return 0;
  }

  size_t in_idx = 0;
  size_t out_idx = 0;

  // Bootstrap: first input ever becomes both prev and curr.  Force phase=1.0
  // so the next iteration consumes ANOTHER input before producing — that
  // way curr ≠ prev and interpolation is meaningful from the very first
  // output.
  if (!s_servo.primed) {
    s_servo.prev_l = s_servo.curr_l = in[0];
    s_servo.prev_r = s_servo.curr_r = in[1];
    s_servo.primed = true;
    s_servo.phase = 1.0f;
    in_idx = 1;
  }

  while (out_idx < out_capacity) {
    // Advance through input until phase is in [0, 1) — the valid range for
    // interpolating between prev and curr.
    while (s_servo.phase >= 1.0f) {
      if (in_idx >= in_frames) {
        return out_idx;
      }
      s_servo.prev_l = s_servo.curr_l;
      s_servo.prev_r = s_servo.curr_r;
      s_servo.curr_l = in[in_idx * 2];
      s_servo.curr_r = in[in_idx * 2 + 1];
      s_servo.phase -= 1.0f;
      in_idx++;
    }

    // Linear interpolation between prev and curr at fractional position.
    float f = s_servo.phase;
    float one_minus_f = 1.0f - f;
    int32_t l = (int32_t)(one_minus_f * (float)s_servo.prev_l +
                          f * (float)s_servo.curr_l);
    int32_t r = (int32_t)(one_minus_f * (float)s_servo.prev_r +
                          f * (float)s_servo.curr_r);
    // Linear interp of int16 endpoints with f in [0,1) cannot overflow but
    // clamp anyway in case rounding pushes us by 1.
    if (l > 32767) {
      l = 32767;
    } else if (l < -32768) {
      l = -32768;
    }
    if (r > 32767) {
      r = 32767;
    } else if (r < -32768) {
      r = -32768;
    }

    out[out_idx * 2] = (int16_t)l;
    out[out_idx * 2 + 1] = (int16_t)r;
    out_idx++;

    s_servo.phase += s_servo.step;
  }

  return out_idx;
}
