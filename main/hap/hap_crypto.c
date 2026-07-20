#include <string.h>

#include "hap.h"
#include "hap_internal.h"

#include "esp_log.h"
#include "mbedtls/md.h"
#include "sodium.h"

static const char *TAG = "hap_crypto";

#define HKDF_SHA512_HASH_LEN 64

// RFC 5869 HKDF over mbedTLS HMAC-SHA512.
//
// This used to call libsodium's crypto_auth_hmacsha512_* streaming API with
// keys whose length is not crypto_auth_hmacsha512_KEYBYTES (the salt is 12
// bytes and the PRK is 64). That produced HMAC output which does not match a
// reference HKDF, so the derived AirPlay 2 control-channel keys were wrong
// even though both peers agreed on the SRP session key, and every encrypted
// RTSP frame failed authentication. mbedTLS' HMAC handles arbitrary key
// lengths per the spec.
int hap_hkdf_sha512(const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
                    size_t ikm_len, const uint8_t *info, size_t info_len,
                    uint8_t *okm, size_t okm_len) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
  if (!md || !okm) {
    return -1;
  }

  uint8_t zero_salt[HKDF_SHA512_HASH_LEN] = {0};
  const uint8_t *use_salt = (salt && salt_len > 0) ? salt : zero_salt;
  size_t use_salt_len = (salt && salt_len > 0) ? salt_len : sizeof(zero_salt);

  uint8_t prk[HKDF_SHA512_HASH_LEN];
  uint8_t t[HKDF_SHA512_HASH_LEN];
  size_t t_len = 0;
  size_t pos = 0;
  uint8_t counter = 1;
  int ret = -1;

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);

  // Extract: PRK = HMAC(salt, ikm)
  if (mbedtls_md_hmac(md, use_salt, use_salt_len, ikm, ikm_len, prk) != 0) {
    goto done;
  }

  // Expand: T(n) = HMAC(PRK, T(n-1) || info || n)
  if (mbedtls_md_setup(&ctx, md, 1) != 0) {
    goto done;
  }
  while (pos < okm_len) {
    if (mbedtls_md_hmac_starts(&ctx, prk, sizeof(prk)) != 0 ||
        (t_len > 0 && mbedtls_md_hmac_update(&ctx, t, t_len) != 0) ||
        (info && info_len > 0 &&
         mbedtls_md_hmac_update(&ctx, info, info_len) != 0) ||
        mbedtls_md_hmac_update(&ctx, &counter, 1) != 0 ||
        mbedtls_md_hmac_finish(&ctx, t) != 0) {
      goto done;
    }
    t_len = sizeof(t);

    size_t copy_len = okm_len - pos;
    if (copy_len > sizeof(t)) {
      copy_len = sizeof(t);
    }
    memcpy(okm + pos, t, copy_len);
    pos += copy_len;
    counter++;
  }
  ret = 0;

done:
  mbedtls_md_free(&ctx);
  sodium_memzero(prk, sizeof(prk));
  sodium_memzero(t, sizeof(t));
  return ret;
}

esp_err_t hap_derive_audio_key(hap_session_t *session, uint8_t *audio_key,
                               size_t key_len) {
  if (!session || !audio_key || key_len < 16) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!session->session_established) {
    ESP_LOGW(TAG, "Cannot derive audio key before session established");
    return ESP_ERR_INVALID_STATE;
  }

  hap_hkdf_sha512((uint8_t *)"Control-Salt", 12, session->shared_secret, 32,
                  (uint8_t *)"Control-Read-Encryption-Key", 27, audio_key,
                  key_len);

  return ESP_OK;
}

esp_err_t hap_encrypt(hap_session_t *session, const uint8_t *plaintext,
                      size_t plaintext_len, uint8_t *ciphertext,
                      size_t *ciphertext_len) {
  if (!session->session_established) {
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, &session->encrypt_nonce, 8);

  unsigned long long ct_len = 0;
  crypto_aead_chacha20poly1305_ietf_encrypt(ciphertext, &ct_len, plaintext,
                                            plaintext_len, NULL, 0, NULL, nonce,
                                            session->encrypt_key);

  *ciphertext_len = (size_t)ct_len;
  session->encrypt_nonce++;

  return ESP_OK;
}

esp_err_t hap_decrypt(hap_session_t *session, const uint8_t *ciphertext,
                      size_t ciphertext_len, uint8_t *plaintext,
                      size_t *plaintext_len) {
  if (!session->session_established) {
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, &session->decrypt_nonce, 8);

  unsigned long long pt_len = 0;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(
          plaintext, &pt_len, NULL, ciphertext, ciphertext_len, NULL, 0, nonce,
          session->decrypt_key) != 0) {
    return ESP_ERR_INVALID_STATE;
  }

  *plaintext_len = (size_t)pt_len;
  session->decrypt_nonce++;

  return ESP_OK;
}
