#include <string.h>

#include "hap.h"
#include "hap_internal.h"
#include "srp.h"
#include "tlv8.h"

#include "esp_log.h"
#include "sodium.h"

static const char *TAG = "hap_setup";

#define TLV_TYPE_METHOD 0x00
#define TLV_TYPE_SALT   0x02
#define TLV_TYPE_PROOF  0x04
#define TLV_TYPE_FLAGS  0x13

// kPairingFlag_Transient from the HAP pairing-flags bitfield.
#define PAIRING_FLAG_TRANSIENT 0x10

#define PAIR_SETUP_M1 1
#define PAIR_SETUP_M2 2
#define PAIR_SETUP_M3 3
#define PAIR_SETUP_M4 4
#define PAIR_SETUP_M5 5
#define PAIR_SETUP_M6 6

esp_err_t hap_pair_setup_m1(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len) {
  size_t state_len = 0;
  size_t flags_len = 0;
  const uint8_t *state =
      tlv8_find(input, input_len, TLV_TYPE_STATE, &state_len);
  const uint8_t *flags =
      tlv8_find(input, input_len, TLV_TYPE_FLAGS, &flags_len);

  if (!state || state_len != 1 || state[0] != PAIR_SETUP_M1) {
    ESP_LOGE(TAG, "Invalid pair-setup M1 state");
    return ESP_ERR_INVALID_ARG;
  }

  // The pairing-flags TLV is a little-endian integer of minimal length, not a
  // single byte.  AirPlay 2 transient setup sends 0x138, which needs two or
  // four bytes, so an exact flags_len==1 test silently misses the transient
  // bit and starts SRP with the wrong password ("0000" instead of "3939").
  // The exchange then completes with a shared secret the sender does not
  // share, and the first encrypted RTSP frame fails to decrypt.
  uint64_t flag_bits = 0;
  if (flags && flags_len > 0 && flags_len <= sizeof(flag_bits)) {
    for (size_t i = 0; i < flags_len; i++) {
      flag_bits |= (uint64_t)flags[i] << (8 * i);
    }
  }
  bool transient = (flag_bits & PAIRING_FLAG_TRANSIENT) != 0;
  session->pair_setup_transient = transient;
  ESP_LOGI(TAG, "Pair-setup M1: flags=0x%llx (%u bytes) transient=%d",
           (unsigned long long)flag_bits, (unsigned)flags_len, transient);

  if (session->srp) {
    srp_session_free(session->srp);
  }
  session->srp = srp_session_create();
  if (!session->srp) {
    ESP_LOGE(TAG, "Failed to create SRP session");
    return ESP_ERR_NO_MEM;
  }

  const char *password = transient ? "3939" : "0000";
  esp_err_t err = srp_start(session->srp, "Pair-Setup", password);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start SRP: %d", err);
    return err;
  }

  size_t pk_len = 0;
  const uint8_t *salt = srp_get_salt(session->srp);
  const uint8_t *pk = srp_get_public_key(session->srp, &pk_len);

  tlv8_encoder_t enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_SETUP_M2);
  tlv8_encode(&enc, TLV_TYPE_SALT, salt, SRP_SALT_BYTES);
  tlv8_encode(&enc, TLV_TYPE_PUBLIC_KEY, pk, pk_len);

  *output_len = tlv8_encoder_size(&enc);
  session->pair_setup_state = PAIR_SETUP_M2;
  return ESP_OK;
}

esp_err_t hap_pair_setup_m3(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len) {
  if (!session->srp) {
    ESP_LOGE(TAG, "No SRP session for M3");
    return ESP_ERR_INVALID_STATE;
  }

  size_t state_len = 0;
  size_t pk_len = 0;
  size_t proof_len = 0;
  const uint8_t *state =
      tlv8_find(input, input_len, TLV_TYPE_STATE, &state_len);

  if (!state || state_len != 1 || state[0] != PAIR_SETUP_M3) {
    ESP_LOGE(TAG, "Invalid pair-setup M3 state");
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t client_pk[512];
  if (!tlv8_decode_concat(input, input_len, TLV_TYPE_PUBLIC_KEY, client_pk,
                          sizeof(client_pk), &pk_len)) {
    ESP_LOGE(TAG, "Missing client public key in M3");
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t client_proof[64];
  if (!tlv8_decode_concat(input, input_len, TLV_TYPE_PROOF, client_proof,
                          sizeof(client_proof), &proof_len)) {
    ESP_LOGE(TAG, "Missing client proof in M3");
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err = srp_verify_client(session->srp, client_pk, pk_len,
                                    client_proof, proof_len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Client verification failed");
    tlv8_encoder_t enc;
    tlv8_encoder_init(&enc, output, output_capacity);
    tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_SETUP_M4);
    tlv8_encode_byte(&enc, TLV_TYPE_ERROR, 0x02);
    *output_len = tlv8_encoder_size(&enc);
    return ESP_OK;
  }

  if (session->pair_setup_transient) {
    size_t srp_key_len = 0;
    const uint8_t *srp_key = srp_get_session_key(session->srp, &srp_key_len);
    if (!srp_key || srp_key_len == 0) {
      ESP_LOGE(TAG, "Missing SRP session key for transient pairing");
      return ESP_ERR_INVALID_STATE;
    }

    // Full material so the HKDF derivation can be reproduced off-device and
    // compared byte for byte against a reference implementation. Split across
    // lines because the log transport truncates long records.
    char hex[80];
    for (int half = 0; half < 2; half++) {
      for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", srp_key[half * 32 + i]);
      }
      ESP_LOGI(TAG, "HKDFCHK K[%d]=%s", half, hex);
    }
    ESP_LOGI(TAG, "HKDFCHK srp_key_len=%u", (unsigned)srp_key_len);
    memcpy(session->shared_secret, srp_key, 32);

    // Alternate derivation using only the first 32 bytes of K, for senders
    // that treat the shared secret as 32 bytes wide.
    hap_hkdf_sha512((uint8_t *)"Control-Salt", 12, srp_key, 32,
                    (uint8_t *)"Control-Read-Encryption-Key", 27,
                    session->alt_encrypt_key, 32);
    hap_hkdf_sha512((uint8_t *)"Control-Salt", 12, srp_key, 32,
                    (uint8_t *)"Control-Write-Encryption-Key", 28,
                    session->alt_decrypt_key, 32);
    session->alt_keys_valid = true;

    for (int i = 0; i < 32; i++) {
      snprintf(hex + i * 2, 3, "%02x", session->decrypt_key[i]);
    }
    ESP_LOGI(TAG, "HKDFCHK read64=%s", hex);
    for (int i = 0; i < 32; i++) {
      snprintf(hex + i * 2, 3, "%02x", session->encrypt_key[i]);
    }
    ESP_LOGI(TAG, "HKDFCHK write64=%s", hex);
    hap_hkdf_sha512((uint8_t *)"Control-Salt", 12, srp_key, srp_key_len,
                    (uint8_t *)"Control-Read-Encryption-Key", 27,
                    session->encrypt_key, 32);
    hap_hkdf_sha512((uint8_t *)"Control-Salt", 12, srp_key, srp_key_len,
                    (uint8_t *)"Control-Write-Encryption-Key", 28,
                    session->decrypt_key, 32);
    session->encrypt_nonce = 0;
    session->decrypt_nonce = 0;
    session->session_established = true;
  }

  const uint8_t *server_proof = srp_get_proof(session->srp);

  tlv8_encoder_t enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_SETUP_M4);
  tlv8_encode(&enc, TLV_TYPE_PROOF, server_proof, SRP_PROOF_BYTES);

  *output_len = tlv8_encoder_size(&enc);
  session->pair_setup_state = PAIR_SETUP_M4;

  return ESP_OK;
}

esp_err_t hap_pair_setup_m5(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len) {
  (void)output;
  (void)output_capacity;
  (void)output_len;

  if (!session->srp) {
    ESP_LOGE(TAG, "No SRP session for M5");
    return ESP_ERR_INVALID_STATE;
  }

  size_t state_len = 0;
  const uint8_t *state =
      tlv8_find(input, input_len, TLV_TYPE_STATE, &state_len);

  if (!state || state_len != 1 || state[0] != PAIR_SETUP_M5) {
    ESP_LOGE(TAG, "Invalid pair-setup M5 state");
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t encrypted[512];
  size_t encrypted_len = 0;
  if (!tlv8_decode_concat(input, input_len, TLV_TYPE_ENCRYPTED_DATA, encrypted,
                          sizeof(encrypted), &encrypted_len)) {
    ESP_LOGE(TAG, "Missing encrypted data in M5");
    return ESP_ERR_INVALID_ARG;
  }

  size_t srp_key_len = 0;
  const uint8_t *srp_key = srp_get_session_key(session->srp, &srp_key_len);
  if (!srp_key || srp_key_len == 0) {
    ESP_LOGE(TAG, "Missing SRP session key");
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t setup_key[32];
  hap_hkdf_sha512((uint8_t *)"Pair-Setup-Encrypt-Salt", 23, srp_key,
                  srp_key_len, (uint8_t *)"Pair-Setup-Encrypt-Info", 23,
                  setup_key, 32);

  uint8_t nonce[12] = {0, 0, 0, 0, 'P', 'S', '-', 'M', 's', 'g', '0', '5'};
  uint8_t decrypted[512];
  unsigned long long decrypted_len = 0;

  if (crypto_aead_chacha20poly1305_ietf_decrypt(decrypted, &decrypted_len, NULL,
                                                encrypted, encrypted_len, NULL,
                                                0, nonce, setup_key) != 0) {
    ESP_LOGE(TAG, "M5 decryption failed");
    return ESP_FAIL;
  }

  session->encrypt_nonce = 0;
  session->decrypt_nonce = 0;

  return ESP_OK;
}
