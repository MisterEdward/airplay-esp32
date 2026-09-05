#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Byte-oriented log journal with independent reader cursors.
 *
 * A single ring buffer holds the most recent `capacity` bytes of log text.
 * Writers append; readers each keep their own 64-bit absolute cursor and
 * pull whatever has been written since.  Reading never consumes data, so a
 * WebSocket viewer, an HTTP download and a diagnostics dump can all walk the
 * same history at their own pace.  A reader that falls more than `capacity`
 * bytes behind is told how many bytes it missed and resumed at the oldest
 * retained byte.
 *
 * The struct carries no lock: the caller serialises access (the firmware
 * wraps it in a spinlock so ISR-context log calls stay cheap; the host tests
 * call it directly).
 */

typedef struct {
  char *data;
  size_t capacity;
  uint64_t end;     // absolute byte index one past the newest byte
  uint64_t boot_id; // arbitrary value so a reader can detect a restart
} log_journal_t;

void log_journal_init(log_journal_t *journal, char *storage, size_t capacity,
                      uint64_t boot_id);

/** Append `len` bytes, overwriting the oldest bytes when full. */
void log_journal_append(log_journal_t *journal, const char *data, size_t len);

/**
 * Copy up to `capacity` bytes newer than `*cursor` into `out`.
 * `*cursor` advances by the number of bytes returned.  If the cursor pointed
 * at data that has already been overwritten, `*missed` receives the number
 * of skipped bytes and the read continues from the oldest retained byte.
 * A cursor beyond the current end (e.g. from before a reboot) is reset to
 * the oldest retained byte.
 */
size_t log_journal_read(const log_journal_t *journal, uint64_t *cursor,
                        char *out, size_t capacity, uint64_t *missed);

/** Absolute index of the oldest byte still retained. */
uint64_t log_journal_oldest(const log_journal_t *journal);

/** Bytes currently retained. */
size_t log_journal_used(const log_journal_t *journal);
