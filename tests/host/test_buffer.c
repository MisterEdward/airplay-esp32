// Host tests for the sorted PCM ring.  audio_buffer.c had no coverage at
// all, which is awkward for the module every frame passes through and the
// one the repair plan wants rewritten: these tests pin the behaviour the
// rewrite has to preserve.
//
// FreeRTOS is faked before including the implementation.  The host is
// single-threaded, so a critical section is a no-op and the counting
// semaphore is an integer — enough to prove that the semaphore and `count`
// never disagree, which is the property the current code maintains by hand.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define portENTER_CRITICAL(m)        ((void)(m))
#define portEXIT_CRITICAL(m)         ((void)(m))
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdTRUE                       1
#define pdFALSE                      0
#define portMAX_DELAY                0xFFFFFFFFu
#define pdMS_TO_TICKS(ms)            (ms)

typedef struct {
  int count;
  int max;
} fake_sem_t;

static fake_sem_t g_sem;

static void *xSemaphoreCreateCounting(int max, int initial) {
  g_sem.max = max;
  g_sem.count = initial;
  return &g_sem;
}
static int xSemaphoreTake(void *h, unsigned ticks) {
  (void)ticks;
  fake_sem_t *s = (fake_sem_t *)h;
  if (!s || s->count <= 0) {
    return pdFALSE;
  }
  s->count--;
  return pdTRUE;
}
static int xSemaphoreTakeFromISR(void *h, void *woken) {
  (void)woken;
  return xSemaphoreTake(h, 0);
}
static int xSemaphoreGive(void *h) {
  fake_sem_t *s = (fake_sem_t *)h;
  if (!s || s->count >= s->max) {
    return pdFALSE;
  }
  s->count++;
  return pdTRUE;
}
static void vSemaphoreDelete(void *h) {
  (void)h;
}

/* Quiet unless something goes wrong; the tests assert, they do not read. */
void test_log(const char *tag, const char *format, ...) {
  (void)tag;
  (void)format;
}

/* The pool normally lives in PSRAM; plain malloc is the host equivalent. */
void *heap_caps_malloc(size_t size, unsigned caps) {
  (void)caps;
  return malloc(size);
}
void heap_caps_free(void *ptr) {
  free(ptr);
}

#include "../../main/audio/audio_buffer.c"

/* ------------------------------------------------------------------ */

static int16_t g_pcm[AAC_FRAMES_PER_PACKET * 2];

static void put(audio_buffer_t *b, uint32_t ts) {
  /* Stamp the payload so a frame can be identified after it comes back. */
  for (size_t i = 0; i < AAC_FRAMES_PER_PACKET * 2; i++) {
    g_pcm[i] = (int16_t)(ts + i);
  }
  bool ok =
      audio_buffer_queue_decoded(b, NULL, ts, g_pcm, AAC_FRAMES_PER_PACKET, 2);
  assert(ok);
}

static uint32_t take_ts(audio_buffer_t *b) {
  void *item = NULL;
  size_t size = 0;
  bool ok = audio_buffer_take(b, &item, &size, 0);
  assert(ok);
  uint32_t ts = ((audio_frame_header_t *)item)->rtp_timestamp;
  audio_buffer_return(b, item);
  return ts;
}

static bool take_fails(audio_buffer_t *b) {
  void *item = NULL;
  size_t size = 0;
  return !audio_buffer_take(b, &item, &size, 0);
}

/* The semaphore is a second copy of `count`, kept in step by hand.  Every
 * test asserts they agree: a rewrite that replaces the semaphore with a task
 * notification has to keep exactly this invariant. */
static void assert_consistent(audio_buffer_t *b) {
  assert(g_sem.count == b->count);
  assert(b->count + b->free_top == b->capacity);
}

static void test_order(void) {
  audio_buffer_t b;
  assert(audio_buffer_init(&b) == ESP_OK);

  /* Deliberately out of order, including a duplicate-free spread. */
  const uint32_t in[] = {5000, 1000, 9000, 3000, 7000};
  for (size_t i = 0; i < 5; i++) {
    put(&b, in[i]);
  }
  assert(audio_buffer_get_frame_count(&b) == 5);
  assert_consistent(&b);

  uint32_t oldest = 0, newest = 0;
  assert(audio_buffer_oldest_timestamp(&b, &oldest) && oldest == 1000);
  assert(audio_buffer_peek_newest_rtp(&b, &newest) && newest == 9000);

  uint32_t prev = 0;
  for (int i = 0; i < 5; i++) {
    uint32_t ts = take_ts(&b);
    assert(ts > prev);
    prev = ts;
  }
  assert(take_fails(&b));
  assert_consistent(&b);
  audio_buffer_deinit(&b);
}

static void test_wraparound(void) {
  audio_buffer_t b;
  assert(audio_buffer_init(&b) == ESP_OK);

  /* Straddle the 32-bit rollover: 0xFFFFFF00 must come out before 0x100. */
  put(&b, 0x00000100u);
  put(&b, 0xFFFFFF00u);
  put(&b, 0xFFFFFFF0u);
  assert(take_ts(&b) == 0xFFFFFF00u);
  assert(take_ts(&b) == 0xFFFFFFF0u);
  assert(take_ts(&b) == 0x00000100u);
  assert_consistent(&b);
  audio_buffer_deinit(&b);
}

static void test_overflow_drops_oldest(void) {
  audio_buffer_t b;
  assert(audio_buffer_init(&b) == ESP_OK);

  const int over = 50;
  for (int i = 0; i < b.capacity + over; i++) {
    put(&b, (uint32_t)(1000 + i * 100));
  }
  /* Capacity is a hard ceiling and the oldest frames are the ones that go. */
  assert(b.count == b.capacity);
  assert_consistent(&b);

  uint32_t oldest = 0;
  assert(audio_buffer_oldest_timestamp(&b, &oldest));
  assert(oldest == (uint32_t)(1000 + over * 100));

  /* Everything that survived comes out in order, starting at that oldest. */
  uint32_t prev = 0;
  for (int i = 0; i < b.capacity; i++) {
    uint32_t ts = take_ts(&b);
    assert(ts >= oldest);
    assert(i == 0 || ts > prev);
    prev = ts;
  }
  assert(take_fails(&b));
  assert_consistent(&b);
  audio_buffer_deinit(&b);
}

static void test_flush(void) {
  audio_buffer_t b;
  assert(audio_buffer_init(&b) == ESP_OK);

  for (int i = 0; i < 40; i++) {
    put(&b, (uint32_t)(500 + i));
  }
  audio_buffer_flush(&b);
  assert(audio_buffer_get_frame_count(&b) == 0);
  /* The drained semaphore is what stops the consumer from being handed a
   * slot that flush already returned to the free stack. */
  assert(take_fails(&b));
  assert_consistent(&b);

  /* Reusable afterwards. */
  put(&b, 12345);
  assert(take_ts(&b) == 12345);
  assert_consistent(&b);
  audio_buffer_deinit(&b);
}

static void test_no_slot_leak(void) {
  audio_buffer_t b;
  assert(audio_buffer_init(&b) == ESP_OK);

  /* Many put/take cycles must not lose slots to the free stack. */
  for (int round = 0; round < 200; round++) {
    put(&b, (uint32_t)(round * 352));
    assert(take_ts(&b) == (uint32_t)(round * 352));
  }
  assert(b.free_top == b.capacity);
  assert_consistent(&b);
  audio_buffer_deinit(&b);
}

static void test_bulk_start(void) {
  audio_buffer_t b;
  assert(audio_buffer_init(&b) == ESP_OK);

  /* A stale island far below, then a contiguous run at the head.  Playout
   * start must be told to begin at the run, not at the island. */
  put(&b, 1000);
  const uint32_t base = 500000;
  for (int i = 0; i < 5; i++) {
    put(&b, base + (uint32_t)i * AAC_FRAMES_PER_PACKET);
  }
  uint32_t start = 0;
  assert(audio_buffer_bulk_start_rtp(&b, &start));
  assert(start == base);
  assert_consistent(&b);
  audio_buffer_deinit(&b);
}

static void test_nearly_full(void) {
  audio_buffer_t b;
  assert(audio_buffer_init(&b) == ESP_OK);
  assert(!audio_buffer_is_nearly_full(&b));
  for (int i = 0; i < b.capacity * 9 / 10 + 1; i++) {
    put(&b, (uint32_t)(i * 352));
  }
  assert(audio_buffer_is_nearly_full(&b));
  assert_consistent(&b);
  audio_buffer_deinit(&b);
}

/* Randomised: arbitrary arrival order, interleaved consumption, capacity
 * pressure.  The ring shifts whichever side is shorter, so both the
 * head-moves-back and the tail-moves-forward branches need exercising with
 * positions the hand-written cases never reach. */
static void test_stress_random_order(void) {
  audio_buffer_t b;
  assert(audio_buffer_init(&b) == ESP_OK);

  unsigned seed = 12345;
  int live = 0;

  for (int step = 0; step < 60000; step++) {
    seed = seed * 1103515245u + 12345u;
    unsigned r = (seed >> 16) & 0x7FFF;

    if (r % 3 != 0 || live == 0) {
      /* Timestamps scattered over a window wider than the buffer holds, so
       * inserts land at every position rather than always appending. */
      uint32_t ts = 1000000u + ((seed >> 8) % 20000u) * 352u;
      for (size_t i = 0; i < AAC_FRAMES_PER_PACKET * 2; i++) {
        g_pcm[i] = (int16_t)ts;
      }
      if (audio_buffer_queue_decoded(&b, NULL, ts, g_pcm, AAC_FRAMES_PER_PACKET,
                                     2)) {
        live = b.count;
      }
    } else {
      /* Whatever take() hands over must be exactly the frame the buffer
       * just reported as its oldest.  Global monotonicity across takes is
       * deliberately not asserted: a late packet may legitimately arrive
       * below the playhead, and the real stream does that too. */
      uint32_t expect = 0;
      bool had = audio_buffer_oldest_timestamp(&b, &expect);
      void *item = NULL;
      size_t size = 0;
      if (audio_buffer_take(&b, &item, &size, 0)) {
        assert(had);
        uint32_t ts = ((audio_frame_header_t *)item)->rtp_timestamp;
        assert(ts == expect);
        audio_buffer_return(&b, item);
        live = b.count;
      } else {
        assert(!had);
      }
    }

    /* The invariants that must never break, checked on every single step. */
    assert(b.count >= 0 && b.count <= b.capacity);
    assert(b.head >= 0 && b.head < b.capacity);
    assert_consistent(&b);

    /* And the ring is sorted, always. */
    if ((step % 500) == 0) {
      for (int i = 1; i < b.count; i++) {
        assert((int32_t)(slot_timestamp(&b, ring_get(&b, i)) -
                         slot_timestamp(&b, ring_get(&b, i - 1))) >= 0);
      }
    }
  }
  audio_buffer_deinit(&b);
}

int main(void) {
  test_order();
  test_wraparound();
  test_overflow_drops_oldest();
  test_flush();
  test_no_slot_leak();
  test_bulk_start();
  test_nearly_full();
  test_stress_random_order();
  printf("buffer tests passed\n");
  return 0;
}
