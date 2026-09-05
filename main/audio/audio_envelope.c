#include "audio_envelope.h"

#include <string.h>

#define Q15_ONE 32768
// Fade position is kept in Q23 so that a 150 ms fade at 48 kHz (7200 frames)
// gets a step of 1165, not a truncated 4 (which stretched the fade by 14%).
#define Q23_ONE (32768 << 8)

// Volume slew divisor: each stereo frame moves the current gain by
// (target - current) / VOLUME_SLEW_DIV, minimum one LSB.  At 48 kHz this is a
// ~2.7 ms time constant — fast enough that a slider drag feels immediate,
// slow enough that the per-frame gain step is ~0.4% worst case, which is far
// below the audibility threshold for a gain discontinuity.
#define VOLUME_SLEW_DIV 128

static uint32_t ms_to_frames(uint32_t sample_rate, uint32_t ms) {
  uint64_t frames = ((uint64_t)sample_rate * ms) / 1000ULL;
  if (frames < 2) {
    frames = 2;
  }
  if (frames > 0x7FFFFFFF) {
    frames = 0x7FFFFFFF;
  }
  return (uint32_t)frames;
}

int32_t audio_envelope_shape_q15(int32_t linear_q15) {
  int32_t x = linear_q15;
  if (x <= 0) {
    return 0;
  }
  if (x >= Q15_ONE) {
    return Q15_ONE;
  }
  // 3x² - 2x³ in Q15, evaluated as x·x·(3 - 2x) with ONE final truncation.
  // A single floor of a strictly increasing function is non-decreasing;
  // truncating twice (x² first) is not, and produced 1-LSB reversals that
  // the monotonicity test caught.
  int64_t three_minus_2x = 3 * (int64_t)Q15_ONE - 2 * (int64_t)x;
  int64_t r = ((int64_t)x * x * three_minus_2x) >> 30;
  if (r < 0) {
    r = 0;
  }
  if (r > Q15_ONE) {
    r = Q15_ONE;
  }
  return (int32_t)r;
}

void audio_envelope_init(audio_envelope_t *env, uint32_t sample_rate,
                         uint32_t fade_in_ms, uint32_t fade_out_ms) {
  memset(env, 0, sizeof(*env));
  env->fade_in_frames = ms_to_frames(sample_rate, fade_in_ms);
  env->fade_out_frames = ms_to_frames(sample_rate, fade_out_ms);
  env->level_q23 = 0;
  env->step_q23 = 0;
  env->state = ENVELOPE_SILENT;
  env->volume_q15 = -1;
}

void audio_envelope_fade_in(audio_envelope_t *env) {
  if (env->state == ENVELOPE_OPEN || env->state == ENVELOPE_FADING_IN) {
    return;
  }
  env->step_q23 = (int32_t)(Q23_ONE / env->fade_in_frames);
  if (env->step_q23 < 1) {
    env->step_q23 = 1;
  }
  env->state = ENVELOPE_FADING_IN;
}

void audio_envelope_fade_out(audio_envelope_t *env) {
  if (env->state == ENVELOPE_SILENT || env->state == ENVELOPE_FADING_OUT) {
    return;
  }
  int32_t step = (int32_t)(Q23_ONE / env->fade_out_frames);
  if (step < 1) {
    step = 1;
  }
  env->step_q23 = -step;
  env->state = ENVELOPE_FADING_OUT;
}

void audio_envelope_cut(audio_envelope_t *env) {
  env->level_q23 = 0;
  env->step_q23 = 0;
  env->state = ENVELOPE_SILENT;
}

void audio_envelope_set_volume_now(audio_envelope_t *env, int32_t volume_q15) {
  if (volume_q15 < 0) {
    volume_q15 = 0;
  }
  if (volume_q15 > Q15_ONE) {
    volume_q15 = Q15_ONE;
  }
  env->volume_q15 = volume_q15;
}

bool audio_envelope_apply(audio_envelope_t *env, int16_t *pcm, size_t frames,
                          int32_t volume_target_q15) {
  if (volume_target_q15 < 0) {
    volume_target_q15 = 0;
  }
  if (volume_target_q15 > Q15_ONE) {
    volume_target_q15 = Q15_ONE;
  }
  if (env->volume_q15 < 0) {
    env->volume_q15 = volume_target_q15; // first block: no ramp from zero
  }

  // Fast path: fully closed and staying closed.
  if (env->state == ENVELOPE_SILENT) {
    memset(pcm, 0, frames * 2 * sizeof(int16_t));
    return false;
  }

  bool audible = false;
  for (size_t i = 0; i < frames; i++) {
    // Volume slew (once per stereo frame so L and R share the gain).
    if (env->volume_q15 != volume_target_q15) {
      int32_t diff = volume_target_q15 - env->volume_q15;
      int32_t step = diff / VOLUME_SLEW_DIV;
      if (step == 0) {
        step = diff > 0 ? 1 : -1;
      }
      env->volume_q15 += step;
    }

    // Fade level advance.
    if (env->step_q23 != 0) {
      env->level_q23 += env->step_q23;
      if (env->level_q23 >= Q23_ONE) {
        env->level_q23 = Q23_ONE;
        env->step_q23 = 0;
        env->state = ENVELOPE_OPEN;
      } else if (env->level_q23 <= 0) {
        env->level_q23 = 0;
        env->step_q23 = 0;
        env->state = ENVELOPE_SILENT;
      }
    }

    int32_t fade = audio_envelope_shape_q15(env->level_q23 >> 8);
    int32_t gain = (int32_t)(((int64_t)env->volume_q15 * fade) >> 15);
    if (gain == 0) {
      pcm[2 * i] = 0;
      pcm[2 * i + 1] = 0;
      continue;
    }
    // Divide rather than shift: division truncates toward zero, so a
    // symmetric waveform stays symmetric after gain (an arithmetic shift
    // rounds negatives toward -inf and adds a tiny DC offset).
    int32_t l = ((int32_t)pcm[2 * i] * gain) / Q15_ONE;
    int32_t r = ((int32_t)pcm[2 * i + 1] * gain) / Q15_ONE;
    pcm[2 * i] = (int16_t)l;
    pcm[2 * i + 1] = (int16_t)r;
    if (l != 0 || r != 0) {
      audible = true;
    }
  }
  return audible;
}

envelope_state_t audio_envelope_state(const audio_envelope_t *env) {
  return env->state;
}

bool audio_envelope_is_silent(const audio_envelope_t *env) {
  return env->state == ENVELOPE_SILENT;
}
