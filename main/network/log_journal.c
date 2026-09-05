#include "log_journal.h"

#include <string.h>

void log_journal_init(log_journal_t *journal, char *storage, size_t capacity,
                      uint64_t boot_id) {
  journal->data = storage;
  journal->capacity = capacity;
  journal->end = 0;
  journal->boot_id = boot_id;
}

uint64_t log_journal_oldest(const log_journal_t *journal) {
  return journal->end > journal->capacity ? journal->end - journal->capacity
                                          : 0;
}

size_t log_journal_used(const log_journal_t *journal) {
  return journal->end > journal->capacity ? journal->capacity
                                          : (size_t)journal->end;
}

void log_journal_append(log_journal_t *journal, const char *data, size_t len) {
  if (!journal->data || journal->capacity == 0 || len == 0) {
    return;
  }
  // Only the last `capacity` bytes of an oversized append can survive.
  if (len > journal->capacity) {
    data += len - journal->capacity;
    journal->end += len - journal->capacity;
    len = journal->capacity;
  }
  size_t pos = (size_t)(journal->end % journal->capacity);
  size_t first = journal->capacity - pos;
  if (first > len) {
    first = len;
  }
  memcpy(journal->data + pos, data, first);
  if (len > first) {
    memcpy(journal->data, data + first, len - first);
  }
  journal->end += len;
}

size_t log_journal_read(const log_journal_t *journal, uint64_t *cursor,
                        char *out, size_t capacity, uint64_t *missed) {
  uint64_t oldest = log_journal_oldest(journal);
  if (missed) {
    *missed = 0;
  }
  if (!journal->data || journal->capacity == 0 || capacity == 0) {
    return 0;
  }
  if (*cursor > journal->end) {
    // Stale cursor from a previous boot: restart from the oldest byte.
    *cursor = oldest;
  } else if (*cursor < oldest) {
    if (missed) {
      *missed = oldest - *cursor;
    }
    *cursor = oldest;
  }
  uint64_t available = journal->end - *cursor;
  size_t count = available < capacity ? (size_t)available : capacity;
  size_t pos = (size_t)(*cursor % journal->capacity);
  size_t first = journal->capacity - pos;
  if (first > count) {
    first = count;
  }
  memcpy(out, journal->data + pos, first);
  if (count > first) {
    memcpy(out + first, journal->data, count - first);
  }
  *cursor += count;
  return count;
}
