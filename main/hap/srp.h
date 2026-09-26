#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "mbedtls/bignum.h"

/**
 * SRP-6a implementation for HomeKit/AirPlay pair-setup
 * Uses 3072-bit prime N and SHA-512
 *
 * Cost on the ESP32-S3 RSA accelerator (fitted to the journal's M1/M3
 * times, not measured per primitive): one 3072-bit Montgomery multiplication
 * is ~0.13 ms and a modexp does ~1.5 of them per exponent bit, so exponent
 * LENGTH is what matters.  The old full-size 3072-bit server secret cost
 * ~0.6 s in each of M1 and M3; the 256-bit one below (as in Apple's HomeKit
 * ADK) ~50 ms.  mbedtls_mpi_mod_mpi() and the R^2 mod N that
 * mbedtls_mpi_exp_mod() recomputes on every call are long divisions of
 * ~100 small accelerator calls each (~25 ms), so R^2 is cached and the
 * reductions use Barrett (srp_mod_n).
 */

// SRP parameter sizes
#define SRP_PRIME_BITS        3072
#define SRP_PRIME_BYTES       (SRP_PRIME_BITS / 8) // 384 bytes
#define SRP_SALT_BYTES        16
#define SRP_SECRET_BYTES      32 // server secret b: 256 bits, as HomeKit ADK
#define SRP_PROOF_BYTES       64
#define SRP_SESSION_KEY_BYTES 64 // SHA-512 output

// SRP session context
typedef struct srp_session {
  uint8_t salt[SRP_SALT_BYTES];
  uint8_t server_public_key[SRP_PRIME_BYTES]; // B
  uint8_t server_secret[SRP_SECRET_BYTES];    // b
  uint8_t verifier[SRP_PRIME_BYTES];          // v = g^x, kept for M3
  uint8_t client_public_key[SRP_PRIME_BYTES]; // A
  uint8_t session_key[SRP_SESSION_KEY_BYTES]; // K
  size_t session_key_len;
  uint8_t proof_m1[SRP_SESSION_KEY_BYTES]; // Client proof
  uint8_t proof_m2[SRP_SESSION_KEY_BYTES]; // Server proof
  int state;
  bool verified;
  // Microseconds spent in the last srp_start() / srp_verify_client(), for
  // the one-line pairing log.  srp_start: t1 = g^x, t2 = g^b.
  // srp_verify_client: t1 = v^u, t2 = (A*v^u)^b, t3 = A*v^u mod N.
  int64_t t1_us;
  int64_t t2_us;
  int64_t t3_us;
} srp_session_t;

/**
 * One-time init of the per-device constants (N, k, H(N)^H(g), R^2 mod N and
 * the Barrett constant).  Call once before any other srp_* function, from a
 * single task; everything it sets is read-only afterwards.
 */
esp_err_t srp_global_init(void);

/**
 * Create a new SRP session
 */
srp_session_t *srp_session_create(void);

/**
 * Free an SRP session
 */
void srp_session_free(srp_session_t *session);

/**
 * Start SRP session (generate salt, secret b, verifier v and public key B)
 * For transient pairing, username="Pair-Setup" and password="3939"
 *
 * @param session SRP session
 * @param username Username (typically "Pair-Setup")
 * @param password Password (typically "3939" for transient)
 * @return ESP_OK on success
 */
esp_err_t srp_start(srp_session_t *session, const char *username,
                    const char *password);

/**
 * Get salt for M2 response
 */
const uint8_t *srp_get_salt(srp_session_t *session);

/**
 * Get server public key B for M2 response
 * @param len Output: length of public key
 */
const uint8_t *srp_get_public_key(srp_session_t *session, size_t *len);

/**
 * Process client's public key A and proof M1 from M3
 * Verifies client's proof and generates server proof M2
 *
 * @param session SRP session
 * @param client_public_key Client's public key A
 * @param client_pk_len Length of client public key
 * @param client_proof Client's proof M1
 * @param proof_len Length of proof
 * @return ESP_OK if verification succeeds
 */
esp_err_t srp_verify_client(srp_session_t *session,
                            const uint8_t *client_public_key,
                            size_t client_pk_len, const uint8_t *client_proof,
                            size_t proof_len);

/**
 * Get server proof M2 for M4 response
 */
const uint8_t *srp_get_proof(srp_session_t *session);

/**
 * Get session key K after successful verification
 * This key is used to encrypt M5/M6 messages
 */
const uint8_t *srp_get_session_key(srp_session_t *session, size_t *len);

/**
 * r = x mod N for 0 <= x < 2^6144 (Barrett), falling back to
 * mbedtls_mpi_mod_mpi() outside that range.  r may alias x.  Exposed for the
 * host test.
 */
int srp_mod_n(mbedtls_mpi *r, const mbedtls_mpi *x);

/**
 * Spare server keys for the next transient pair-setup (srp_pool.c).
 *
 * srp_pool_start() starts a low-priority task that keeps one precomputed
 * {salt, b, v, B} for "Pair-Setup"/"3939" ready, so M1 does no modexp.
 * srp_pool_take() moves it into @p session (true) or returns false (pool
 * empty, other credentials, or pool not started); either way it schedules a
 * refill a little later so the refill never competes with the M3 that
 * follows.
 */
void srp_pool_start(void);
bool srp_pool_take(srp_session_t *session, const char *username,
                   const char *password);
