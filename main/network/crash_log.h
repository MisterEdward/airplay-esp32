#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Diagnostics that outlive a reset.
 *
 * The log journal lives in PSRAM and is rebuilt from scratch on every boot,
 * so the lines that matter most — the ones written just before a panic,
 * watchdog or brownout — are exactly the ones that are always lost.  This
 * module keeps a small tail of the journal in RTC memory, which the digital
 * core reset does not clear, and replays it into the new journal at startup
 * under a `previous boot` banner.
 *
 * It also tallies reset reasons across boots.  `esp_reset_reason()` only
 * reports the last one; a board that is browning out under load looks
 * identical to a board that rebooted once, until you watch it for a while.
 *
 * Caveat: RTC memory survives a reset, not a power cut.  After the rail
 * actually drops to zero the tail and the counters start over, which is
 * itself a signal — `boots` back at 1 means the board lost power.
 */

#define CRASH_LOG_REASONS 16

typedef struct {
  uint32_t boots;                        // boots since the counters were lost
  uint32_t by_reason[CRASH_LOG_REASONS]; // indexed by esp_reset_reason_t
  uint32_t abnormal;                     // panic, watchdogs and brownouts
  uint32_t since_abnormal;               // clean boots since the last one
  uint8_t last_abnormal;                 // esp_reset_reason_t, 0 if none yet
} crash_log_stats_t;

/**
 * Adopt whatever survived the last reset and start a fresh tail for this
 * boot.  Called by log_stream_init() before the journal exists.
 */
esp_err_t crash_log_init(void);

/** Mirror a journal line into the retained tail.  Cheap: one memcpy. */
void crash_log_write(const char *data, size_t len);

/**
 * The retained tail from the previous boot, or NULL.  Valid until
 * crash_log_forget_previous(); the text is NUL-terminated.
 */
const char *crash_log_previous(size_t *len);

/** Release the retained tail once it has been copied somewhere useful. */
void crash_log_forget_previous(void);

/** Boot and reset-reason tally.  Never fails; zeroed when unavailable. */
void crash_log_get_stats(crash_log_stats_t *out);

/** Human-readable name for an esp_reset_reason_t value. */
const char *crash_log_reason_name(int reason);
