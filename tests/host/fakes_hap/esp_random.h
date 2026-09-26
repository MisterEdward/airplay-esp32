#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// Good enough for a test: SRP only needs the secrets to differ per session.
static inline void esp_fill_random(void *buf, size_t len) {
  uint8_t *p = (uint8_t *)buf;
  for (size_t i = 0; i < len; i++) {
    p[i] = (uint8_t)(rand() >> 7);
  }
}
