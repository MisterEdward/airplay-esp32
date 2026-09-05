#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#include <stddef.h>
#include <stdint.h>

/**
 * Wireless diagnostics: every ESP log line goes to UART (unchanged) AND into a
 * PSRAM journal that outlives the browser tab.  Exposed as:
 *
 *   ws://<ip>/ws/logs             live stream, each new viewer first gets the
 *                                 recent backlog so a page refresh loses nothing
 *   GET  /api/logs?after=<cursor> incremental pull (headers X-Log-Cursor,
 *                                 X-Log-Missed, X-Log-Boot)
 *   GET  /api/logs/download       the whole retained journal as text/plain
 *   GET  /api/logs/level          current runtime log levels
 *   POST /api/logs/level          {"tag":"audio_time","level":"debug"}
 *
 * The journal is ~192 KB: at the usual ~150 bytes/line that is ~20 minutes of
 * INFO-level playout reports, or a few minutes at DEBUG.
 */

/** Install the log hook and allocate the journal.  Call before WiFi init. */
esp_err_t log_stream_init(void);

/** Register the WebSocket endpoint and the HTTP API on `server`. */
esp_err_t log_stream_register(httpd_handle_t server);

/** Random-per-boot identifier so a viewer can detect a restart. */
uint64_t log_stream_boot_id(void);

/** Bytes currently retained in the journal. */
size_t log_stream_journal_used(void);

/** Lines that had to be truncated to fit the formatting buffer. */
uint32_t log_stream_truncated_lines(void);
