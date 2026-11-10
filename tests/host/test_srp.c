// Host test for main/hap/srp.c: a known-answer pair-setup against an
// independent Python SRP-6a (srp_kat.h), and the Barrett reduction against
// mbedtls long division.  Uses the software mbedtls bignum from the IDF tree;
// tests/host/run.sh skips it when that tree is not installed.
#include "srp.h"

#include "esp_random.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "srp_kat.h"

static uint8_t q_buf[256];
static size_t q_len;
static size_t q_pos;
static uint32_t prng = 12345;

void fake_random_queue(const uint8_t *bytes, size_t len) {
  assert(q_len + len <= sizeof(q_buf));
  memcpy(q_buf + q_len, bytes, len);
  q_len += len;
}

void esp_fill_random(void *buf, size_t len) {
  uint8_t *out = buf;
  for (size_t i = 0; i < len; i++) {
    if (q_pos < q_len) {
      out[i] = q_buf[q_pos++];
    } else {
      prng = prng * 1103515245u + 12345u;
      out[i] = (uint8_t)(prng >> 16);
    }
  }
  if (q_pos == q_len) {
    q_pos = q_len = 0;
  }
}

static int rng(void *ctx, unsigned char *out, size_t len) {
  (void)ctx;
  esp_fill_random(out, len);
  return 0;
}

static srp_session_t *start_kat(void) {
  srp_session_t *s = srp_session_create();
  assert(s);
  fake_random_queue(kat_salt, sizeof(kat_salt));
  fake_random_queue(kat_b, sizeof(kat_b));
  assert(srp_start(s, "Pair-Setup", "3939") == ESP_OK);
  size_t len = 0;
  assert(memcmp(srp_get_salt(s), kat_salt, sizeof(kat_salt)) == 0);
  assert(memcmp(srp_get_public_key(s, &len), kat_B, sizeof(kat_B)) == 0);
  assert(len == sizeof(kat_B));
  return s;
}

static void test_kat(void) {
  srp_session_t *s = start_kat();
  assert(srp_verify_client(s, kat_A, sizeof(kat_A), kat_M1, sizeof(kat_M1)) ==
         ESP_OK);
  size_t klen = 0;
  assert(memcmp(srp_get_proof(s), kat_M2, sizeof(kat_M2)) == 0);
  assert(memcmp(srp_get_session_key(s, &klen), kat_K, sizeof(kat_K)) == 0);
  assert(klen == sizeof(kat_K));
  // M3 is single-use.
  assert(srp_verify_client(s, kat_A, sizeof(kat_A), kat_M1, sizeof(kat_M1)) !=
         ESP_OK);
  srp_session_free(s);

  // Copying the keys into a fresh session (what the pool does) works.
  s = start_kat();
  srp_session_t *copy = srp_session_create();
  *copy = *s;
  srp_session_free(s);
  assert(srp_verify_client(copy, kat_A, sizeof(kat_A), kat_M1,
                           sizeof(kat_M1)) == ESP_OK);
  assert(memcmp(srp_get_proof(copy), kat_M2, sizeof(kat_M2)) == 0);
  srp_session_free(copy);
}

// The encodings that only matter once in 256 pair-setups each: a salt, an A
// or a B that starts with a zero byte.  The sender hashes all 16 salt bytes
// and pads A and B to 384 bytes in M1 and M2; srp.c used the shortest form
// of all three and rejected the proof.
static void check_vector(const uint8_t *salt, const uint8_t *b,
                         const uint8_t *A, const uint8_t *B, const uint8_t *M1,
                         const uint8_t *M2, const uint8_t *K) {
  srp_session_t *s = srp_session_create();
  assert(s);
  fake_random_queue(salt, 16);
  fake_random_queue(b, 32);
  assert(srp_start(s, "Pair-Setup", "3939") == ESP_OK);
  size_t len = 0;
  assert(memcmp(srp_get_salt(s), salt, 16) == 0);
  assert(memcmp(srp_get_public_key(s, &len), B, 384) == 0);
  assert(srp_verify_client(s, A, 384, M1, 64) == ESP_OK);
  size_t klen = 0;
  assert(memcmp(srp_get_proof(s), M2, 64) == 0);
  assert(memcmp(srp_get_session_key(s, &klen), K, 64) == 0);
  srp_session_free(s);
}

static void test_kat_leading_zeros(void) {
  assert(kat0_salt[0] == 0);
  check_vector(kat0_salt, kat0_b, kat0_A, kat0_B, kat0_M1, kat0_M2, kat0_K);
  assert(katA0_A[0] == 0 && katA0_B[0] != 0);
  check_vector(katA0_salt, katA0_b, katA0_A, katA0_B, katA0_M1, katA0_M2,
               katA0_K);
  assert(katB0_B[0] == 0 && katB0_A[0] != 0);
  check_vector(katB0_salt, katB0_b, katB0_A, katB0_B, katB0_M1, katB0_M2,
               katB0_K);
}

// N is static in srp.c; recover it as x - (x mod N) with x = 2^3072 - 1
// (N > 2^3071, so x mod N = x - N).
static void get_n(mbedtls_mpi *n) {
  mbedtls_mpi x, r;
  mbedtls_mpi_init(&x);
  mbedtls_mpi_init(&r);
  assert(mbedtls_mpi_lset(&x, 1) == 0);
  assert(mbedtls_mpi_shift_l(&x, 3072) == 0);
  assert(mbedtls_mpi_sub_int(&x, &x, 1) == 0);
  assert(srp_mod_n(&r, &x) == 0);
  assert(mbedtls_mpi_sub_mpi(n, &x, &r) == 0);
  assert(mbedtls_mpi_bitlen(n) == 3072);
  mbedtls_mpi_free(&x);
  mbedtls_mpi_free(&r);
}

static void check_mod(const mbedtls_mpi *x, const mbedtls_mpi *n) {
  mbedtls_mpi got, want, alias;
  mbedtls_mpi_init(&got);
  mbedtls_mpi_init(&want);
  mbedtls_mpi_init(&alias);
  assert(srp_mod_n(&got, x) == 0);
  assert(mbedtls_mpi_mod_mpi(&want, x, n) == 0);
  assert(mbedtls_mpi_cmp_mpi(&got, &want) == 0);
  assert(mbedtls_mpi_copy(&alias, x) == 0);
  assert(srp_mod_n(&alias, &alias) == 0);
  assert(mbedtls_mpi_cmp_mpi(&alias, &want) == 0);
  mbedtls_mpi_free(&got);
  mbedtls_mpi_free(&want);
  mbedtls_mpi_free(&alias);
}

static void test_barrett(void) {
  mbedtls_mpi n, x, t;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&x);
  mbedtls_mpi_init(&t);
  get_n(&n);

  // Edges: 0, N-1, N, N+1, 2N, 3N-1, N^2-1, N^2, 2^6144-1, 2^6144.
  assert(mbedtls_mpi_lset(&x, 0) == 0);
  check_mod(&x, &n);
  for (int d = -1; d <= 1; d++) {
    assert(mbedtls_mpi_add_int(&x, &n, d) == 0);
    check_mod(&x, &n);
  }
  assert(mbedtls_mpi_mul_int(&x, &n, 2) == 0);
  check_mod(&x, &n);
  assert(mbedtls_mpi_mul_int(&x, &n, 3) == 0);
  assert(mbedtls_mpi_sub_int(&x, &x, 1) == 0);
  check_mod(&x, &n);
  assert(mbedtls_mpi_mul_mpi(&x, &n, &n) == 0);
  check_mod(&x, &n);
  assert(mbedtls_mpi_sub_int(&x, &x, 1) == 0);
  check_mod(&x, &n);
  assert(mbedtls_mpi_lset(&x, 1) == 0);
  assert(mbedtls_mpi_shift_l(&x, 6144) == 0);
  check_mod(&x, &n); // 6145 bits: long-division fallback
  assert(mbedtls_mpi_sub_int(&x, &x, 1) == 0);
  check_mod(&x, &n);
  assert(mbedtls_mpi_lset(&x, -7) == 0);
  check_mod(&x, &n); // negative: fallback

  // Random values of every size up to 6144 bits.
  for (int i = 0; i < 3000; i++) {
    size_t bits = 1 + (size_t)(i * 2654435761u) % 6144;
    assert(mbedtls_mpi_fill_random(&x, (bits + 7) / 8, rng, NULL) == 0);
    if (mbedtls_mpi_bitlen(&x) > bits) {
      assert(mbedtls_mpi_shift_r(&x, mbedtls_mpi_bitlen(&x) - bits) == 0);
    }
    check_mod(&x, &n);
    // Products of two reduced values, the shape M3 reduces.
    if (i % 10 == 0) {
      assert(mbedtls_mpi_mod_mpi(&t, &x, &n) == 0);
      assert(mbedtls_mpi_mul_mpi(&x, &t, &t) == 0);
      check_mod(&x, &n);
    }
  }
  mbedtls_mpi_free(&n);
  mbedtls_mpi_free(&x);
  mbedtls_mpi_free(&t);
}

static void test_rejects(void) {
  uint8_t bad[64];
  memcpy(bad, kat_M1, sizeof(bad));
  bad[17] ^= 1;
  srp_session_t *s = start_kat();
  assert(srp_verify_client(s, kat_A, sizeof(kat_A), bad, sizeof(bad)) !=
         ESP_OK);
  assert(srp_get_proof(s) == NULL);
  assert(srp_get_session_key(s, NULL) == NULL);
  srp_session_free(s);

  // A = 0 and A = N are refused.
  uint8_t a[384] = {0};
  s = start_kat();
  assert(srp_verify_client(s, a, sizeof(a), kat_M1, 64) != ESP_OK);
  srp_session_free(s);

  mbedtls_mpi n;
  mbedtls_mpi_init(&n);
  get_n(&n);
  assert(mbedtls_mpi_write_binary(&n, a, sizeof(a)) == 0);
  s = start_kat();
  assert(srp_verify_client(s, a, sizeof(a), kat_M1, 64) != ESP_OK);
  srp_session_free(s);
  mbedtls_mpi_free(&n);
}

int main(void) {
  assert(srp_global_init() == ESP_OK);
  test_kat();
  test_kat_leading_zeros();
  test_rejects();
  test_barrett();
  printf("test_srp: all passed\n");
  return 0;
}
