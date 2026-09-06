#include "crash_log.h"

#include "esp_attr.h"
#include "esp_system.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Bumped whenever the retained layout changes, so an image with a different
// struct does not read the previous image's bytes as its own.
#define CRASH_LOG_MAGIC 0xFAB1E002u

// RTC slow memory is 8 KiB in total and shared with the system, so the tail
// is deliberately small.  At the usual ~150 bytes per line this holds the
// last twenty or so lines, which is what a post-mortem actually needs.
#define CRASH_LOG_BYTES 3072

typedef struct {
  uint32_t magic;
  uint32_t boots;
  uint32_t by_reason[CRASH_LOG_REASONS];
  uint32_t abnormal;
  uint32_t since_abnormal;
  uint32_t last_abnormal;
  uint32_t head;   // next write offset into text[]
  uint32_t filled; // bytes ever written, capped at CRASH_LOG_BYTES
  char text[CRASH_LOG_BYTES];
} crash_ring_t;

// Uninitialised by the startup code on purpose: whatever the previous boot
// left here is exactly what we came for.
static RTC_NOINIT_ATTR crash_ring_t s_ring;

static char *s_prev;
static size_t s_prev_len;

static bool reason_is_abnormal(esp_reset_reason_t r) {
  switch (r) {
  case ESP_RST_PANIC:
  case ESP_RST_INT_WDT:
  case ESP_RST_TASK_WDT:
  case ESP_RST_WDT:
  case ESP_RST_BROWNOUT:
    return true;
  default:
    return false;
  }
}

static bool ring_is_sane(void) {
  return s_ring.magic == CRASH_LOG_MAGIC && s_ring.head <= CRASH_LOG_BYTES &&
         s_ring.filled <= CRASH_LOG_BYTES;
}

/**
 * Copy the ring out in write order.  A wrapped ring starts mid-line, so the
 * copy begins after the first newline: a truncated first line reads as
 * corruption to anyone looking at the journal later.
 */
static void adopt_previous_tail(void) {
  if (s_ring.filled == 0) {
    return;
  }

  char *buf = malloc(s_ring.filled + 1);
  if (!buf) {
    return;
  }

  size_t n = 0;
  if (s_ring.filled < CRASH_LOG_BYTES) {
    memcpy(buf, s_ring.text, s_ring.filled);
    n = s_ring.filled;
  } else {
    size_t tail = CRASH_LOG_BYTES - s_ring.head;
    memcpy(buf, s_ring.text + s_ring.head, tail);
    memcpy(buf + tail, s_ring.text, s_ring.head);
    n = CRASH_LOG_BYTES;
    // Drop the partial line the wrap left at the front.
    char *nl = memchr(buf, '\n', n);
    if (nl && (size_t)(nl - buf) + 1 < n) {
      size_t skip = (size_t)(nl - buf) + 1;
      memmove(buf, buf + skip, n - skip);
      n -= skip;
    }
  }

  buf[n] = '\0';
  s_prev = buf;
  s_prev_len = n;
}

esp_err_t crash_log_init(void) {
  const esp_reset_reason_t reason = esp_reset_reason();
  const bool sane = ring_is_sane();

  // A power-on clears the RTC domain in practice; anything found there is
  // stale bytes rather than the previous boot's words.
  if (sane && reason != ESP_RST_POWERON) {
    adopt_previous_tail();
  }

  if (!sane) {
    memset(&s_ring, 0, sizeof(s_ring));
    s_ring.magic = CRASH_LOG_MAGIC;
  }

  s_ring.boots++;
  if (reason < CRASH_LOG_REASONS) {
    s_ring.by_reason[reason]++;
  }
  if (reason_is_abnormal(reason)) {
    s_ring.abnormal++;
    s_ring.since_abnormal = 0;
    s_ring.last_abnormal = (uint32_t)reason;
  } else {
    s_ring.since_abnormal++;
  }

  // Start this boot's tail empty so the retained text is never a mixture of
  // two boots.
  s_ring.head = 0;
  s_ring.filled = 0;
  return ESP_OK;
}

void crash_log_write(const char *data, size_t len) {
  if (!data || len == 0 || s_ring.magic != CRASH_LOG_MAGIC) {
    return;
  }

  // A line longer than the ring can only contribute its own tail.
  if (len > CRASH_LOG_BYTES) {
    data += len - CRASH_LOG_BYTES;
    len = CRASH_LOG_BYTES;
  }

  size_t first = CRASH_LOG_BYTES - s_ring.head;
  if (first > len) {
    first = len;
  }
  memcpy(s_ring.text + s_ring.head, data, first);
  if (len > first) {
    memcpy(s_ring.text, data + first, len - first);
  }

  s_ring.head = (uint32_t)((s_ring.head + len) % CRASH_LOG_BYTES);
  if (s_ring.filled < CRASH_LOG_BYTES) {
    size_t filled = s_ring.filled + len;
    s_ring.filled =
        (uint32_t)(filled > CRASH_LOG_BYTES ? CRASH_LOG_BYTES : filled);
  }
}

const char *crash_log_previous(size_t *len) {
  if (len) {
    *len = s_prev_len;
  }
  return s_prev;
}

void crash_log_forget_previous(void) {
  free(s_prev);
  s_prev = NULL;
  s_prev_len = 0;
}

void crash_log_get_stats(crash_log_stats_t *out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (s_ring.magic != CRASH_LOG_MAGIC) {
    return;
  }
  out->boots = s_ring.boots;
  memcpy(out->by_reason, s_ring.by_reason, sizeof(out->by_reason));
  out->abnormal = s_ring.abnormal;
  out->since_abnormal = s_ring.since_abnormal;
  out->last_abnormal = (uint8_t)s_ring.last_abnormal;
}

const char *crash_log_reason_name(int reason) {
  switch (reason) {
  case ESP_RST_POWERON:
    return "poweron";
  case ESP_RST_EXT:
    return "external";
  case ESP_RST_SW:
    return "software";
  case ESP_RST_PANIC:
    return "panic";
  case ESP_RST_INT_WDT:
    return "int_wdt";
  case ESP_RST_TASK_WDT:
    return "task_wdt";
  case ESP_RST_WDT:
    return "other_wdt";
  case ESP_RST_DEEPSLEEP:
    return "deepsleep";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_SDIO:
    return "sdio";
  default:
    return "unknown";
  }
}
