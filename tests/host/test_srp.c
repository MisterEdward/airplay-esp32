// Host test for the SRP-6a server (main/hap/srp.c) against an independent
// client written here from RFC 5054 (SHA-512, 3072-bit group, g = 5), with
// the HomeKit conventions srp.c uses (minimal-length S, A, B and salt in the
// proofs).  Covers the transient PIN, a non-transient PIN, a wrong PIN, and a
// session whose server side was precomputed and copied, as srp_pool does.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../main/hap/srp.c"

static void h512(uint8_t *out, const void *a, size_t alen) {
  crypto_hash_sha512(out, (const uint8_t *)a, alen);
}

static size_t min_bytes(const mbedtls_mpi *m, uint8_t *buf) {
  size_t n = mbedtls_mpi_size(m);
  mbedtls_mpi_write_binary(m, buf, n);
  return n;
}

// Runs one pair-setup M1..M4 exchange; returns 0 when the server accepted the
// client's proof and the client accepted the server's.
static int exchange(srp_session_t *srv, const char *client_pin) {
  mbedtls_mpi N, g, a, A, B, k, x, u, S, t1, t2, e;
  mbedtls_mpi *all[] = {&N, &g, &a, &A, &B, &k, &x, &u, &S, &t1, &t2, &e};
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    mbedtls_mpi_init(all[i]);
  }
  mbedtls_mpi_read_binary(&N, srp_N, sizeof(srp_N));
  mbedtls_mpi_lset(&g, 5);

  uint8_t a_bytes[32];
  for (size_t i = 0; i < sizeof(a_bytes); i++) {
    a_bytes[i] = (uint8_t)rand();
  }
  mbedtls_mpi_read_binary(&a, a_bytes, sizeof(a_bytes));
  assert(mbedtls_mpi_exp_mod(&A, &g, &a, &N, NULL) == 0);

  size_t blen = 0;
  const uint8_t *Bb = srp_get_public_key(srv, &blen);
  const uint8_t *salt = srp_get_salt(srv);
  mbedtls_mpi_read_binary(&B, Bb, blen);

  uint8_t Apad[SRP_PRIME_BYTES];
  mbedtls_mpi_write_binary(&A, Apad, sizeof(Apad));
  uint8_t Bpad[SRP_PRIME_BYTES];
  mbedtls_mpi_write_binary(&B, Bpad, sizeof(Bpad));

  uint8_t hbuf[64];
  uint8_t cat[SRP_PRIME_BYTES * 2];
  memcpy(cat, Apad, SRP_PRIME_BYTES);
  memcpy(cat + SRP_PRIME_BYTES, Bpad, SRP_PRIME_BYTES);
  h512(hbuf, cat, sizeof(cat));
  mbedtls_mpi_read_binary(&u, hbuf, 64);

  memcpy(cat, srp_N, SRP_PRIME_BYTES);
  memset(cat + SRP_PRIME_BYTES, 0, SRP_PRIME_BYTES);
  cat[sizeof(cat) - 1] = 5;
  h512(hbuf, cat, sizeof(cat));
  mbedtls_mpi_read_binary(&k, hbuf, 64);

  char ip[64];
  int iplen = snprintf(ip, sizeof(ip), "Pair-Setup:%s", client_pin);
  uint8_t inner[64];
  h512(inner, ip, (size_t)iplen);
  uint8_t sx[SRP_SALT_BYTES + 64];
  memcpy(sx, salt, SRP_SALT_BYTES);
  memcpy(sx + SRP_SALT_BYTES, inner, 64);
  h512(hbuf, sx, sizeof(sx));
  mbedtls_mpi_read_binary(&x, hbuf, 64);

  // S = (B - k * g^x) ^ (a + u * x) mod N
  assert(mbedtls_mpi_exp_mod(&t1, &g, &x, &N, NULL) == 0);
  assert(mbedtls_mpi_mul_mpi(&t2, &k, &t1) == 0);
  assert(mbedtls_mpi_mod_mpi(&t2, &t2, &N) == 0);
  assert(mbedtls_mpi_sub_mpi(&t1, &B, &t2) == 0);
  assert(mbedtls_mpi_mod_mpi(&t1, &t1, &N) == 0);
  assert(mbedtls_mpi_mul_mpi(&e, &u, &x) == 0);
  assert(mbedtls_mpi_add_mpi(&e, &e, &a) == 0);
  assert(mbedtls_mpi_exp_mod(&S, &t1, &e, &N, NULL) == 0);

  uint8_t Sb[SRP_PRIME_BYTES];
  uint8_t K[64];
  h512(K, Sb, min_bytes(&S, Sb));

  uint8_t hN[64], hg[64], hI[64], xr[64];
  uint8_t gb = 5;
  h512(hN, srp_N, sizeof(srp_N));
  h512(hg, &gb, 1);
  h512(hI, "Pair-Setup", 10);
  for (int i = 0; i < 64; i++) {
    xr[i] = hN[i] ^ hg[i];
  }
  const uint8_t *sp = salt;
  size_t sl = SRP_SALT_BYTES;
  while (sl > 1 && *sp == 0) {
    sp++;
    sl--;
  }
  uint8_t Amin[SRP_PRIME_BYTES], Bmin[SRP_PRIME_BYTES];
  size_t amin = min_bytes(&A, Amin);
  size_t bmin = min_bytes(&B, Bmin);
  uint8_t M1[64];
  crypto_hash_sha512_state st;
  crypto_hash_sha512_init(&st);
  crypto_hash_sha512_update(&st, xr, 64);
  crypto_hash_sha512_update(&st, hI, 64);
  crypto_hash_sha512_update(&st, sp, sl);
  crypto_hash_sha512_update(&st, Amin, amin);
  crypto_hash_sha512_update(&st, Bmin, bmin);
  crypto_hash_sha512_update(&st, K, 64);
  crypto_hash_sha512_final(&st, M1);

  int rc = -1;
  if (srp_verify_client(srv, Amin, amin, M1, 64) == ESP_OK) {
    uint8_t M2[64];
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, Amin, amin);
    crypto_hash_sha512_update(&st, M1, 64);
    crypto_hash_sha512_update(&st, K, 64);
    crypto_hash_sha512_final(&st, M2);
    size_t klen = 0;
    const uint8_t *sk = srp_get_session_key(srv, &klen);
    assert(sk && klen == 64);
    assert(memcmp(sk, K, 64) == 0);
    assert(memcmp(srp_get_proof(srv), M2, 64) == 0);
    rc = 0;
  }
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    mbedtls_mpi_free(all[i]);
  }
  return rc;
}

static void test_sha512(void) {
  uint8_t out[64];
  h512(out, "abc", 3);
  static const uint8_t want[8] = {0xdd, 0xaf, 0x35, 0xa1,
                                  0x93, 0x61, 0x7a, 0xba};
  assert(memcmp(out, want, sizeof(want)) == 0);
}

int main(void) {
  srand(7);
  test_sha512();

  srp_session_t *s = srp_session_create();
  assert(srp_start(s, "Pair-Setup", "3939") == ESP_OK);
  assert(exchange(s, "3939") == 0);
  srp_session_free(s);

  // A non-transient PIN: srp_verify_client used to recompute v with 3939.
  s = srp_session_create();
  assert(srp_start(s, "Pair-Setup", "0000") == ESP_OK);
  assert(exchange(s, "0000") == 0);
  srp_session_free(s);

  s = srp_session_create();
  assert(srp_start(s, "Pair-Setup", "3939") == ESP_OK);
  assert(exchange(s, "1234") != 0);
  srp_session_free(s);

  // Precomputed server half, copied into the live session later.
  srp_session_t *pre = srp_session_create();
  assert(srp_start(pre, "Pair-Setup", "3939") == ESP_OK);
  s = srp_session_create();
  memcpy(s, pre, sizeof(*s));
  srp_session_free(pre);
  assert(exchange(s, "3939") == 0);
  srp_session_free(s);

  printf("srp: all tests passed\n");
  return 0;
}
