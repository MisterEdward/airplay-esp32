#include <string.h>

#include "hap.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sodium.h"

#include "srp.h"

static const char *TAG = "hap";

#define NVS_NAMESPACE  "airplay"
#define NVS_KEY_PUBLIC "ed25519_pub"
#define NVS_KEY_SECRET "ed25519_sec"

static uint8_t g_device_public_key[HAP_ED25519_PUBLIC_KEY_SIZE];
static uint8_t g_device_secret_key[HAP_ED25519_SECRET_KEY_SIZE];
static bool g_initialized = false;

esp_err_t hap_init(void) {
  if (g_initialized) {
    return ESP_OK;
  }

  if (sodium_init() < 0) {
    ESP_LOGE(TAG, "Failed to initialize libsodium");
    return ESP_FAIL;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  size_t pub_len = HAP_ED25519_PUBLIC_KEY_SIZE;
  size_t sec_len = HAP_ED25519_SECRET_KEY_SIZE;

  err = nvs_get_blob(nvs, NVS_KEY_PUBLIC, g_device_public_key, &pub_len);
  if (err == ESP_OK) {
    err = nvs_get_blob(nvs, NVS_KEY_SECRET, g_device_secret_key, &sec_len);
  }

  if (err != ESP_OK || pub_len != HAP_ED25519_PUBLIC_KEY_SIZE ||
      sec_len != HAP_ED25519_SECRET_KEY_SIZE) {
    crypto_sign_keypair(g_device_public_key, g_device_secret_key);

    err = nvs_set_blob(nvs, NVS_KEY_PUBLIC, g_device_public_key,
                       HAP_ED25519_PUBLIC_KEY_SIZE);
    if (err == ESP_OK) {
      err = nvs_set_blob(nvs, NVS_KEY_SECRET, g_device_secret_key,
                         HAP_ED25519_SECRET_KEY_SIZE);
    }
    if (err == ESP_OK) {
      err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to store keypair in NVS: %s", esp_err_to_name(err));
      nvs_close(nvs);
      return err;
    }
  }

  nvs_close(nvs);

  srp_pool_init();
  g_initialized = true;
  return ESP_OK;
}

const uint8_t *hap_get_public_key(void) {
  return g_device_public_key;
}

void hap_get_pairing_id(char *out, size_t out_len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

hap_session_t *hap_session_create(void) {
  hap_session_t *session = calloc(1, sizeof(hap_session_t));
  if (!session) {
    return NULL;
  }

  memcpy(session->device_public_key, g_device_public_key,
         HAP_ED25519_PUBLIC_KEY_SIZE);
  memcpy(session->device_secret_key, g_device_secret_key,
         HAP_ED25519_SECRET_KEY_SIZE);

  crypto_box_keypair(session->session_public_key, session->session_secret_key);

  session->pair_verify_state = 0;
  session->session_established = false;
  session->pair_setup_transient = false;
  session->encrypt_nonce = 0;
  session->decrypt_nonce = 0;

  return session;
}

void hap_session_free(hap_session_t *session) {
  if (!session) {
    return;
  }

  if (session->srp) {
    srp_session_free(session->srp);
    session->srp = NULL;
  }
  // A connection that left after M1 took the precomputed context.
  srp_pool_refill();

  sodium_memzero(session->device_secret_key,
                 sizeof(session->device_secret_key));
  sodium_memzero(session->session_secret_key,
                 sizeof(session->session_secret_key));
  sodium_memzero(session->shared_secret, sizeof(session->shared_secret));
  sodium_memzero(session->encrypt_key, sizeof(session->encrypt_key));
  sodium_memzero(session->decrypt_key, sizeof(session->decrypt_key));
  free(session);
}
