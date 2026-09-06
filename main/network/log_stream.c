/**
 * Wireless log capture: ESP-IDF log hook → PSRAM journal → WebSocket / HTTP.
 *
 * Design notes
 * ------------
 *  - The hook runs in the context of whoever calls ESP_LOGx, including the
 *    audio playout task.  It must be cheap and must never block: format into
 *    a stack buffer, take a spinlock for the memcpy into the ring, done.
 *  - The journal (log_journal.c) keeps absolute 64-bit byte cursors, so each
 *    WebSocket client and each HTTP poller reads independently.  Nothing is
 *    consumed by reading; closing the page loses nothing.
 *  - va_list handling: the original v0.2.0 hook passed `args` to the UART
 *    vprintf and then a va_copy to vsnprintf.  Passing a va_list to a
 *    function consumes it on Xtensa, so copies must be made BEFORE either
 *    consumer runs.  Symptom of the bug: WebSocket lines whose arguments were
 *    garbage while the UART line was fine.
 */

#include "log_stream.h"
#include "log_journal.h"
#include "spiram_task.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_JOURNAL_SIZE (192 * 1024)
#define LOG_LINE_MAX     384

#define BROADCAST_TASK_STACK  4096
#define BROADCAST_INTERVAL_MS 100
#define MAX_SEND_CHUNK        1024
// How much history a freshly connected WebSocket viewer receives.
#define WS_BACKLOG_BYTES (16 * 1024)
// Per-poll cap for GET /api/logs.
#define HTTP_PULL_MAX (8 * 1024)

static const char *TAG = "log_stream";

static log_journal_t s_journal;
static portMUX_TYPE s_journal_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_truncated_lines;
static uint64_t s_boot_id;

static httpd_handle_t s_server;
#if !CONFIG_ESP_CONSOLE_NONE || !CONFIG_ESP_CONSOLE_SECONDARY_NONE
static vprintf_like_t s_orig_vprintf;
#endif

/* Per-WebSocket-client cursors.  Indexed by socket fd; a fd that vanishes
 * from httpd's client list is forgotten, so a reused fd starts fresh. */
typedef struct {
  int fd;
  uint64_t cursor;
  bool active;
} ws_client_t;
static ws_client_t s_ws_clients[CONFIG_LWIP_MAX_SOCKETS];

/* ------------------------------------------------------------------ */
/*  Journal access                                                     */
/* ------------------------------------------------------------------ */

static void journal_append_locked(const char *data, size_t len) {
  portENTER_CRITICAL(&s_journal_lock);
  log_journal_append(&s_journal, data, len);
  portEXIT_CRITICAL(&s_journal_lock);
}

static size_t journal_read_locked(uint64_t *cursor, char *out, size_t cap,
                                  uint64_t *missed) {
  portENTER_CRITICAL(&s_journal_lock);
  size_t n = log_journal_read(&s_journal, cursor, out, cap, missed);
  portEXIT_CRITICAL(&s_journal_lock);
  return n;
}

static uint64_t journal_end(void) {
  portENTER_CRITICAL(&s_journal_lock);
  uint64_t end = s_journal.end;
  portEXIT_CRITICAL(&s_journal_lock);
  return end;
}

/* ------------------------------------------------------------------ */
/*  Log hook                                                           */
/* ------------------------------------------------------------------ */

// Tags that log on every socket send.  With a WebSocket viewer attached,
// each journal line pushed to the browser makes httpd/event emit ~4 DEBUG
// lines of their own, which land in the journal, which get pushed... The
// feedback loop saturates the httpd task within seconds (observed: HTTP
// dead, ping alive).  These stay at INFO whatever the viewer asks for.
static const char *const s_pinned_tags[] = {
    "httpd_txrx", "httpd_parse", "httpd_sess", "httpd_uri",
    "httpd_ws",   "httpd",       "event",      "esp-tls",
};

static void pin_noisy_tags(void) {
  for (size_t i = 0; i < sizeof(s_pinned_tags) / sizeof(s_pinned_tags[0]);
       i++) {
    esp_log_level_set(s_pinned_tags[i], ESP_LOG_INFO);
  }
}

static int log_vprintf_hook(const char *fmt, va_list args) {
  va_list journal_args;
  va_copy(journal_args, args);

#if !CONFIG_ESP_CONSOLE_NONE || !CONFIG_ESP_CONSOLE_SECONDARY_NONE
  va_list uart_args;
  va_copy(uart_args, args);
  int ret = s_orig_vprintf ? s_orig_vprintf(fmt, uart_args) : 0;
  va_end(uart_args);
#endif

  char buf[LOG_LINE_MAX];
  int len = vsnprintf(buf, sizeof(buf), fmt, journal_args);
  va_end(journal_args);
#if CONFIG_ESP_CONSOLE_NONE && CONFIG_ESP_CONSOLE_SECONDARY_NONE
  // Preserve printf's would-have-written count, before journal truncation.
  int ret = len;
#endif

  if (len > 0) {
    if ((size_t)len >= sizeof(buf)) {
      // Keep the line terminated so the viewer's line splitter stays sane.
      len = sizeof(buf) - 1;
      buf[len - 1] = '\n';
      s_truncated_lines++;
    }
    journal_append_locked(buf, (size_t)len);
  }
  return ret;
}

/* ------------------------------------------------------------------ */
/*  WebSocket                                                          */
/* ------------------------------------------------------------------ */

static esp_err_t ws_log_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    // Handshake completed by httpd.  Register the client so the broadcast
    // task starts it from a backlog instead of from "now".
    int fd = httpd_req_to_sockfd(req);
    uint64_t end = journal_end();
    for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS; i++) {
      if (!s_ws_clients[i].active) {
        s_ws_clients[i].fd = fd;
        s_ws_clients[i].cursor =
            end > WS_BACKLOG_BYTES ? end - WS_BACKLOG_BYTES : 0;
        s_ws_clients[i].active = true;
        break;
      }
    }
    return ESP_OK;
  }

  /* Drain any frame the browser sends (CLOSE, ping) so framing stays
   * aligned; the viewer never sends commands over the socket. */
  httpd_ws_frame_t frame = {0};
  if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) {
    return ESP_OK;
  }
  if (frame.len > 0) {
    uint8_t buf[128];
    if (frame.len <= sizeof(buf)) {
      frame.payload = buf;
      httpd_ws_recv_frame(req, &frame, sizeof(buf));
    }
  }
  return ESP_OK;
}

static ws_client_t *ws_client_for_fd(int fd) {
  for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS; i++) {
    if (s_ws_clients[i].active && s_ws_clients[i].fd == fd) {
      return &s_ws_clients[i];
    }
  }
  return NULL;
}

static void broadcast_task(void *arg) {
  (void)arg;
  char *buf = malloc(MAX_SEND_CHUNK);
  if (!buf) {
    vTaskDelete(NULL);
    return;
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(BROADCAST_INTERVAL_MS));

    int fds[CONFIG_LWIP_MAX_SOCKETS];
    size_t fd_count = CONFIG_LWIP_MAX_SOCKETS;
    if (!s_server ||
        httpd_get_client_list(s_server, &fd_count, fds) != ESP_OK) {
      continue;
    }

    // Forget clients whose fd is no longer a WebSocket session.
    for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS; i++) {
      if (!s_ws_clients[i].active) {
        continue;
      }
      bool present = false;
      for (size_t f = 0; f < fd_count; f++) {
        if (fds[f] == s_ws_clients[i].fd &&
            httpd_ws_get_fd_info(s_server, fds[f]) ==
                HTTPD_WS_CLIENT_WEBSOCKET) {
          present = true;
          break;
        }
      }
      if (!present) {
        s_ws_clients[i].active = false;
      }
    }

    for (size_t f = 0; f < fd_count; f++) {
      if (httpd_ws_get_fd_info(s_server, fds[f]) != HTTPD_WS_CLIENT_WEBSOCKET) {
        continue;
      }
      ws_client_t *client = ws_client_for_fd(fds[f]);
      if (!client) {
        // A socket upgraded before we saw its GET (should not happen, but be
        // safe): start it from the backlog.
        uint64_t end = journal_end();
        for (size_t i = 0; i < CONFIG_LWIP_MAX_SOCKETS; i++) {
          if (!s_ws_clients[i].active) {
            client = &s_ws_clients[i];
            client->fd = fds[f];
            client->cursor =
                end > WS_BACKLOG_BYTES ? end - WS_BACKLOG_BYTES : 0;
            client->active = true;
            break;
          }
        }
        if (!client) {
          continue;
        }
      }
      // Send up to a few chunks per tick so a backlog drains quickly while a
      // slow client cannot monopolise the task.
      for (int chunk = 0; chunk < 8; chunk++) {
        uint64_t missed = 0;
        size_t len =
            journal_read_locked(&client->cursor, buf, MAX_SEND_CHUNK, &missed);
        if (len == 0) {
          break;
        }
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)buf,
            .len = len,
        };
        if (httpd_ws_send_frame_async(s_server, client->fd, &frame) != ESP_OK) {
          client->active = false;
          break;
        }
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/*  HTTP API                                                           */
/* ------------------------------------------------------------------ */

static esp_err_t logs_pull_handler(httpd_req_t *req) {
  uint64_t cursor = 0;
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[24];
    if (httpd_query_key_value(query, "after", val, sizeof(val)) == ESP_OK) {
      cursor = strtoull(val, NULL, 10);
    }
  }

  char *buf = heap_caps_malloc(HTTP_PULL_MAX, MALLOC_CAP_SPIRAM);
  if (!buf) {
    buf = malloc(HTTP_PULL_MAX);
  }
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    return ESP_FAIL;
  }
  uint64_t missed = 0;
  size_t len = journal_read_locked(&cursor, buf, HTTP_PULL_MAX, &missed);

  char hdr[32];
  snprintf(hdr, sizeof(hdr), "%llu", (unsigned long long)cursor);
  httpd_resp_set_hdr(req, "X-Log-Cursor", hdr);
  char hdr2[32];
  snprintf(hdr2, sizeof(hdr2), "%llu", (unsigned long long)missed);
  httpd_resp_set_hdr(req, "X-Log-Missed", hdr2);
  char hdr3[32];
  snprintf(hdr3, sizeof(hdr3), "%016llx", (unsigned long long)s_boot_id);
  httpd_resp_set_hdr(req, "X-Log-Boot", hdr3);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  esp_err_t err = httpd_resp_send(req, buf, (ssize_t)len);
  free(buf);
  return err;
}

static esp_err_t logs_download_handler(httpd_req_t *req) {
  char *buf = malloc(MAX_SEND_CHUNK);
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    return ESP_FAIL;
  }
  char disp[80];
  snprintf(disp, sizeof(disp), "attachment; filename=\"fable-%08llx.log\"",
           (unsigned long long)(s_boot_id & 0xFFFFFFFFULL));
  httpd_resp_set_hdr(req, "Content-Disposition", disp);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_type(req, "text/plain; charset=utf-8");

  int n = snprintf(buf, MAX_SEND_CHUNK,
                   "# fable-5.1 log journal boot=%016llx retained=%u bytes "
                   "truncated_lines=%" PRIu32 "\n",
                   (unsigned long long)s_boot_id,
                   (unsigned)log_stream_journal_used(), s_truncated_lines);
  esp_err_t err = httpd_resp_send_chunk(req, buf, n);

  uint64_t cursor = 0; // journal_read resets a too-old cursor to the oldest
  uint64_t missed = 0;
  while (err == ESP_OK) {
    size_t len = journal_read_locked(&cursor, buf, MAX_SEND_CHUNK, &missed);
    if (len == 0) {
      break;
    }
    err = httpd_resp_send_chunk(req, buf, (ssize_t)len);
  }
  httpd_resp_send_chunk(req, NULL, 0);
  free(buf);
  return err;
}

static const char *level_name(esp_log_level_t level) {
  switch (level) {
  case ESP_LOG_NONE:
    return "none";
  case ESP_LOG_ERROR:
    return "error";
  case ESP_LOG_WARN:
    return "warn";
  case ESP_LOG_INFO:
    return "info";
  case ESP_LOG_DEBUG:
    return "debug";
  case ESP_LOG_VERBOSE:
    return "verbose";
  default:
    return "?";
  }
}

static bool level_from_name(const char *name, esp_log_level_t *level) {
  static const struct {
    const char *name;
    esp_log_level_t level;
  } map[] = {
      {"none", ESP_LOG_NONE},   {"error", ESP_LOG_ERROR},
      {"warn", ESP_LOG_WARN},   {"info", ESP_LOG_INFO},
      {"debug", ESP_LOG_DEBUG}, {"verbose", ESP_LOG_VERBOSE},
  };
  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
    if (strcasecmp(map[i].name, name) == 0) {
      *level = map[i].level;
      return true;
    }
  }
  return false;
}

// Tags a diagnostician is most likely to raise to DEBUG.  Listed so the
// WebUI can offer them without the user having to remember the strings.
static const char *const s_known_tags[] = {
    "*",           "audio_time",    "audio_recv", "audio_buf", "audio_output",
    "rtsp_server", "rtsp_handlers", "ptp_clock",  "usb_audio", "pc_wake",
    "source_vol",  "wifi",          "log_stream",
};

static esp_err_t logs_level_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON *tags = cJSON_CreateArray();
  for (size_t i = 0; i < sizeof(s_known_tags) / sizeof(s_known_tags[0]); i++) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "tag", s_known_tags[i]);
    cJSON_AddStringToObject(t, "level",
                            level_name(esp_log_level_get(s_known_tags[i])));
    cJSON_AddItemToArray(tags, t);
  }
  cJSON_AddItemToObject(json, "tags", tags);
  cJSON_AddStringToObject(
      json, "max_compiled",
      level_name((esp_log_level_t)CONFIG_LOG_MAXIMUM_LEVEL));
  cJSON_AddNumberToObject(json, "journal_bytes",
                          (double)log_stream_journal_used());
  cJSON_AddNumberToObject(json, "truncated_lines", (double)s_truncated_lines);
  cJSON_AddBoolToObject(json, "success", true);
  char *out = cJSON_PrintUnformatted(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
  free(out);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t logs_level_post_handler(httpd_req_t *req) {
  char body[160];
  int len = httpd_req_recv(req, body, sizeof(body) - 1);
  if (len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    return ESP_FAIL;
  }
  body[len] = '\0';
  cJSON *json = cJSON_Parse(body);
  const cJSON *tag = json ? cJSON_GetObjectItem(json, "tag") : NULL;
  const cJSON *lvl = json ? cJSON_GetObjectItem(json, "level") : NULL;
  esp_log_level_t level;
  if (!cJSON_IsString(tag) || !cJSON_IsString(lvl) ||
      !level_from_name(lvl->valuestring, &level) ||
      strlen(tag->valuestring) == 0 || strlen(tag->valuestring) > 31) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected {tag,level}");
    return ESP_FAIL;
  }
  if (level > (esp_log_level_t)CONFIG_LOG_MAXIMUM_LEVEL) {
    level = (esp_log_level_t)CONFIG_LOG_MAXIMUM_LEVEL;
  }
  esp_log_level_set(tag->valuestring, level);
  pin_noisy_tags(); // '*' would otherwise drag httpd/event up with it
  ESP_LOGI(TAG, "Log level for '%s' set to %s", tag->valuestring,
           level_name(level));
  cJSON_Delete(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t log_stream_init(void) {
  char *storage = NULL;
#ifdef CONFIG_SPIRAM
  storage =
      heap_caps_malloc(LOG_JOURNAL_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  size_t size = LOG_JOURNAL_SIZE;
  if (!storage) {
    size = 16 * 1024;
    storage = malloc(size);
  }
  if (!storage) {
    return ESP_ERR_NO_MEM;
  }
  s_boot_id = ((uint64_t)esp_random() << 32) | esp_random();
  log_journal_init(&s_journal, storage, size, s_boot_id);
#if !CONFIG_ESP_CONSOLE_NONE || !CONFIG_ESP_CONSOLE_SECONDARY_NONE
  s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);
#else
  esp_log_set_vprintf(log_vprintf_hook);
#endif
  pin_noisy_tags();
  ESP_LOGI(TAG, "Journal %u bytes, boot=%016llx", (unsigned)size,
           (unsigned long long)s_boot_id);
  return ESP_OK;
}

esp_err_t log_stream_register(httpd_handle_t server) {
  s_server = server;

  httpd_uri_t ws_uri = {
      .uri = "/ws/logs",
      .method = HTTP_GET,
      .handler = ws_log_handler,
      .is_websocket = true,
  };
  esp_err_t err = httpd_register_uri_handler(server, &ws_uri);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register /ws/logs: %s", esp_err_to_name(err));
    return err;
  }
  httpd_uri_t pull = {
      .uri = "/api/logs", .method = HTTP_GET, .handler = logs_pull_handler};
  httpd_register_uri_handler(server, &pull);
  httpd_uri_t dl = {.uri = "/api/logs/download",
                    .method = HTTP_GET,
                    .handler = logs_download_handler};
  httpd_register_uri_handler(server, &dl);
  httpd_uri_t lvl_get = {.uri = "/api/logs/level",
                         .method = HTTP_GET,
                         .handler = logs_level_get_handler};
  httpd_register_uri_handler(server, &lvl_get);
  httpd_uri_t lvl_post = {.uri = "/api/logs/level",
                          .method = HTTP_POST,
                          .handler = logs_level_post_handler};
  httpd_register_uri_handler(server, &lvl_post);

  task_create_spiram(broadcast_task, "log_ws", BROADCAST_TASK_STACK, NULL, 3,
                     NULL, NULL);
  ESP_LOGI(TAG, "Log streaming on /ws/logs, journal API on /api/logs");
  return ESP_OK;
}

uint64_t log_stream_boot_id(void) {
  return s_boot_id;
}

size_t log_stream_journal_used(void) {
  portENTER_CRITICAL(&s_journal_lock);
  size_t used = log_journal_used(&s_journal);
  portEXIT_CRITICAL(&s_journal_lock);
  return used;
}

uint32_t log_stream_truncated_lines(void) {
  return s_truncated_lines;
}
