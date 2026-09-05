#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Wake the PC.  Two mechanisms, both attempted by pc_wake_trigger():
 *
 *   1. USB remote wakeup + HID nudge, when the PC is asleep (S3) and the
 *      USB device is enumerated and suspended.
 *   2. Wake-on-LAN magic packet to the configured MAC over UDP broadcast,
 *      which is the only thing that works from fully off (S5).
 *
 * Sending both is harmless: a running PC ignores a magic packet, and an
 * awake USB host ignores a resume.
 */

typedef struct {
  bool usb_attempted;
  bool usb_resume_sent;
  bool wol_attempted;
  bool wol_sent;
  char detail[96];
} pc_wake_result_t;

/** Parse "aa:bb:cc:dd:ee:ff" (or with '-' / no separators) into 6 bytes. */
bool pc_wake_parse_mac(const char *text, uint8_t mac[6]);

/** Format 6 bytes as "aa:bb:cc:dd:ee:ff" into a 18+ byte buffer. */
void pc_wake_format_mac(const uint8_t mac[6], char *out, size_t out_size);

/** Send the magic packet for `mac` (broadcast, 3 repeats). */
esp_err_t pc_wake_send_wol(const uint8_t mac[6]);

/** Run the full wake sequence using the configured MAC. */
esp_err_t pc_wake_trigger(pc_wake_result_t *result);
