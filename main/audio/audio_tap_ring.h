#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * Single-producer / single-consumer ring of interleaved stereo int16 frames,
 * lock-free, for handing audio from one task to another without either side
 * ever blocking the other.  Used by the USB capture tap: the render task
 * (producer, real-time) copies every block it gives the DAC in, the USB IN
 * pump (consumer) drains it at the host's pace.
 *
 *  - `head` is written only by the producer, `tail` only by the consumer.
 *    Both are free-running frame counters; `head - tail` is the fill level
 *    and wraps correctly because the capacity is a power of two.
 *  - The producer publishes with a release store after the copy, the
 *    consumer reads it with an acquire load before the copy (and the other
 *    way round for `tail`), so the data is visible before the index that
 *    covers it, on either core.
 *  - A full ring never overwrites: the producer writes what fits and
 *    reports the rest as not written, so the consumer never sees a torn
 *    block.
 *
 * Header-only so the host tests and the firmware share one definition.
 */

typedef struct {
  int16_t *buf;        // 2 * cap_frames samples
  uint32_t cap_frames; // power of two
  uint32_t head;       // producer: frames written since init
  uint32_t tail;       // consumer: frames read since init
} audio_tap_ring_t;

static inline bool audio_tap_ring_init(audio_tap_ring_t *r, int16_t *buf,
                                       uint32_t cap_frames) {
  if (!r || !buf || cap_frames == 0 || (cap_frames & (cap_frames - 1)) != 0) {
    return false;
  }
  r->buf = buf;
  r->cap_frames = cap_frames;
  r->head = 0;
  r->tail = 0;
  return true;
}

/** Frames waiting (either side may call it; the answer may be stale). */
static inline uint32_t audio_tap_ring_level(const audio_tap_ring_t *r) {
  uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
  uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
  return head - tail;
}

/** Producer: copy up to `frames`; returns how many fitted. */
static inline size_t audio_tap_ring_write(audio_tap_ring_t *r,
                                          const int16_t *pcm, size_t frames) {
  uint32_t head = r->head; // own index
  uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
  uint32_t space = r->cap_frames - (head - tail);
  size_t n = frames < space ? frames : space;
  if (n == 0) {
    return 0;
  }
  uint32_t mask = r->cap_frames - 1;
  uint32_t pos = head & mask;
  size_t first = r->cap_frames - pos;
  if (first > n) {
    first = n;
  }
  memcpy(&r->buf[2 * pos], pcm, first * 2 * sizeof(int16_t));
  if (n > first) {
    memcpy(&r->buf[0], &pcm[2 * first], (n - first) * 2 * sizeof(int16_t));
  }
  __atomic_store_n(&r->head, head + (uint32_t)n, __ATOMIC_RELEASE);
  return n;
}

/** Consumer: copy up to `max_frames` out; returns how many. */
static inline size_t audio_tap_ring_read(audio_tap_ring_t *r, int16_t *out,
                                         size_t max_frames) {
  uint32_t tail = r->tail; // own index
  uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
  uint32_t avail = head - tail;
  size_t n = max_frames < avail ? max_frames : avail;
  if (n == 0) {
    return 0;
  }
  uint32_t mask = r->cap_frames - 1;
  uint32_t pos = tail & mask;
  size_t first = r->cap_frames - pos;
  if (first > n) {
    first = n;
  }
  memcpy(out, &r->buf[2 * pos], first * 2 * sizeof(int16_t));
  if (n > first) {
    memcpy(&out[2 * first], &r->buf[0], (n - first) * 2 * sizeof(int16_t));
  }
  __atomic_store_n(&r->tail, tail + (uint32_t)n, __ATOMIC_RELEASE);
  return n;
}

/** Consumer: drop everything currently queued; returns how many frames. */
static inline uint32_t audio_tap_ring_discard(audio_tap_ring_t *r) {
  uint32_t tail = r->tail;
  uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
  __atomic_store_n(&r->tail, head, __ATOMIC_RELEASE);
  return head - tail;
}
