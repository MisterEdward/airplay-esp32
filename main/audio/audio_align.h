#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Sample-accurate acquisition helpers for the timing engine.
 *
 * Acquisition (the first frame after start, seek or track change) must land
 * the first audible sample on its scheduled instant.  Two cases:
 *
 *   early by E µs  → emit exactly ceil(E · rate / 1e6) frames of silence
 *                    before the frame.  Ceil, never floor: a wait that is a
 *                    fraction of a sample too long is inaudible, a wait that
 *                    is too short is a permanent positive offset.
 *   late by L µs   → drop the first floor(L · rate / 1e6) frames of the block
 *                    and play the remainder.  Floor here for the same reason
 *                    mirrored: prefer being a fraction of a sample late to
 *                    skipping audio that has not expired yet.
 *
 * Both results are clamped to `available`.  With 64-bit arithmetic the
 * multiply cannot overflow for any realistic error (< 2^40 µs).  Header-only
 * so the host tests and the firmware share one definition.
 */

static inline size_t audio_align_silence_frames(int64_t early_us,
                                                uint32_t sample_rate,
                                                size_t available) {
  if (early_us <= 0 || sample_rate == 0 || available == 0) {
    return 0;
  }
  // Saturate before multiplying: an implausible error (bad anchor) must not
  // overflow the 64-bit product.  `available` frames are at most ~1 s here,
  // so anything beyond a few seconds is "all of it" anyway.
  if (early_us > 60LL * 1000000LL) {
    return available;
  }
  uint64_t num = (uint64_t)early_us * sample_rate + 999999ULL;
  uint64_t frames = num / 1000000ULL;
  return frames > available ? available : (size_t)frames;
}

static inline size_t audio_align_trim_frames(int64_t late_us,
                                             uint32_t sample_rate,
                                             size_t available) {
  if (late_us <= 0 || sample_rate == 0 || available == 0) {
    return 0;
  }
  if (late_us > 60LL * 1000000LL) {
    return available;
  }
  uint64_t frames = ((uint64_t)late_us * sample_rate) / 1000000ULL;
  return frames > available ? available : (size_t)frames;
}

/** Duration in µs of `frames` frames at `sample_rate` (rounded down). */
static inline int64_t audio_align_frames_to_us(size_t frames,
                                               uint32_t sample_rate) {
  if (sample_rate == 0) {
    return 0;
  }
  return (int64_t)(((uint64_t)frames * 1000000ULL) / sample_rate);
}
