#include <string.h>

#include "hap.h"
#include "hap_internal.h"
#include "hap_pairings.h"
#include "srp.h"
#include "tlv8.h"

#include "esp_log.h"
#include "sodium.h"

static const char *TAG = "hap_setup";

#define TLV_TYPE_METHOD 0x00
#define TLV_TYPE_SALT   0x02
#define TLV_TYPE_PROOF  0x04
#define TLV_TYPE_FLAGS  0x13

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

  bool transient = false;
  if (flags && flags_len == 1) {
    transient = (flags[0] & 0x10) != 0;
  }
  session->pair_setup_transient = transient;

  if (session->srp) {
    srp_session_free(session->srp);
  }
  session->srp = srp_session_create();
  if (!session->srp) {
    ESP_LOGE(TAG, "Failed to create SRP session");
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = transient ? srp_start_transient(session->srp)
                            : srp_start(session->srp, "Pair-Setup", "0000");
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
  // This pairing's heavy math is done: let the pool prepare the next one.
  srp_pool_refill();
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

    memcpy(session->shared_secret, srp_key, 32);
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

// M6 failure reply: state M6 and an error code, in the clear.
static esp_err_t m6_error(uint8_t *output, size_t output_capacity,
                          size_t *output_len, uint8_t code) {
  tlv8_encoder_t enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_SETUP_M6);
  tlv8_encode_byte(&enc, TLV_TYPE_ERROR, code);
  *output_len = tlv8_encoder_size(&enc);
  return ESP_OK;
}

// Non-transient pair-setup, exchange M5/M6 (HAP pair-setup steps 5-6): check
// the controller's signature, remember its long-term key, and answer with
// ours, signed, so the controller can pair-verify on later connects instead
// of running SRP again.  Transient pairing stops at M4 and never gets here.
esp_err_t hap_pair_setup_m5(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len) {
  if (!session->srp || session->pair_setup_state != PAIR_SETUP_M4 ||
      session->pair_setup_transient) {
    ESP_LOGE(TAG, "M5 without a completed non-transient M4");
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
                          sizeof(encrypted), &encrypted_len) ||
      encrypted_len < crypto_aead_chacha20poly1305_ietf_ABYTES) {
    ESP_LOGE(TAG, "Missing encrypted data in M5");
    return m6_error(output, output_capacity, output_len,
                    TLV_ERROR_AUTHENTICATION);
  }

  size_t srp_key_len = 0;
  const uint8_t *srp_key = srp_get_session_key(session->srp, &srp_key_len);
  if (!srp_key || srp_key_len == 0) {
    ESP_LOGE(TAG, "Missing SRP session key");
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t setup_key[32];
  hap_hkdf_sha512((const uint8_t *)"Pair-Setup-Encrypt-Salt", 23, srp_key,
                  srp_key_len, (const uint8_t *)"Pair-Setup-Encrypt-Info", 23,
                  setup_key, sizeof(setup_key));

  uint8_t nonce5[12] = {0, 0, 0, 0, 'P', 'S', '-', 'M', 's', 'g', '0', '5'};
  uint8_t sub[512];
  unsigned long long sub_len = 0;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(sub, &sub_len, NULL, encrypted,
                                                encrypted_len, NULL, 0, nonce5,
                                                setup_key) != 0) {
    ESP_LOGE(TAG, "M5 decryption failed");
    return m6_error(output, output_capacity, output_len,
                    TLV_ERROR_AUTHENTICATION);
  }

  size_t id_len = 0;
  size_t ltpk_len = 0;
  size_t sig_len = 0;
  const uint8_t *ctrl_id =
      tlv8_find(sub, (size_t)sub_len, TLV_TYPE_IDENTIFIER, &id_len);
  const uint8_t *ctrl_ltpk =
      tlv8_find(sub, (size_t)sub_len, TLV_TYPE_PUBLIC_KEY, &ltpk_len);
  const uint8_t *ctrl_sig =
      tlv8_find(sub, (size_t)sub_len, TLV_TYPE_SIGNATURE, &sig_len);
  if (!ctrl_id || id_len == 0 || id_len > HAP_PAIRING_ID_MAX || !ctrl_ltpk ||
      ltpk_len != crypto_sign_PUBLICKEYBYTES || !ctrl_sig ||
      sig_len != crypto_sign_BYTES) {
    ESP_LOGE(TAG, "M5 sub-TLV incomplete (id=%zu pk=%zu sig=%zu)", id_len,
             ltpk_len, sig_len);
    return m6_error(output, output_capacity, output_len,
                    TLV_ERROR_AUTHENTICATION);
  }

  // iOSDeviceInfo = iOSDeviceX || iOSDevicePairingID || iOSDeviceLTPK
  uint8_t info[32 + HAP_PAIRING_ID_MAX + 32];
  size_t info_len = 32;
  hap_hkdf_sha512((const uint8_t *)"Pair-Setup-Controller-Sign-Salt", 31,
                  srp_key, srp_key_len,
                  (const uint8_t *)"Pair-Setup-Controller-Sign-Info", 31, info,
                  32);
  memcpy(info + info_len, ctrl_id, id_len);
  info_len += id_len;
  memcpy(info + info_len, ctrl_ltpk, 32);
  info_len += 32;
  if (crypto_sign_verify_detached(ctrl_sig, info, info_len, ctrl_ltpk) != 0) {
    ESP_LOGE(TAG, "M5 controller signature invalid");
    return m6_error(output, output_capacity, output_len,
                    TLV_ERROR_AUTHENTICATION);
  }

  if (hap_pairings_add(ctrl_id, id_len, ctrl_ltpk) != ESP_OK) {
    return m6_error(output, output_capacity, output_len, TLV_ERROR_MAX_PEERS);
  }

  // AccessoryInfo = AccessoryX || AccessoryPairingID || AccessoryLTPK.  The
  // pairing ID is the one pair-verify M2 signs with.
  char acc_id[18];
  hap_get_pairing_id(acc_id, sizeof(acc_id));
  size_t acc_id_len = strlen(acc_id);
  info_len = 32;
  hap_hkdf_sha512((const uint8_t *)"Pair-Setup-Accessory-Sign-Salt", 30,
                  srp_key, srp_key_len,
                  (const uint8_t *)"Pair-Setup-Accessory-Sign-Info", 30, info,
                  32);
  memcpy(info + info_len, acc_id, acc_id_len);
  info_len += acc_id_len;
  memcpy(info + info_len, session->device_public_key, 32);
  info_len += 32;
  uint8_t acc_sig[crypto_sign_BYTES];
  crypto_sign_detached(acc_sig, NULL, info, info_len,
                       session->device_secret_key);

  uint8_t plain[256];
  tlv8_encoder_t sub_enc;
  tlv8_encoder_init(&sub_enc, plain, sizeof(plain));
  tlv8_encode(&sub_enc, TLV_TYPE_IDENTIFIER, (const uint8_t *)acc_id,
              acc_id_len);
  tlv8_encode(&sub_enc, TLV_TYPE_PUBLIC_KEY, session->device_public_key, 32);
  tlv8_encode(&sub_enc, TLV_TYPE_SIGNATURE, acc_sig, sizeof(acc_sig));

  uint8_t nonce6[12] = {0, 0, 0, 0, 'P', 'S', '-', 'M', 's', 'g', '0', '6'};
  uint8_t sealed[256 + crypto_aead_chacha20poly1305_ietf_ABYTES];
  unsigned long long sealed_len = 0;
  crypto_aead_chacha20poly1305_ietf_encrypt(sealed, &sealed_len, plain,
                                            tlv8_encoder_size(&sub_enc), NULL,
                                            0, NULL, nonce6, setup_key);
  sodium_memzero(setup_key, sizeof(setup_key));

  tlv8_encoder_t enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_SETUP_M6);
  tlv8_encode(&enc, TLV_TYPE_ENCRYPTED_DATA, sealed, (size_t)sealed_len);
  *output_len = tlv8_encoder_size(&enc);
  session->pair_setup_state = PAIR_SETUP_M6;
  ESP_LOGI(TAG, "non-transient pair-setup complete for %.*s", (int)id_len,
           (const char *)ctrl_id);
  return ESP_OK;
}
