#pragma once
// The libsodium subset srp.c uses, backed by mbedtls SHA-512.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mbedtls/sha512.h"

typedef mbedtls_sha512_context crypto_hash_sha512_state;

static inline int crypto_hash_sha512_init(crypto_hash_sha512_state *s) {
  mbedtls_sha512_init(s);
  return mbedtls_sha512_starts(s, 0);
}
static inline int crypto_hash_sha512_update(crypto_hash_sha512_state *s,
                                            const uint8_t *in, size_t len) {
  return mbedtls_sha512_update(s, in, len);
}
static inline int crypto_hash_sha512_final(crypto_hash_sha512_state *s,
                                           uint8_t *out) {
  int ret = mbedtls_sha512_finish(s, out);
  mbedtls_sha512_free(s);
  return ret;
}
static inline int crypto_hash_sha512(uint8_t *out, const uint8_t *in,
                                     size_t len) {
  return mbedtls_sha512(in, len, out, 0);
}
static inline void sodium_memzero(void *p, size_t len) {
  volatile uint8_t *v = (volatile uint8_t *)p;
  while (len--) {
    *v++ = 0;
  }
}
static inline int sodium_memcmp(const void *a, const void *b, size_t len) {
  return memcmp(a, b, len) == 0 ? 0 : -1;
}
