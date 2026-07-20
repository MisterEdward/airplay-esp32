#include "rtsp_crypto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "sodium.h"

static const char *TAG = "rtsp_crypto";

// Send all data, handling partial sends
static int send_all(int socket, const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t r = send(socket, data + sent, len - sent, 0);
    if (r <= 0) {
      return -1;
    }
    sent += (size_t)r;
  }
  return 0;
}

int rtsp_crypto_read_block(int socket, rtsp_conn_t *conn, uint8_t *buffer,
                           size_t buffer_size) {
  if (!conn || !conn->hap_session || !conn->encrypted_mode) {
    // Expected during session teardown - not an error
    return -1;
  }

  // Read 2-byte length header (little-endian).  The socket has SO_RCVTIMEO
  // set for shutdown responsiveness; if it fires after a partial read we
  // must keep waiting rather than return — abandoning N consumed bytes here
  // permanently desyncs the encrypted stream framing and turns every later
  // recv into garbage interpreted as a fresh length prefix.  Cancellation
  // is handled by shutdown(SHUT_RDWR), which makes recv return a real error.
  uint8_t len_buf[2];
  size_t received = 0;
  while (received < 2) {
    ssize_t r = recv(socket, len_buf + received, 2 - received, 0);
    if (r > 0) {
      received += (size_t)r;
      continue;
    }
    if (r == 0) {
      return -1; // peer closed
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      continue;
    }
    return -1;
  }

  uint16_t block_len = (uint16_t)len_buf[0] | ((uint16_t)len_buf[1] << 8);

  if (block_len == 0 || block_len > RTSP_ENCRYPTED_BLOCK_MAX ||
      block_len > buffer_size) {
    ESP_LOGE(TAG, "Invalid encrypted block length: %d", block_len);
    return -1;
  }

  // Allocate temporary buffer for encrypted data
  size_t encrypted_len = block_len + 16; // +16 for Poly1305 tag
  uint8_t *encrypted = malloc(encrypted_len);
  if (!encrypted) {
    ESP_LOGE(TAG, "Failed to allocate encrypted buffer");
    return -1;
  }

  received = 0;
  while (received < encrypted_len) {
    ssize_t r = recv(socket, encrypted + received, encrypted_len - received, 0);
    if (r > 0) {
      received += (size_t)r;
      continue;
    }
    if (r == 0) {
      free(encrypted);
      return -1; // peer closed
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      continue;
    }
    free(encrypted);
    return -1;
  }

  // Decrypt using session keys
  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, &conn->hap_session->decrypt_nonce, 8);

  unsigned long long plaintext_len;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(
          buffer, &plaintext_len, NULL, encrypted, encrypted_len, len_buf,
          sizeof(len_buf), nonce, conn->hap_session->decrypt_key) != 0) {
    // Diagnostic fallback: retry once with the accessory's write key. The HAP
    // spec maps Control-Write-Encryption-Key to the accessory's read side, but
    // if that role mapping is inverted for this sender the frame decrypts
    // cleanly with the other key. Succeeding here identifies the real mapping
    // instead of leaving an unexplained authentication failure.
    // Candidates in order: swapped roles, then the 32-byte-IKM derivation,
    // then that derivation with swapped roles. Whichever authenticates is the
    // mapping this sender actually uses, so adopt it for the whole session.
    typedef struct {
      const char *name;
      const uint8_t *read_key;
      const uint8_t *write_key;
    } key_candidate_t;

    key_candidate_t candidates[3];
    size_t candidate_count = 0;
    candidates[candidate_count++] =
        (key_candidate_t){"swapped roles", conn->hap_session->encrypt_key,
                          conn->hap_session->decrypt_key};
    if (conn->hap_session->alt_keys_valid) {
      candidates[candidate_count++] =
          (key_candidate_t){"32-byte IKM", conn->hap_session->alt_decrypt_key,
                            conn->hap_session->alt_encrypt_key};
      candidates[candidate_count++] = (key_candidate_t){
          "32-byte IKM, swapped roles", conn->hap_session->alt_encrypt_key,
          conn->hap_session->alt_decrypt_key};
    }

    size_t hit = candidate_count;
    for (size_t i = 0; i < candidate_count; i++) {
      if (crypto_aead_chacha20poly1305_ietf_decrypt(
              buffer, &plaintext_len, NULL, encrypted, encrypted_len, len_buf,
              sizeof(len_buf), nonce, candidates[i].read_key) == 0) {
        hit = i;
        break;
      }
    }

    if (hit < candidate_count) {
      ESP_LOGW(TAG,
               "Decrypted using '%s' (block=%u nonce=%llu) — adopting for"
               " this session",
               candidates[hit].name, (unsigned)block_len,
               (unsigned long long)conn->hap_session->decrypt_nonce);
      uint8_t new_read[32];
      uint8_t new_write[32];
      memcpy(new_read, candidates[hit].read_key, 32);
      memcpy(new_write, candidates[hit].write_key, 32);
      memcpy(conn->hap_session->decrypt_key, new_read, 32);
      memcpy(conn->hap_session->encrypt_key, new_write, 32);
    } else {
      // Every candidate mapping failed. Dump just enough material to verify
      // the HKDF derivation off-device against a reference implementation.
      char keyhex[24];
      char altkeyhex[24];
      char cthex[24];
      for (int i = 0; i < 8; i++) {
        snprintf(keyhex + i * 2, 3, "%02x", conn->hap_session->decrypt_key[i]);
        snprintf(altkeyhex + i * 2, 3, "%02x",
                 conn->hap_session->alt_decrypt_key[i]);
        snprintf(cthex + i * 2, 3, "%02x", encrypted[i]);
      }
      ESP_LOGE(TAG,
               "Decrypt failed, all %u derivations rejected (block=%u"
               " nonce=%llu) key64=%s key32=%s ct=%s",
               (unsigned)(candidate_count + 1), (unsigned)block_len,
               (unsigned long long)conn->hap_session->decrypt_nonce, keyhex,
               altkeyhex, cthex);
      free(encrypted);
      return -1;
    }
  }
  free(encrypted);

  conn->hap_session->decrypt_nonce++;

  return (int)plaintext_len;
}

int rtsp_crypto_write_frame(int socket, rtsp_conn_t *conn, const uint8_t *data,
                            size_t data_len) {
  if (!conn || !conn->hap_session || !conn->encrypted_mode) {
    // Expected during session teardown - not an error
    return -1;
  }

  size_t offset = 0;
  while (offset < data_len) {
    uint16_t block_len = (data_len - offset) > RTSP_ENCRYPTED_BLOCK_MAX
                             ? RTSP_ENCRYPTED_BLOCK_MAX
                             : (uint16_t)(data_len - offset);

    uint8_t len_buf[2];
    len_buf[0] = block_len & 0xFF;
    len_buf[1] = (block_len >> 8) & 0xFF;

    uint8_t nonce[12] = {0};
    memcpy(nonce + 4, &conn->hap_session->encrypt_nonce, 8);

    size_t encrypted_len = block_len + 16; // +16 for Poly1305 tag
    uint8_t *encrypted = malloc(encrypted_len);
    if (!encrypted) {
      ESP_LOGE(TAG, "Failed to allocate encrypted buffer");
      return -1;
    }

    unsigned long long ct_len;
    crypto_aead_chacha20poly1305_ietf_encrypt(
        encrypted, &ct_len, data + offset, block_len, len_buf, sizeof(len_buf),
        NULL, nonce, conn->hap_session->encrypt_key);

    if (ct_len != encrypted_len) {
      ESP_LOGE(TAG, "Unexpected encrypted length: %llu", ct_len);
      free(encrypted);
      return -1;
    }

    if (send_all(socket, len_buf, sizeof(len_buf)) != 0 ||
        send_all(socket, encrypted, encrypted_len) != 0) {
      ESP_LOGE(TAG, "Failed to send encrypted block");
      free(encrypted);
      return -1;
    }

    free(encrypted);
    conn->hap_session->encrypt_nonce++;
    offset += block_len;
  }

  return 0;
}
