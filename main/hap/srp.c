#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/bignum.h"
#include "sodium.h"

#include "srp.h"

static const char *TAG = "srp";

// SRP-6a 3072-bit prime N (from RFC 5054)
static const uint8_t srp_N[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC9, 0x0F, 0xDA, 0xA2,
    0x21, 0x68, 0xC2, 0x34, 0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1,
    0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67, 0xCC, 0x74, 0x02, 0x0B, 0xBE, 0xA6,
    0x3B, 0x13, 0x9B, 0x22, 0x51, 0x4A, 0x08, 0x79, 0x8E, 0x34, 0x04, 0xDD,
    0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B, 0x30, 0x2B, 0x0A, 0x6D,
    0xF2, 0x5F, 0x14, 0x37, 0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45,
    0xE4, 0x85, 0xB5, 0x76, 0x62, 0x5E, 0x7E, 0xC6, 0xF4, 0x4C, 0x42, 0xE9,
    0xA6, 0x37, 0xED, 0x6B, 0x0B, 0xFF, 0x5C, 0xB6, 0xF4, 0x06, 0xB7, 0xED,
    0xEE, 0x38, 0x6B, 0xFB, 0x5A, 0x89, 0x9F, 0xA5, 0xAE, 0x9F, 0x24, 0x11,
    0x7C, 0x4B, 0x1F, 0xE6, 0x49, 0x28, 0x66, 0x51, 0xEC, 0xE4, 0x5B, 0x3D,
    0xC2, 0x00, 0x7C, 0xB8, 0xA1, 0x63, 0xBF, 0x05, 0x98, 0xDA, 0x48, 0x36,
    0x1C, 0x55, 0xD3, 0x9A, 0x69, 0x16, 0x3F, 0xA8, 0xFD, 0x24, 0xCF, 0x5F,
    0x83, 0x65, 0x5D, 0x23, 0xDC, 0xA3, 0xAD, 0x96, 0x1C, 0x62, 0xF3, 0x56,
    0x20, 0x85, 0x52, 0xBB, 0x9E, 0xD5, 0x29, 0x07, 0x70, 0x96, 0x96, 0x6D,
    0x67, 0x0C, 0x35, 0x4E, 0x4A, 0xBC, 0x98, 0x04, 0xF1, 0x74, 0x6C, 0x08,
    0xCA, 0x18, 0x21, 0x7C, 0x32, 0x90, 0x5E, 0x46, 0x2E, 0x36, 0xCE, 0x3B,
    0xE3, 0x9E, 0x77, 0x2C, 0x18, 0x0E, 0x86, 0x03, 0x9B, 0x27, 0x83, 0xA2,
    0xEC, 0x07, 0xA2, 0x8F, 0xB5, 0xC5, 0x5D, 0xF0, 0x6F, 0x4C, 0x52, 0xC9,
    0xDE, 0x2B, 0xCB, 0xF6, 0x95, 0x58, 0x17, 0x18, 0x39, 0x95, 0x49, 0x7C,
    0xEA, 0x95, 0x6A, 0xE5, 0x15, 0xD2, 0x26, 0x18, 0x98, 0xFA, 0x05, 0x10,
    0x15, 0x72, 0x8E, 0x5A, 0x8A, 0xAA, 0xC4, 0x2D, 0xAD, 0x33, 0x17, 0x0D,
    0x04, 0x50, 0x7A, 0x33, 0xA8, 0x55, 0x21, 0xAB, 0xDF, 0x1C, 0xBA, 0x64,
    0xEC, 0xFB, 0x85, 0x04, 0x58, 0xDB, 0xEF, 0x0A, 0x8A, 0xEA, 0x71, 0x57,
    0x5D, 0x06, 0x0C, 0x7D, 0xB3, 0x97, 0x0F, 0x85, 0xA6, 0xE1, 0xE4, 0xC7,
    0xAB, 0xF5, 0xAE, 0x8C, 0xDB, 0x09, 0x33, 0xD7, 0x1E, 0x8C, 0x94, 0xE0,
    0x4A, 0x25, 0x61, 0x9D, 0xCE, 0xE3, 0xD2, 0x26, 0x1A, 0xD2, 0xEE, 0x6B,
    0xF1, 0x2F, 0xFA, 0x06, 0xD9, 0x8A, 0x08, 0x64, 0xD8, 0x76, 0x02, 0x73,
    0x3E, 0xC8, 0x6A, 0x64, 0x52, 0x1F, 0x2B, 0x18, 0x17, 0x7B, 0x20, 0x0C,
    0xBB, 0xE1, 0x17, 0x57, 0x7A, 0x61, 0x5D, 0x6C, 0x77, 0x09, 0x88, 0xC0,
    0xBA, 0xD9, 0x46, 0xE2, 0x08, 0xE2, 0x4F, 0xA0, 0x74, 0xE5, 0xAB, 0x31,
    0x43, 0xDB, 0x5B, 0xFC, 0xE0, 0xFD, 0x10, 0x8E, 0x4B, 0x82, 0xD1, 0x20,
    0xA9, 0x3A, 0xD2, 0xCA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define SRP_GENERATOR 5

// Per-device constants: set once by srp_global_init(), read-only afterwards,
// so the RTSP client tasks and the pool task share them without a lock.
static mbedtls_mpi g_N;
static mbedtls_mpi g_k;    // k = H(N || PAD(g))
static mbedtls_mpi g_rr;   // R^2 mod N: exp_mod's Montgomery constant
static mbedtls_mpi g_mu;   // floor(2^(2 * 3072) / N), the Barrett constant
static uint8_t g_h_ng[64]; // H(N) xor H(g)
static bool g_ready;

// Helper: write MPI to buffer with minimum bytes (no leading zeros except for
// value 0)
static size_t mpi_to_bytes_min(const mbedtls_mpi *mpi, uint8_t *buf,
                               size_t len) {
  size_t mpi_size = mbedtls_mpi_size(mpi);
  if (mpi_size == 0) {
    if (len < 1) {
      return 0;
    }
    buf[0] = 0;
    return 1;
  }
  if (mpi_size > len) {
    return 0;
  }
  if (mbedtls_mpi_write_binary(mpi, buf, mpi_size) != 0) {
    return 0;
  }
  return mpi_size;
}

// Helper: write MPI to buffer, zero-padded to fixed length
static int mpi_to_bytes_padded(const mbedtls_mpi *mpi, uint8_t *buf,
                               size_t len) {
  size_t mpi_size = mbedtls_mpi_size(mpi);
  if (mpi_size > len) {
    return -1;
  }
  memset(buf, 0, len);
  return mbedtls_mpi_write_binary(mpi, buf + (len - mpi_size), mpi_size);
}

// Helper: trim leading zeros from buffer
static void trim_leading_zeros(const uint8_t *in, size_t in_len,
                               const uint8_t **out, size_t *out_len) {
  while (in_len > 1 && *in == 0) {
    in++;
    in_len--;
  }
  *out = in;
  *out_len = in_len;
}

// Compute M1 = H(H(N)^H(g) || H(I) || s || A || B || K)
static void compute_m1(uint8_t *out, const uint8_t *h_Ng_xor,
                       const uint8_t *h_I, const uint8_t *salt, size_t salt_len,
                       const uint8_t *A, size_t A_len, const uint8_t *B,
                       size_t B_len, const uint8_t *K, size_t K_len) {
  crypto_hash_sha512_state state;
  crypto_hash_sha512_init(&state);
  crypto_hash_sha512_update(&state, h_Ng_xor, 64);
  crypto_hash_sha512_update(&state, h_I, 64);
  crypto_hash_sha512_update(&state, salt, salt_len);
  crypto_hash_sha512_update(&state, A, A_len);
  crypto_hash_sha512_update(&state, B, B_len);
  crypto_hash_sha512_update(&state, K, K_len);
  crypto_hash_sha512_final(&state, out);
}

esp_err_t srp_global_init(void) {
  if (g_ready) {
    return ESP_OK;
  }
  int64_t t0 = esp_timer_get_time();
  int ret = 0;
  mbedtls_mpi g, e, t;
  mbedtls_mpi_init(&g_N);
  mbedtls_mpi_init(&g_k);
  mbedtls_mpi_init(&g_rr);
  mbedtls_mpi_init(&g_mu);
  mbedtls_mpi_init(&g);
  mbedtls_mpi_init(&e);
  mbedtls_mpi_init(&t);

  MBEDTLS_MPI_CHK(mbedtls_mpi_read_binary(&g_N, srp_N, sizeof(srp_N)));

  // k = H(N || PAD(g)) (512 bits, already < N) and H(N) xor H(g)
  {
    static const uint8_t zeros[64] = {0};
    const uint8_t g_byte = SRP_GENERATOR;
    uint8_t k_hash[64];
    crypto_hash_sha512_state state;
    crypto_hash_sha512_init(&state);
    crypto_hash_sha512_update(&state, srp_N, sizeof(srp_N));
    for (size_t left = SRP_PRIME_BYTES - 1; left > 0;) {
      size_t n = left < sizeof(zeros) ? left : sizeof(zeros);
      crypto_hash_sha512_update(&state, zeros, n);
      left -= n;
    }
    crypto_hash_sha512_update(&state, &g_byte, 1);
    crypto_hash_sha512_final(&state, k_hash);
    MBEDTLS_MPI_CHK(mbedtls_mpi_read_binary(&g_k, k_hash, sizeof(k_hash)));

    uint8_t h_g[64];
    crypto_hash_sha512(g_h_ng, srp_N, sizeof(srp_N));
    crypto_hash_sha512(h_g, &g_byte, 1);
    for (int i = 0; i < 64; i++) {
      g_h_ng[i] ^= h_g[i];
    }
  }

  // mu = floor(2^6144 / N)
  MBEDTLS_MPI_CHK(mbedtls_mpi_lset(&t, 1));
  MBEDTLS_MPI_CHK(mbedtls_mpi_shift_l(&t, 2 * SRP_PRIME_BITS));
  MBEDTLS_MPI_CHK(mbedtls_mpi_div_mpi(&g_mu, NULL, &t, &g_N));

  // The first exp_mod with an empty cache fills g_rr; later calls only read
  // it.  Every base and exponent is < N, so the accelerator word count (and
  // so R) is the same for every call.
  MBEDTLS_MPI_CHK(mbedtls_mpi_lset(&g, SRP_GENERATOR));
  MBEDTLS_MPI_CHK(mbedtls_mpi_lset(&e, 2));
  MBEDTLS_MPI_CHK(mbedtls_mpi_exp_mod(&t, &g, &e, &g_N, &g_rr));
  if (mbedtls_mpi_cmp_int(&t, SRP_GENERATOR * SRP_GENERATOR) != 0) {
    ret = MBEDTLS_ERR_MPI_BAD_INPUT_DATA;
    goto cleanup;
  }
  g_ready = true;
  ESP_LOGI(TAG, "constants ready in %lld us",
           (long long)(esp_timer_get_time() - t0));

cleanup:
  mbedtls_mpi_free(&g);
  mbedtls_mpi_free(&e);
  mbedtls_mpi_free(&t);
  if (ret != 0) {
    ESP_LOGE(TAG, "init failed: -0x%04x", (unsigned)-ret);
    mbedtls_mpi_free(&g_N);
    mbedtls_mpi_free(&g_k);
    mbedtls_mpi_free(&g_rr);
    mbedtls_mpi_free(&g_mu);
    return ESP_FAIL;
  }
  return ESP_OK;
}

// Barrett reduction (HAC 14.42 with base 2, k = 3072): with
// q = floor(floor(x / 2^(k-1)) * mu / 2^(k+1)), x - q*N lies in [0, 3N).
// Whatever q is, x - q*N is congruent to x, so the range check at the end is
// the whole correctness argument; anything unexpected falls back to the long
// division (~100 accelerator calls, ~25 ms on the S3).
int srp_mod_n(mbedtls_mpi *r, const mbedtls_mpi *x) {
  if (!g_ready || mbedtls_mpi_cmp_int(x, 0) < 0 ||
      mbedtls_mpi_bitlen(x) > 2 * SRP_PRIME_BITS) {
    return mbedtls_mpi_mod_mpi(r, x, &g_N);
  }
  if (mbedtls_mpi_cmp_mpi(x, &g_N) < 0) {
    return mbedtls_mpi_copy(r, x);
  }
  int ret = 0;
  mbedtls_mpi q, t;
  mbedtls_mpi_init(&q);
  mbedtls_mpi_init(&t);
  // No operand aliasing: the accelerated multiply splits long operands in
  // place.
  MBEDTLS_MPI_CHK(mbedtls_mpi_copy(&t, x));
  MBEDTLS_MPI_CHK(mbedtls_mpi_shift_r(&t, SRP_PRIME_BITS - 1));
  MBEDTLS_MPI_CHK(mbedtls_mpi_mul_mpi(&q, &t, &g_mu));
  MBEDTLS_MPI_CHK(mbedtls_mpi_shift_r(&q, SRP_PRIME_BITS + 1));
  MBEDTLS_MPI_CHK(mbedtls_mpi_mul_mpi(&t, &q, &g_N));
  MBEDTLS_MPI_CHK(mbedtls_mpi_sub_mpi(&q, x, &t));
  for (int i = 0; i < 3 && mbedtls_mpi_cmp_mpi(&q, &g_N) >= 0; i++) {
    MBEDTLS_MPI_CHK(mbedtls_mpi_sub_mpi(&q, &q, &g_N));
  }
  if (mbedtls_mpi_cmp_int(&q, 0) < 0 || mbedtls_mpi_cmp_mpi(&q, &g_N) >= 0) {
    ESP_LOGW(TAG, "Barrett out of range, using long division");
    ret = mbedtls_mpi_mod_mpi(r, x, &g_N);
  } else {
    ret = mbedtls_mpi_copy(r, &q);
  }

cleanup:
  mbedtls_mpi_free(&q);
  mbedtls_mpi_free(&t);
  return ret;
}

srp_session_t *srp_session_create(void) {
  srp_session_t *session = calloc(1, sizeof(srp_session_t));
  return session;
}

void srp_session_free(srp_session_t *session) {
  if (session) {
    sodium_memzero(session, sizeof(srp_session_t));
    free(session);
  }
}

esp_err_t srp_start(srp_session_t *session, const char *username,
                    const char *password) {
  if (!session || !username || !password) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!g_ready) {
    return ESP_ERR_INVALID_STATE;
  }

  mbedtls_mpi g, v, b, B, x, tmp;
  mbedtls_mpi_init(&g);
  mbedtls_mpi_init(&v);
  mbedtls_mpi_init(&b);
  mbedtls_mpi_init(&B);
  mbedtls_mpi_init(&x);
  mbedtls_mpi_init(&tmp);

  int ret = 0;
  int64_t t0 = 0;
  int64_t t1 = 0;
  int64_t t2 = 0;

  // Generate random salt
  esp_fill_random(session->salt, SRP_SALT_BYTES);

  // x = H(s || H(I || ":" || P))
  {
    uint8_t inner_hash[64];
    crypto_hash_sha512_state state;
    crypto_hash_sha512_init(&state);
    crypto_hash_sha512_update(&state, (const uint8_t *)username,
                              strlen(username));
    crypto_hash_sha512_update(&state, (const uint8_t *)":", 1);
    crypto_hash_sha512_update(&state, (const uint8_t *)password,
                              strlen(password));
    crypto_hash_sha512_final(&state, inner_hash);

    uint8_t x_hash[64];
    crypto_hash_sha512_init(&state);
    crypto_hash_sha512_update(&state, session->salt, SRP_SALT_BYTES);
    crypto_hash_sha512_update(&state, inner_hash, 64);
    crypto_hash_sha512_final(&state, x_hash);

    MBEDTLS_MPI_CHK(mbedtls_mpi_read_binary(&x, x_hash, 64));
    sodium_memzero(x_hash, sizeof(x_hash));
  }
  MBEDTLS_MPI_CHK(mbedtls_mpi_lset(&g, SRP_GENERATOR));

  // v = g^x mod N (512-bit exponent), kept for M3
  t0 = esp_timer_get_time();
  MBEDTLS_MPI_CHK(mbedtls_mpi_exp_mod(&v, &g, &x, &g_N, &g_rr));
  t1 = esp_timer_get_time();

  // Server secret b: 256 random bits (RFC 5054 asks for at least 256; Apple's
  // HomeKit ADK uses 32 bytes).  The accelerator's time follows the exponent
  // length, so this is 12x cheaper than a 3072-bit b for g^b and S.
  esp_fill_random(session->server_secret, SRP_SECRET_BYTES);
  MBEDTLS_MPI_CHK(
      mbedtls_mpi_read_binary(&b, session->server_secret, SRP_SECRET_BYTES));
  if (mbedtls_mpi_cmp_int(&b, 0) == 0) {
    ret = MBEDTLS_ERR_MPI_BAD_INPUT_DATA;
    goto cleanup;
  }

  // B = (k*v + g^b) mod N
  MBEDTLS_MPI_CHK(mbedtls_mpi_exp_mod(&tmp, &g, &b, &g_N, &g_rr));
  t2 = esp_timer_get_time();
  MBEDTLS_MPI_CHK(mbedtls_mpi_mul_mpi(&B, &g_k, &v));
  MBEDTLS_MPI_CHK(mbedtls_mpi_add_mpi(&B, &B, &tmp));
  MBEDTLS_MPI_CHK(srp_mod_n(&B, &B));
  if (mbedtls_mpi_cmp_int(&B, 0) == 0) {
    ret = MBEDTLS_ERR_MPI_BAD_INPUT_DATA;
    goto cleanup;
  }

  MBEDTLS_MPI_CHK(mpi_to_bytes_padded(&v, session->verifier, SRP_PRIME_BYTES));
  MBEDTLS_MPI_CHK(
      mpi_to_bytes_padded(&B, session->server_public_key, SRP_PRIME_BYTES));
  session->t1_us = t1 - t0;
  session->t2_us = t2 - t1;
  session->t3_us = 0;
  session->verified = false;
  session->state = 1;

cleanup:
  mbedtls_mpi_free(&g);
  mbedtls_mpi_free(&v);
  mbedtls_mpi_free(&b);
  mbedtls_mpi_free(&B);
  mbedtls_mpi_free(&x);
  mbedtls_mpi_free(&tmp);

  return ret == 0 ? ESP_OK : ESP_FAIL;
}

const uint8_t *srp_get_salt(srp_session_t *session) {
  return session ? session->salt : NULL;
}

const uint8_t *srp_get_public_key(srp_session_t *session, size_t *len) {
  if (!session) {
    return NULL;
  }
  if (len) {
    *len = SRP_PRIME_BYTES;
  }
  return session->server_public_key;
}

esp_err_t srp_verify_client(srp_session_t *session,
                            const uint8_t *client_public_key,
                            size_t client_pk_len, const uint8_t *client_proof,
                            size_t proof_len) {
  if (!session || !client_public_key || !client_proof ||
      proof_len < SRP_PROOF_BYTES) {
    return ESP_ERR_INVALID_ARG;
  }
  session->t1_us = 0;
  session->t2_us = 0;
  session->t3_us = 0;
  if (!g_ready || session->state != 1) {
    return ESP_ERR_INVALID_STATE;
  }

  // Store client's public key A (zero-padded)
  if (client_pk_len > SRP_PRIME_BYTES) {
    client_pk_len = SRP_PRIME_BYTES;
  }
  memset(session->client_public_key, 0, SRP_PRIME_BYTES);
  memcpy(session->client_public_key + (SRP_PRIME_BYTES - client_pk_len),
         client_public_key, client_pk_len);

  mbedtls_mpi A, Ar, B, b, u, S, v, tmp, tmp2;
  mbedtls_mpi_init(&A);
  mbedtls_mpi_init(&Ar);
  mbedtls_mpi_init(&B);
  mbedtls_mpi_init(&b);
  mbedtls_mpi_init(&u);
  mbedtls_mpi_init(&S);
  mbedtls_mpi_init(&v);
  mbedtls_mpi_init(&tmp);
  mbedtls_mpi_init(&tmp2);

  int ret = 0;
  bool proof_ok = false;
  int64_t t0 = 0;
  int64_t t1 = 0;
  int64_t t2 = 0;
  int64_t t3 = 0;

  // Load parameters
  MBEDTLS_MPI_CHK(
      mbedtls_mpi_read_binary(&A, session->client_public_key, SRP_PRIME_BYTES));
  MBEDTLS_MPI_CHK(
      mbedtls_mpi_read_binary(&B, session->server_public_key, SRP_PRIME_BYTES));
  MBEDTLS_MPI_CHK(
      mbedtls_mpi_read_binary(&b, session->server_secret, SRP_SECRET_BYTES));
  MBEDTLS_MPI_CHK(
      mbedtls_mpi_read_binary(&v, session->verifier, SRP_PRIME_BYTES));

  // Check A % N != 0 (covers A == 0)
  MBEDTLS_MPI_CHK(srp_mod_n(&Ar, &A));
  if (mbedtls_mpi_cmp_int(&Ar, 0) == 0) {
    ESP_LOGE(TAG, "Invalid client public key (A mod N == 0)");
    goto cleanup;
  }

  // u = H(PAD(A) || PAD(B))
  {
    uint8_t u_hash[64];
    crypto_hash_sha512_state state;
    crypto_hash_sha512_init(&state);
    crypto_hash_sha512_update(&state, session->client_public_key,
                              SRP_PRIME_BYTES);
    crypto_hash_sha512_update(&state, session->server_public_key,
                              SRP_PRIME_BYTES);
    crypto_hash_sha512_final(&state, u_hash);
    MBEDTLS_MPI_CHK(mbedtls_mpi_read_binary(&u, u_hash, 64));
  }
  if (mbedtls_mpi_cmp_int(&u, 0) == 0) {
    ESP_LOGE(TAG, "Invalid scrambler (u == 0)");
    goto cleanup;
  }

  // S = (A * v^u)^b mod N, v from M1 (no second g^x)
  t0 = esp_timer_get_time();
  MBEDTLS_MPI_CHK(mbedtls_mpi_exp_mod(&tmp, &v, &u, &g_N, &g_rr));
  t1 = esp_timer_get_time();
  MBEDTLS_MPI_CHK(mbedtls_mpi_mul_mpi(&tmp2, &Ar, &tmp));
  MBEDTLS_MPI_CHK(srp_mod_n(&tmp2, &tmp2));
  t2 = esp_timer_get_time();
  MBEDTLS_MPI_CHK(mbedtls_mpi_exp_mod(&S, &tmp2, &b, &g_N, &g_rr));
  t3 = esp_timer_get_time();
  session->t1_us = t1 - t0;
  session->t2_us = t3 - t2;
  session->t3_us = t2 - t1;

  // K = H(S)
  uint8_t S_bytes[SRP_PRIME_BYTES];
  size_t S_len = mpi_to_bytes_min(&S, S_bytes, sizeof(S_bytes));
  crypto_hash_sha512(session->session_key, S_bytes, S_len);
  sodium_memzero(S_bytes, sizeof(S_bytes));
  session->session_key_len = 64;

  // Compute expected M1 = H(H(N)^H(g) || H(I) || s || A || B || K)
  uint8_t expected_m1[64];
  {
    // H(I) where I = "Pair-Setup"
    uint8_t h_I[64];
    crypto_hash_sha512(h_I, (const uint8_t *)"Pair-Setup", 10);

    // Get minimal representations
    const uint8_t *salt_ptr;
    size_t salt_len;
    trim_leading_zeros(session->salt, SRP_SALT_BYTES, &salt_ptr, &salt_len);

    uint8_t A_bytes[SRP_PRIME_BYTES];
    uint8_t B_bytes[SRP_PRIME_BYTES];
    size_t A_len = mpi_to_bytes_min(&A, A_bytes, sizeof(A_bytes));
    size_t B_len = mpi_to_bytes_min(&B, B_bytes, sizeof(B_bytes));

    compute_m1(expected_m1, g_h_ng, h_I, salt_ptr, salt_len, A_bytes, A_len,
               B_bytes, B_len, session->session_key, 64);
  }

  // Verify client proof
  if (sodium_memcmp(client_proof, expected_m1, SRP_PROOF_BYTES) != 0) {
    ESP_LOGE(TAG, "Client proof verification failed");
    goto cleanup;
  }

  memcpy(session->proof_m1, client_proof, SRP_PROOF_BYTES);
  {
    uint8_t A_bytes[SRP_PRIME_BYTES];
    size_t A_len = mpi_to_bytes_min(&A, A_bytes, sizeof(A_bytes));

    crypto_hash_sha512_state state;
    crypto_hash_sha512_init(&state);
    crypto_hash_sha512_update(&state, A_bytes, A_len);
    crypto_hash_sha512_update(&state, session->proof_m1, SRP_PROOF_BYTES);
    crypto_hash_sha512_update(&state, session->session_key,
                              session->session_key_len);
    crypto_hash_sha512_final(&state, session->proof_m2);
  }

  session->verified = true;
  session->state = 2;
  proof_ok = true;

cleanup:
  mbedtls_mpi_free(&A);
  mbedtls_mpi_free(&Ar);
  mbedtls_mpi_free(&B);
  mbedtls_mpi_free(&b);
  mbedtls_mpi_free(&u);
  mbedtls_mpi_free(&S);
  mbedtls_mpi_free(&v);
  mbedtls_mpi_free(&tmp);
  mbedtls_mpi_free(&tmp2);

  return (ret == 0 && proof_ok) ? ESP_OK : ESP_FAIL;
}

const uint8_t *srp_get_proof(srp_session_t *session) {
  if (!session || !session->verified) {
    return NULL;
  }
  return session->proof_m2;
}

const uint8_t *srp_get_session_key(srp_session_t *session, size_t *len) {
  if (!session || !session->verified) {
    return NULL;
  }
  if (len) {
    *len = session->session_key_len;
  }
  return session->session_key;
}
