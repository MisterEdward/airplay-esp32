#include "audio_resample.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned calls;
static unsigned fail_call;

void test_log(const char *tag, const char *format, ...) {
  (void)tag;
  (void)format;
}

void *heap_caps_realloc(void *ptr, size_t size, unsigned caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (++calls == fail_call) {
    return NULL;
  }
  return realloc(ptr, size);
}

int main(void) {
  int16_t in[1025 * 2] = {0};
  int16_t out[1133 * 2];
  // Allocation failure at either conversion buffer must clean up safely.
  for (unsigned fail = 1; fail <= 2; fail++) {
    calls = 0;
    fail_call = fail;
    assert(!audio_resample_init(44100, 48000, 2));
    assert(!audio_resample_is_active());
    audio_resample_destroy();
  }
  // Growth can fail on either buffer, including after the other grew.
  for (unsigned fail = 1; fail <= 2; fail++) {
    fail_call = 0;
    assert(audio_resample_init(44100, 48000, 2));
    for (size_t i = 0; i < sizeof(out) / sizeof(out[0]); i++) {
      out[i] = 12345;
    }
    fail_call = calls + fail;
    assert(audio_resample_process(in, 1025, out, 1133) == 0);
    for (size_t i = 0; i < sizeof(out) / sizeof(out[0]); i++) {
      assert(out[i] == 12345);
    }
    fail_call = 0;
    size_t n = audio_resample_process(in, 1025, out, 1133);
    assert(n > 0 && n <= 1133);
    for (size_t i = 0; i < n * 2; i++) {
      assert(out[i] == 0);
    }
    audio_resample_destroy();
  }
  puts("resampler allocation tests passed");
}
