#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define HAP_PAIRINGS_MAX   8
#define HAP_PAIRING_ID_MAX 64

/**
 * Remember a controller's long-term public key (pair-setup M5).
 * Replaces an existing entry with the same pairing identifier.
 */
esp_err_t hap_pairings_add(const uint8_t *id, size_t id_len,
                           const uint8_t ltpk[32]);

/**
 * Look up a controller's long-term public key (pair-verify M3).
 */
bool hap_pairings_find(const uint8_t *id, size_t id_len, uint8_t ltpk[32]);
