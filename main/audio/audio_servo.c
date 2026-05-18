#include "audio_servo.h"

#include "esp_log.h"
#include "esp_timer.h"
#include <stdint.h>
#include <string.h>

// ----- Tunables -----
// Maximum steady-state correction in parts-per-million.  2000 ppm = 0.2% rate
// change = ~3.5 cents pitch shift, normally very hard to notice on music or
// speech, but strong enough to recover post-seek drift faster.
#define MAX_CORRECTION_PPM 2000

// Temporary catch-up after seek/scrub.  10000 ppm = 1% = ~17 cents, so it is
// not a "forever" setting; use only while phase error is obvious.
#define SEEK_BOOST_MAX_CORRECTION_PPM 10000
#define SEEK_BOOST_TIME_CONSTANT_SEC  3
#define SEEK_BOOST_DURATION_US        12000000LL
#define SEEK_BOOST_MIN_DURATION_US    2500000LL
#define SEEK_BOOST_EXIT_DRIFT_US      8000

// Persistent-late soft boost — used when the realtime UDP path has a steady
// negative drift (frame ≥ 30 ms late) that the normal ±2000 ppm window cannot
// recover from quickly enough.  Unlike the seek boost (±10000 ppm, ~17 cents),
// this targets 7000 ppm (~12 cents) — borderline audible only on sustained pure
// tones, inaudible on music/speech.
//
// Why 7000 and not 5000 (round 1):
//   Round-1 telemetry showed the boost plateauing at +4500 ppm with drift still
//   stuck at -90 to -180 ms.  Steady WiFi loss on the realtime path was adding
//   ~5 ms/s of new late-arrivals, so 5000 ppm exactly matched the loss rate but
//   never recovered existing drift.  7000 ppm gives a net ~2 ms/s catch-up.
//
// Why 300 s timeout (round 1 was 30 s):
//   Round-1 logs show the boost exiting on the 30 s timeout while drift was
//   still at -113 ms.  The condition it fights (WiFi packet loss) is
//   steady-state, not transient like a seek.  300 s is still a safety backstop
//   against a permanently-broken sender but lets the servo finish its work on
//   transient rough patches.  The convergence-exit (±10 ms for 1 s) remains the
//   primary exit — the timeout is only the backstop.
//
// Exits once drift returns to within ±10 ms for 1 s, or after 300 s safety
// timeout.  After exit, requires 5 s of normal operation before re-arming
// (hysteresis to avoid hunting).
#define SOFT_BOOST_MAX_CORRECTION_PPM   7000
#define SOFT_BOOST_TRIGGER_DRIFT_US     30000      // -30 ms late triggers
#define SOFT_BOOST_TRIGGER_DURATION_US  2000000LL  // must be late for 2 s
#define SOFT_BOOST_EXIT_DRIFT_US        10000      // ±10 ms resets the exit counter
#define SOFT_BOOST_EXIT_DURATION_US     1000000LL  // within ±10 ms for 1 s to exit
#define SOFT_BOOST_TIMEOUT_US           300000000LL // 300 s (5 min) safety backstop
#define SOFT_BOOST_REARM_DELAY_US       5000000LL   // 5 s before re-arm after exit

// Drift below this magnitude is treated as zero — prevents the servo from
// hunting on residual EMA noise once it has converged.
#define DRIFT_DEADBAND_US 5000

// Proportional gain expressed as time-to-recover.  target_ppm = -drift_us /
// TIME_CONSTANT_SEC, so 24 ms of drift produces 1000 ppm correction at 24 s.
#define TIME_CONSTANT_SEC 24

// Per-update smoothing of the applied correction toward the target.  At ~125
// updates per second (audio_servo_update is called once per ~8 ms playback
// chunk), 0.005 yields a ~1.6 s time constant on normal rate changes.  Seek
// boost uses a shorter smoothing window so it can actually reach high ppm.
#define SMOOTHING_NUMERATOR   1
#define SMOOTHING_DENOMINATOR 200
#define BOOST_SMOOTHING_DENOMINATOR 25

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
  int64_t boost_started_us;
  int64_t boost_until_us;

  // Persistent-late soft-boost state (see SOFT_BOOST_* tunables).
  // Separate from the seek boost so a non-seek steady-state drift condition
  // (common on the realtime UDP path when WiFi is marginal) can recover
  // without the full 10000 ppm seek-boost pitch shift.
  int64_t soft_boost_late_since_us;  // when persistent late started; 0=not late
  bool    soft_boost_active;
  int64_t soft_boost_started_us;
  int64_t soft_boost_on_time_since_us; // when we first entered the exit window; 0=not yet
  int64_t soft_boost_rearm_after_us;   // earliest time the boost can re-arm; 0=any time
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

static inline int32_t clamp_ppm_to(int32_t v, int32_t limit) {
  if (v > limit) {
    return limit;
  }
  if (v < -limit) {
    return -limit;
  }
  return v;
}

static inline int32_t abs_i32(int32_t v) {
  return v < 0 ? -v : v;
}

void audio_servo_start_seek_boost(void) {
  int64_t now_us = esp_timer_get_time();
  s_servo.boost_started_us = now_us;
  s_servo.boost_until_us = now_us + SEEK_BOOST_DURATION_US;
}

static const char *TAG = "audio_servo";

void audio_servo_update(int32_t drift_us, bool playing) {
  int64_t now_us = esp_timer_get_time();
  bool boost_active = playing && s_servo.boost_until_us > now_us;
  bool boost_min_elapsed =
      now_us - s_servo.boost_started_us >= SEEK_BOOST_MIN_DURATION_US;
  if (boost_active && boost_min_elapsed &&
      abs_i32(drift_us) < SEEK_BOOST_EXIT_DRIFT_US) {
    s_servo.boost_until_us = 0;
    boost_active = false;
  }

  // Persistent-late soft boost: raise ppm_limit to SOFT_BOOST_MAX_CORRECTION_PPM
  // when steady-state drift is persistently ≥ 30 ms late and the seek boost
  // is NOT active (the seek boost already covers the larger seek-gap case).
  // This recovers the common realtime-UDP scenario where steady WiFi
  // interference keeps the stream ~50-100 ms late and MAX_CORRECTION_PPM=2000
  // can only generate a 50-100 ms / 24 s ≈ 2-4 ppm correction — far too
  // slow to pull the drift back in any reasonable time.
  if (!boost_active && playing) {
    if (drift_us < -SOFT_BOOST_TRIGGER_DRIFT_US) {
      // Frame is persistently late — start or extend the trigger window.
      if (s_servo.soft_boost_late_since_us == 0) {
        s_servo.soft_boost_late_since_us = now_us;
      }
      // Arm the boost once we've been late for the trigger duration, and
      // the re-arm delay (hysteresis) has elapsed since the last exit.
      if (!s_servo.soft_boost_active &&
          (now_us - s_servo.soft_boost_late_since_us >= SOFT_BOOST_TRIGGER_DURATION_US) &&
          (s_servo.soft_boost_rearm_after_us == 0 ||
           now_us >= s_servo.soft_boost_rearm_after_us)) {
        s_servo.soft_boost_active = true;
        s_servo.soft_boost_started_us = now_us;
        s_servo.soft_boost_on_time_since_us = 0;
        ESP_LOGI(TAG, "soft boost enter: drift=%d ms", drift_us / 1000);
      }
    } else {
      // Not persistently late — reset trigger window.
      s_servo.soft_boost_late_since_us = 0;
    }

    if (s_servo.soft_boost_active) {
      bool timed_out =
          (now_us - s_servo.soft_boost_started_us) >= SOFT_BOOST_TIMEOUT_US;
      bool on_time = (drift_us >= -SOFT_BOOST_EXIT_DRIFT_US &&
                      drift_us <= SOFT_BOOST_EXIT_DRIFT_US);

      if (on_time) {
        if (s_servo.soft_boost_on_time_since_us == 0) {
          s_servo.soft_boost_on_time_since_us = now_us;
        }
      } else {
        s_servo.soft_boost_on_time_since_us = 0;
      }

      bool exit_on_time =
          s_servo.soft_boost_on_time_since_us != 0 &&
          (now_us - s_servo.soft_boost_on_time_since_us) >= SOFT_BOOST_EXIT_DURATION_US;

      if (exit_on_time || timed_out) {
        ESP_LOGI(TAG, "soft boost exit: drift=%d ms %s", drift_us / 1000,
                 timed_out ? "(timeout)" : "(converged)");
        s_servo.soft_boost_active = false;
        s_servo.soft_boost_started_us = 0;
        s_servo.soft_boost_on_time_since_us = 0;
        s_servo.soft_boost_late_since_us = 0;
        s_servo.soft_boost_rearm_after_us = now_us + SOFT_BOOST_REARM_DELAY_US;
      }
    }
  } else if (boost_active) {
    // Seek boost takes precedence — reset soft-boost state so it starts
    // fresh after the seek boost exits.
    s_servo.soft_boost_active = false;
    s_servo.soft_boost_late_since_us = 0;
    s_servo.soft_boost_on_time_since_us = 0;
  }

  int32_t ppm_limit = boost_active        ? SEEK_BOOST_MAX_CORRECTION_PPM
                      : s_servo.soft_boost_active ? SOFT_BOOST_MAX_CORRECTION_PPM
                                                  : MAX_CORRECTION_PPM;
  int32_t time_constant = boost_active ? SEEK_BOOST_TIME_CONSTANT_SEC
                                       : TIME_CONSTANT_SEC;

  if (!playing) {
    s_servo.target_ppm = 0;
  } else if (drift_us > -DRIFT_DEADBAND_US && drift_us < DRIFT_DEADBAND_US) {
    s_servo.target_ppm = 0;
  } else {
    // Positive drift -> we are early -> slow down (negative correction).
    // Negative drift -> we are late  -> speed up (positive correction).
    int32_t target = -drift_us / time_constant;
    s_servo.target_ppm = clamp_ppm_to(target, ppm_limit);
  }

  // Smooth applied correction toward target.  Integer math keeps state
  // deterministic and bit-exact across boots.
  int32_t delta = s_servo.target_ppm - s_servo.applied_ppm;
  int32_t smoothing_denominator = boost_active ? BOOST_SMOOTHING_DENOMINATOR
                                               : SMOOTHING_DENOMINATOR;
  int32_t step = (delta * SMOOTHING_NUMERATOR) / smoothing_denominator;
  // Ensure progress: never round to zero when delta is non-zero, otherwise
  // the integer divide stalls forever near the target.
  if (step == 0 && delta != 0) {
    step = (delta > 0) ? 1 : -1;
  }
  int32_t applied_limit = ppm_limit;
  int32_t applied_abs = abs_i32(s_servo.applied_ppm);
  if (applied_abs > applied_limit) {
    applied_limit = applied_abs;
  }
  s_servo.applied_ppm = clamp_ppm_to(s_servo.applied_ppm + step,
                                     applied_limit);

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
