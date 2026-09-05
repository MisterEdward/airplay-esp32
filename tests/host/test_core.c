// Host unit tests for the pure audio/log modules.  Build and run with
// tests/host/run.sh — no ESP-IDF needed.
#include "audio_align.h"
#include "audio_envelope.h"
#include "log_journal.h"
#include "source_volume.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_shape(void) {
  assert(audio_envelope_shape_q15(0) == 0);
  assert(audio_envelope_shape_q15(32768) == 32768);
  assert(audio_envelope_shape_q15(-5) == 0);
  assert(audio_envelope_shape_q15(40000) == 32768);
  int32_t mid = audio_envelope_shape_q15(16384);
  assert(mid > 16000 && mid < 16800); // smoothstep(0.5) = 0.5
  // Monotonic, and flat at both ends (first step tiny, last step tiny).
  int32_t prev = 0;
  for (int32_t x = 0; x <= 32768; x += 64) {
    int32_t y = audio_envelope_shape_q15(x);
    assert(y >= prev);
    prev = y;
  }
  assert(audio_envelope_shape_q15(64) < 4);
  assert(32768 - audio_envelope_shape_q15(32768 - 64) < 4);
}

static void fill(int16_t *pcm, size_t frames, int16_t v) {
  for (size_t i = 0; i < frames; i++) {
    pcm[2 * i] = v;
    pcm[2 * i + 1] = (int16_t)-v;
  }
}

static void test_envelope_fade_in(void) {
  audio_envelope_t e;
  audio_envelope_init(&e, 48000, 150, 100);
  assert(e.fade_in_frames == 7200);
  assert(e.fade_out_frames == 4800);
  assert(audio_envelope_is_silent(&e));

  int16_t pcm[2 * 480];
  // Silent envelope zeroes the block and reports inaudible.
  fill(pcm, 480, 32767);
  assert(!audio_envelope_apply(&e, pcm, 480, 32768));
  assert(pcm[0] == 0 && pcm[959] == 0);

  audio_envelope_fade_in(&e);
  assert(audio_envelope_state(&e) == ENVELOPE_FADING_IN);
  int16_t last = 0;
  size_t blocks = 0;
  while (audio_envelope_state(&e) == ENVELOPE_FADING_IN) {
    fill(pcm, 480, 32767);
    audio_envelope_apply(&e, pcm, 480, 32768);
    for (size_t i = 0; i < 480; i++) {
      assert(pcm[2 * i] >= last);            // monotonic ramp
      assert(pcm[2 * i] == -pcm[2 * i + 1]); // L/R share the gain
      last = pcm[2 * i];
    }
    blocks++;
  }
  // 7200 frames / 480 = 15 blocks of ramp (the last one lands on OPEN).
  assert(blocks == 15 || blocks == 16);
  assert(audio_envelope_state(&e) == ENVELOPE_OPEN);
  fill(pcm, 480, 32767);
  assert(audio_envelope_apply(&e, pcm, 480, 32768));
  assert(pcm[0] == 32767);
}

static void test_envelope_fade_out_and_reverse(void) {
  audio_envelope_t e;
  audio_envelope_init(&e, 48000, 150, 100);
  int16_t pcm[2 * 480];
  audio_envelope_fade_in(&e);
  // Run ~half the fade-in, then request a fade-out: the level must reverse
  // from where it is, never jump.
  for (int b = 0; b < 7; b++) {
    fill(pcm, 480, 32767);
    audio_envelope_apply(&e, pcm, 480, 32768);
  }
  int32_t level_before = e.level_q23;
  assert(level_before > (10000 << 8) && level_before < (25000 << 8));
  audio_envelope_fade_out(&e);
  assert(audio_envelope_state(&e) == ENVELOPE_FADING_OUT);
  fill(pcm, 480, 32767);
  audio_envelope_apply(&e, pcm, 480, 32768);
  assert(e.level_q23 < level_before);
  int16_t prev = pcm[0];
  for (size_t i = 1; i < 480; i++) {
    assert(pcm[2 * i] <= prev);
    prev = pcm[2 * i];
  }
  while (audio_envelope_state(&e) == ENVELOPE_FADING_OUT) {
    fill(pcm, 480, 32767);
    audio_envelope_apply(&e, pcm, 480, 32768);
  }
  assert(audio_envelope_is_silent(&e));
  assert(pcm[959] == 0);
  // Fading in again starts from silence and completes.
  audio_envelope_fade_in(&e);
  int blocks = 0;
  while (audio_envelope_state(&e) == ENVELOPE_FADING_IN && blocks < 100) {
    fill(pcm, 480, 32767);
    audio_envelope_apply(&e, pcm, 480, 32768);
    blocks++;
  }
  assert(audio_envelope_state(&e) == ENVELOPE_OPEN);
}

static void test_envelope_volume_slew(void) {
  audio_envelope_t e;
  audio_envelope_init(&e, 48000, 1, 1);
  audio_envelope_fade_in(&e);
  int16_t pcm[2 * 480];
  // Complete the (2 frame) fade-in first.
  fill(pcm, 480, 16384);
  audio_envelope_apply(&e, pcm, 480, 32768);
  assert(audio_envelope_state(&e) == ENVELOPE_OPEN);
  // First call snapped the volume to target: full level at the end.
  assert(pcm[958] == 16384);
  // Now drop target to half: the block must ramp, not step.
  fill(pcm, 480, 16384);
  audio_envelope_apply(&e, pcm, 480, 16384);
  assert(pcm[0] > 8192 && pcm[0] <= 16384);
  assert(pcm[958] >= 8192 && pcm[958] < 8600); // ~4 time constants in
  int16_t prev = pcm[0];
  for (size_t i = 1; i < 480; i++) {
    assert(pcm[2 * i] <= prev);
    assert(prev - pcm[2 * i] <= 80); // no single step larger than ~0.5%
    prev = pcm[2 * i];
  }
  // Split blocks must produce identical results to one big block.
  audio_envelope_t a, b;
  audio_envelope_init(&a, 44100, 20, 20);
  audio_envelope_fade_in(&a);
  b = a;
  int16_t p[2 * 600], q[2 * 600];
  fill(p, 600, -12345);
  fill(q, 600, -12345);
  audio_envelope_apply(&a, p, 600, 20000);
  audio_envelope_apply(&b, q, 123, 20000);
  audio_envelope_apply(&b, q + 246, 477, 20000);
  assert(memcmp(p, q, sizeof(p)) == 0);
}

static void test_align(void) {
  assert(audio_align_silence_frames(0, 44100, 352) == 0);
  assert(audio_align_silence_frames(-100, 44100, 352) == 0);
  assert(audio_align_silence_frames(1, 44100, 352) == 1);
  assert(audio_align_silence_frames(1000, 44100, 352) == 45);
  assert(audio_align_silence_frames(16000, 44100, 352) == 352);
  assert(audio_align_silence_frames(INT64_MAX / 4, 48000, 352) == 352);
  assert(audio_align_silence_frames(1000, 0, 352) == 0);
  assert(audio_align_silence_frames(1000, 44100, 0) == 0);
  for (int64_t us = 1; us < 7000; us++) {
    size_t n = audio_align_silence_frames(us, 48000, 352);
    assert(n * 1000000ULL >= (uint64_t)us * 48000); // never too short
    assert((n - 1) * 1000000ULL < (uint64_t)us * 48000);
  }
  assert(audio_align_trim_frames(1000, 44100, 352) == 44);
  assert(audio_align_trim_frames(20, 44100, 352) == 0);
  assert(audio_align_trim_frames(100000, 44100, 352) == 352);
  assert(audio_align_frames_to_us(44100, 44100) == 1000000);
  assert(audio_align_frames_to_us(352, 44100) == 7981);
}

static void test_journal(void) {
  char storage[8];
  char a[16] = {0}, b[16] = {0};
  log_journal_t j;
  log_journal_init(&j, storage, sizeof(storage), 42);
  uint64_t c1 = 0, c2 = 0, missed = 99;
  assert(log_journal_read(&j, &c1, a, 8, &missed) == 0 && missed == 0);
  log_journal_append(&j, "abcde", 5);
  assert(log_journal_used(&j) == 5);
  assert(log_journal_read(&j, &c1, a, 2, &missed) == 2);
  assert(missed == 0 && memcmp(a, "ab", 2) == 0);
  assert(log_journal_read(&j, &c2, b, 8, &missed) == 5);
  assert(memcmp(b, "abcde", 5) == 0);
  log_journal_append(&j, "fghijkl", 7);
  assert(log_journal_used(&j) == 8);
  assert(log_journal_oldest(&j) == 4);
  assert(log_journal_read(&j, &c1, a, 8, &missed) == 8);
  assert(missed == 2 && memcmp(a, "efghijkl", 8) == 0);
  assert(log_journal_read(&j, &c2, b, 8, &missed) == 7);
  assert(missed == 0 && memcmp(b, "fghijkl", 7) == 0);
  // Oversized append keeps only the tail.
  log_journal_append(&j, "0123456789ABCDEF", 16);
  c1 = 0;
  assert(log_journal_read(&j, &c1, a, 8, &missed) == 8);
  assert(memcmp(a, "89ABCDEF", 8) == 0);
  // A cursor from the future (previous boot) recovers.
  c1 = UINT64_MAX;
  assert(log_journal_read(&j, &c1, a, 8, &missed) == 8);
  assert(memcmp(a, "89ABCDEF", 8) == 0);
  assert(c1 == j.end);
}

static void test_source_volume(void) {
  source_volume_table_t t;
  source_volume_table_init(&t);
  assert(source_volume_table_validate(&t));
  float db = 0;
  assert(!source_volume_table_get(&t, "phone", &db));
  assert(source_volume_table_set(&t, "phone", -22.0f));
  assert(source_volume_table_set(&t, "mac", -8.25f));
  assert(!source_volume_table_set(&t, "mac", -8.25f)); // unchanged
  assert(source_volume_table_get(&t, "phone", &db) && db == -22.0f);
  assert(source_volume_table_get(&t, "mac", &db) && db == -8.25f);
  assert(!source_volume_table_set(&t, "mac", NAN));
  assert(source_volume_table_set(&t, "mac", 5.0f)); // clamped to 0
  assert(source_volume_table_get(&t, "mac", &db) && db == 0.0f);
  assert(source_volume_table_set(&t, "mac", -40.0f)); // clamped to -30
  assert(source_volume_table_get(&t, "mac", &db) && db == -30.0f);
  // Fill the table and check LRU eviction keeps recently used ids.
  char id[16];
  for (int i = 0; i < SOURCE_VOLUME_SLOTS - 2; i++) {
    snprintf(id, sizeof(id), "dev%d", i);
    source_volume_table_set(&t, id, -10.0f);
  }
  assert(source_volume_table_get(&t, "phone", &db)); // touched: survives
  source_volume_table_set(&t, "newcomer", -5.0f);
  assert(source_volume_table_get(&t, "phone", &db) && db == -22.0f);
  assert(!source_volume_table_get(&t, "mac", &db)); // LRU victim
  // Corrupt blob is rejected and reset.
  t.entries[0].centi_db = 12345;
  assert(!source_volume_table_validate(&t));
  assert(!source_volume_table_get(&t, "phone", &db));
  // Identity normalisation.
  char out[SOURCE_VOLUME_ID_SIZE];
  assert(source_volume_normalize_id("58:55:CA:1A:E2:88", out, sizeof(out)));
  assert(strcmp(out, "58:55:ca:1a:e2:88") == 0);
  assert(!source_volume_normalize_id("bad id\n", out, sizeof(out)));
  assert(!source_volume_normalize_id("", out, sizeof(out)));
  char longid[80];
  memset(longid, 'a', sizeof(longid) - 1);
  longid[sizeof(longid) - 1] = '\0';
  assert(!source_volume_normalize_id(longid, out, sizeof(out)));
}

int main(void) {
  test_shape();
  test_envelope_fade_in();
  test_envelope_fade_out_and_reverse();
  test_envelope_volume_slew();
  test_align();
  test_journal();
  test_source_volume();
  puts("host core tests passed");
  return 0;
}
