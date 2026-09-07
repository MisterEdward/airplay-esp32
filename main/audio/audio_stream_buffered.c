/**
 * AirPlay 2 buffered stream (type 103): AAC over TCP.
 *
 * Two tasks instead of v0.2.0's one:
 *
 *   reader  (prio 5)   recv() framed packets → PSRAM slot → ready queue
 *   decoder (prio 6)   ready queue → RTP gate → decrypt → AAC decode → PCM ring
 *
 * Why split: a single task that both blocks on the socket and spends ~2 ms
 * decoding a frame serialises the two.  During a seek the TCP backlog (up to
 * a TCP window of old-position audio) had to be pulled through the decoder
 * before anything new could start, and a decoder stall (PCM ring full during
 * pause) stopped the socket being drained, so the sender's window closed and
 * its first post-seek burst was delayed.  With the queue in between, the
 * reader keeps the socket drained at line rate while the decoder can wait
 * for an anchor or for ring space without touching TCP.
 *
 * Seek semantics (see buffered_generation in audio_receiver_internal.h):
 * packets queued before FLUSHBUFFERED are dropped by generation; packets
 * that arrive between FLUSHBUFFERED and SETRATEANCHORTIME are HELD in the
 * queue and released to the RTP gates once the anchor is known.  v0.2.0
 * discarded them (discard_all_until_anchor), which cost ~0.5 s per seek.
 *
 * Idle handling: buffered senders transmit in bursts and may stay quiet
 * even with an empty PCM ring. A receive timeout never closes a live
 * session's data socket; only peer EOF, a real error or stop does.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio_receiver_internal.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "audio_crypto.h"
#include "network/socket_utils.h"

#define BUFFERED_AUDIO_PACKET_SIZE 8192
#define AUDIO_BUFFERED_STACK_SIZE  4096
#define AUDIO_DECODER_STACK_SIZE   6144
// Slot payload: AAC-LC at Apple Music's 256 kbps is ~750 bytes per 1024
// samples; 2048 leaves headroom for ALAC-over-TCP or higher rates.
//
// Slot count: after FLUSHBUFFERED the decoder holds every packet until the
// new anchor arrives.  The sender does not anchor until its whole post-flush
// burst has been written to us, and that burst is its full lead (~20 s), so
// the hold has to be able to swallow it.  900 slots ~ 20.9 s of AAC, ~1.9 MB
// PSRAM.  Whatever does not fit is dropped at the READER, never waited on:
// closing the TCP window while waiting for an anchor that only comes after
// the burst is a deadlock, and it cost a 100 s silence followed by the phone
// tearing the session down.
#define BUFFERED_SLOT_PAYLOAD    2048
#define BUFFERED_SLOT_COUNT      900
#define BUFFERED_STALL_TIMEOUT_S 8
#define BUFFERED_HOLD_POLL_MS    2
// Give up waiting for SETRATEANCHORTIME this long after a seek flush and play
// what we hold unscheduled.  Some senders never anchor at all (observed after
// PAUSE + immediate FLUSHBUFFERED on a track change); silence forever is the
// worst possible answer.
#define BUFFERED_ANCHOR_WAIT_US (10 * 1000 * 1000)

#if CONFIG_FREERTOS_UNICORE
#define BUFFERED_DECODER_CORE 0
#else
#define BUFFERED_DECODER_CORE 1
#endif

typedef struct {
  uint32_t seq_no;
  uint32_t timestamp;
  uint32_t generation;
  uint16_t packet_len;
  int64_t received_us;
  uint8_t data[BUFFERED_SLOT_PAYLOAD];
} buffered_slot_t;

static const char *TAG = "audio_buf";

static inline buffered_slot_t *slot_at(audio_receiver_state_t *state,
                                       uint16_t index) {
  return &((buffered_slot_t *)state->buffered_packet_pool)[index];
}

// Preserve partial framing across receive timeouts. stream->running stays
// true for the RTSP session, including pauses; stop shuts down the socket.
// Returns bytes read, 0 = peer closed, -1 = real error or stopped.
static ssize_t read_exact(audio_stream_t *stream, audio_receiver_state_t *state,
                          int sock, uint8_t *buf, size_t len) {
  size_t total = 0;
  int64_t started_us = esp_timer_get_time();
  int64_t last_warn_us = started_us;
  while (total < len && stream->running) {
    ssize_t n = recv(sock, buf + total, len - total, 0);
    if (n > 0) {
      total += (size_t)n;
    } else if (n == 0) {
      ESP_LOGI(TAG, "Buffered audio connection closed by peer");
      return 0;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (!state->timing.playing) {
          vTaskDelay(pdMS_TO_TICKS(100));
          continue;
        }
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_warn_us >= BUFFERED_STALL_TIMEOUT_S * 1000000LL) {
          last_warn_us = now_us;
          state->buffered_stall_timeouts++;
          ESP_LOGW(
              TAG,
              "Buffered audio idle while playing: waited=%lld ms "
              "last_packet=%lld ms ago partial=%u/%u buffered=%d; "
              "keeping data connection open",
              (long long)((now_us - started_us) / 1000LL),
              (long long)((now_us - state->buffered_last_packet_us) / 1000LL),
              (unsigned)total, (unsigned)len,
              audio_buffer_get_frame_count(&state->buffer));
        }
        continue;
      }
      if (stream->running) {
        ESP_LOGE(TAG, "Buffered audio recv error: %d", errno);
      }
      return -1;
    }
  }
  return stream->running ? (ssize_t)total : -1;
}

/* ------------------------------------------------------------------ */
/*  Decoder task                                                       */
/* ------------------------------------------------------------------ */

static void buffered_decoder_task(void *pvParameters) {
  audio_stream_t *stream = (audio_stream_t *)pvParameters;
  audio_receiver_state_t *state = audio_stream_state(stream);
  uint32_t logged_generation = UINT32_MAX;

  while (stream->running) {
    uint16_t index = 0;
    if (xQueueReceive(state->buffered_ready_queue, &index, pdMS_TO_TICKS(20)) !=
        pdTRUE) {
      continue;
    }
    buffered_slot_t *slot = slot_at(state, index);

    // Pre-seek packet: belongs to the old position.  Drop.
    if (slot->generation != state->buffered_generation) {
      state->buffered_generation_drops++;
      state->stats.packets_dropped++;
      xQueueSend(state->buffered_free_queue, &index, 0);
      continue;
    }

    // Post-seek, pre-anchor packet: hold it (compressed, cheap) until the
    // anchor arrives so the RTP gates can judge it.  Abandon the hold if a
    // newer seek supersedes this generation.
    bool held = false;
    while (stream->running && state->discard_all_until_anchor &&
           slot->generation == state->buffered_generation) {
      if (!held) {
        held = true;
        state->buffered_held_packets++;
      }
      int64_t waited_us = esp_timer_get_time() - state->buffered_flush_us;
      if (state->buffered_flush_us && waited_us > BUFFERED_ANCHOR_WAIT_US) {
        // No anchor is coming.  Release the hold and play what we have: the
        // oldest held packets are the ones the sender started the new
        // segment with, so this is the right audio, only unscheduled until
        // an anchor (if any) arrives and the engine re-acquires under it.
        state->discard_all_until_anchor = false;
        state->timing.quick_start = true;
        ESP_LOGW(TAG,
                 "No anchor %lld ms after the flush: playing the held "
                 "segment unscheduled (rtp=%" PRIu32 ")",
                 (long long)(waited_us / 1000LL), slot->timestamp);
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(BUFFERED_HOLD_POLL_MS));
    }
    if (!stream->running || slot->generation != state->buffered_generation) {
      state->buffered_generation_drops++;
      xQueueSend(state->buffered_free_queue, &index, 0);
      continue;
    }

    // Back-pressure: the PCM ring is deep (~7 s); when it is nearly full we
    // simply wait here.  TCP flow control throttles the sender because the
    // reader stops taking free slots.
    while (stream->running && audio_buffer_is_nearly_full(&state->buffer) &&
           slot->generation == state->buffered_generation) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!stream->running || slot->generation != state->buffered_generation) {
      xQueueSend(state->buffered_free_queue, &index, 0);
      continue;
    }

    // Live flush: the sender kept its anchor; what still arrives from the
    // old segment continues the timestamps we already had, the new segment
    // starts with a jump (it is timed near the playhead, the old tail was
    // up to ~16 s ahead of it).
    if (state->live_flush_pending) {
      int32_t step =
          state->live_flush_ref_valid
              ? (int32_t)(slot->timestamp - state->live_flush_last_ts)
              : INT32_MAX;
      int32_t spf =
          stream->format.frame_size > 0 ? stream->format.frame_size : 1024;
      if (state->live_flush_ref_valid && step > 0 && step <= 4 * spf) {
        state->live_flush_last_ts = slot->timestamp;
        state->live_flush_drops++;
        state->stats.packets_dropped++;
        xQueueSend(state->buffered_free_queue, &index, 0);
        continue;
      }
      state->live_flush_pending = false;
      ESP_LOGI(TAG,
               "Live flush: new segment at rtp=%" PRIu32 " seq=%" PRIu32
               " after dropping %" PRIu32 " in-flight packets (step=%ld)",
               slot->timestamp, slot->seq_no, state->live_flush_drops,
               (long)step);
    }

    // RTP gates, in decode order, before any crypto/decoder work.
    if (!audio_stream_accept_timestamp(state, slot->timestamp)) {
      state->stats.packets_dropped++;
      xQueueSend(state->buffered_free_queue, &index, 0);
      continue;
    }

    uint8_t *decrypted = state->decrypt_buffer;
    size_t decrypt_capacity = state->decrypt_buffer_size;
    if (!decrypted) {
      decrypted = slot->data + 12;
      decrypt_capacity = slot->packet_len > 12 ? slot->packet_len - 12 : 0;
    }
    int decrypted_len = audio_crypto_decrypt_buffered(
        &stream->encrypt, slot->data, slot->packet_len, decrypted,
        decrypt_capacity);
    if (decrypted_len < 0) {
      state->stats.decrypt_errors++;
      state->stats.packets_dropped++;
      xQueueSend(state->buffered_free_queue, &index, 0);
      continue;
    }

    state->stats.last_seq = (uint16_t)(slot->seq_no & 0xFFFF);
    state->stats.last_timestamp = slot->timestamp;
    state->blocks_read++;
    state->blocks_read_in_sequence++;

    uint32_t generation_before = state->buffered_generation;
    bool queued = audio_stream_process_accepted_frame(
        state, slot->timestamp, decrypted, (size_t)decrypted_len);
    if (generation_before != state->buffered_generation) {
      // A seek raced this decode.  We are the only producer, so flushing
      // the ring again cannot discard anything from the new generation.
      audio_buffer_flush(&state->buffer);
      queued = false;
    }
    if (!queued) {
      state->stats.packets_dropped++;
    } else if (logged_generation != generation_before) {
      logged_generation = generation_before;
      int64_t now_us = esp_timer_get_time();
      ESP_LOGI(TAG,
               "First frame queued for generation %" PRIu32 ": rtp=%" PRIu32
               " seq=%" PRIu32 " held=%s queue_wait=%lld ms%s%lld ms",
               generation_before, slot->timestamp, slot->seq_no,
               held ? "yes" : "no",
               (long long)((now_us - slot->received_us) / 1000LL),
               state->buffered_flush_us ? " since_flush=" : " since_boot=",
               (long long)((now_us - (state->buffered_flush_us
                                          ? state->buffered_flush_us
                                          : 0)) /
                           1000LL));
    }
    xQueueSend(state->buffered_free_queue, &index, 0);
  }

  state->buffered_decoder_task_handle = NULL;
  vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Reader task                                                        */
/* ------------------------------------------------------------------ */

static void buffered_audio_task(void *pvParameters) {
  audio_stream_t *stream = (audio_stream_t *)pvParameters;
  audio_receiver_state_t *state = audio_stream_state(stream);

  uint32_t logged_flush_serial = 0;
  unsigned post_flush_packets = 3;
  int64_t last_full_log_us = 0;

  while (stream->running) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_sock = accept(state->buffered_listen_socket,
                             (struct sockaddr *)&client_addr, &addr_len);
    if (client_sock < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && stream->running) {
        ESP_LOGE(TAG, "Buffered audio accept error: %d", errno);
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    state->buffered_client_socket = client_sock;
    state->buffered_connections++;
    state->buffered_last_packet_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Buffered data connection #%" PRIu32 " from %s:%u",
             state->buffered_connections, inet_ntoa(client_addr.sin_addr),
             (unsigned)ntohs(client_addr.sin_port));

    struct timeval tv = {.tv_sec = BUFFERED_STALL_TIMEOUT_S, .tv_usec = 0};
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Socket receive buffer: match lwIP's TCP receive window so the kernel
    // buffer can hold exactly what the TCP window allows in flight.  A larger
    // SO_RCVBUF accumulates stale audio that must drain through the RTP
    // gates on every track skip, adding transition latency.
    int rcvbuf = CONFIG_LWIP_TCP_WND_DEFAULT;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    uint8_t *packet = state->buffered_recv_buffer;
    if (!packet) {
      packet = heap_caps_malloc(BUFFERED_AUDIO_PACKET_SIZE,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!packet) {
        packet = malloc(BUFFERED_AUDIO_PACKET_SIZE);
      }
      if (!packet) {
        ESP_LOGE(TAG, "Failed to allocate buffered audio packet buffer");
        close(client_sock);
        state->buffered_client_socket = -1;
        continue;
      }
      state->buffered_recv_buffer = packet;
    }

    while (stream->running) {
      uint8_t len_buf[2];
      if (read_exact(stream, state, client_sock, len_buf, 2) != 2) {
        break;
      }
      uint16_t data_len = (uint16_t)((len_buf[0] << 8) | len_buf[1]);
      if (data_len < 2 || data_len > BUFFERED_AUDIO_PACKET_SIZE) {
        ESP_LOGW(TAG, "Invalid buffered audio packet length: %u", data_len);
        break;
      }
      size_t packet_len = data_len - 2;
      if (read_exact(stream, state, client_sock, packet, packet_len) !=
          (ssize_t)packet_len) {
        break;
      }

      state->stats.packets_received++;
      state->buffered_last_packet_us = esp_timer_get_time();

      if (packet_len < 12) {
        state->stats.packets_dropped++;
        continue;
      }
      if (packet_len > BUFFERED_SLOT_PAYLOAD) {
        // Larger than any AAC frame we expect; count and skip rather than
        // risk the decoder on it.
        ESP_LOGW(TAG, "Buffered packet %u bytes exceeds slot payload %u",
                 (unsigned)packet_len, (unsigned)BUFFERED_SLOT_PAYLOAD);
        state->stats.packets_dropped++;
        continue;
      }

      uint32_t seq_no = (packet[1] << 16) | (packet[2] << 8) | packet[3];
      uint32_t timestamp =
          (packet[4] << 24) | (packet[5] << 16) | (packet[6] << 8) | packet[7];

      uint32_t flush_serial =
          __atomic_load_n(&state->buffered_flush_serial, __ATOMIC_RELAXED);
      if (flush_serial != logged_flush_serial) {
        logged_flush_serial = flush_serial;
        post_flush_packets = 0;
      }
      if (post_flush_packets < 3) {
        post_flush_packets++;
        ESP_LOGI(TAG,
                 "After FLUSHBUFFERED #%" PRIu32 " packet %u/3: rtp=%" PRIu32
                 " seq=%" PRIu32,
                 flush_serial, post_flush_packets, timestamp, seq_no);
      }
      int32_t step = (int32_t)(timestamp - state->buffered_last_rx_ts);
      int64_t spf =
          stream->format.frame_size > 0 ? stream->format.frame_size : 1024;
      if (state->buffered_last_rx_ts_valid &&
          ((int64_t)step > 4 * spf || (int64_t)step < -4 * spf)) {
        ESP_LOGI(TAG,
                 "Buffered RTP jump: rtp=%" PRIu32 " seq=%" PRIu32
                 " previous=%" PRIu32 " step=%" PRId32 " flush=%" PRIu32,
                 timestamp, seq_no, state->buffered_last_rx_ts, step,
                 flush_serial);
      }
      /* This tracks the last timestamp RECEIVED, not the last one stored, so
       * it must advance even for a packet we are about to drop.  Leaving it
       * behind meant that once the hold queue filled, every later packet was
       * still compared against the timestamp from before the drops began and
       * reported as a jump — tens of lines a second, from the one task that
       * has to keep draining TCP for the sender to be able to anchor. */
      state->buffered_last_rx_ts = timestamp;
      state->buffered_last_rx_ts_valid = true;

      // Take a free slot; while the decoder is holding/back-pressured the
      // reader waits here, which closes the TCP window towards the sender.
      uint16_t index = 0;
      int64_t wait_started_us = esp_timer_get_time();
      bool wait_logged = false;
      bool got_slot = false;
      bool drop_packet = false;
      while (stream->running) {
        if (xQueueReceive(state->buffered_free_queue, &index,
                          pdMS_TO_TICKS(20)) == pdTRUE) {
          got_slot = true;
          break;
        }
        int64_t waited_us = esp_timer_get_time() - wait_started_us;
        // Waiting for the anchor: never close the TCP window.  The sender
        // writes its whole post-flush burst before it sends
        // SETRATEANCHORTIME, so a reader that stops taking from TCP here
        // deadlocks: it waits for window, we wait for the anchor.  Drop this
        // packet instead and keep reading.  Dropping the NEWEST keeps the
        // oldest held frames, and those are the ones the anchor names: in
        // every capture the anchor rtp sat at or just below the first packet
        // of the burst.
        if (state->discard_all_until_anchor) {
          drop_packet = true;
          break;
        }
        // Steady state (PCM ring and hold queue both full) is normal
        // back-pressure; only worth a line while a live flush is pending.
        if (!wait_logged && waited_us > 1000000 && state->live_flush_pending) {
          wait_logged = true;
          ESP_LOGW(TAG,
                   "Reader out of packet slots for 1 s (held=%" PRIu32
                   "); sender's TCP window is closed",
                   state->buffered_held_packets);
        }
      }
      if (drop_packet) {
        state->buffered_pre_anchor_drops++;
        state->stats.packets_dropped++;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_full_log_us > 2000000) {
          last_full_log_us = now_us;
          ESP_LOGW(TAG,
                   "Hold queue full before anchor (%" PRIu32
                   " dropped): still reading so the sender can finish its "
                   "burst and anchor",
                   state->buffered_pre_anchor_drops);
        }
        continue;
      }
      if (!stream->running || !got_slot) {
        break;
      }
      buffered_slot_t *slot = slot_at(state, index);
      slot->seq_no = seq_no;
      slot->timestamp = timestamp;
      slot->generation = state->buffered_generation;
      slot->packet_len = (uint16_t)packet_len;
      slot->received_us = state->buffered_last_packet_us;
      memcpy(slot->data, packet, packet_len);
      if (xQueueSend(state->buffered_ready_queue, &index, pdMS_TO_TICKS(20)) !=
          pdTRUE) {
        state->stats.packets_dropped++;
        xQueueSend(state->buffered_free_queue, &index, 0);
      }
    }

    close(client_sock);
    state->buffered_client_socket = -1;
    ESP_LOGI(TAG, "Buffered data connection closed; listener remains active");
  }

  state->buffered_task_handle = NULL;
  vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static bool buffered_wait_for_tasks_stopped(audio_receiver_state_t *state,
                                            int timeout_ticks) {
  while ((state->buffered_task_handle || state->buffered_decoder_task_handle) &&
         timeout_ticks-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return state->buffered_task_handle == NULL &&
         state->buffered_decoder_task_handle == NULL;
}

static void buffered_free_queues(audio_receiver_state_t *state) {
  if (state->buffered_free_queue) {
    vQueueDelete(state->buffered_free_queue);
    state->buffered_free_queue = NULL;
  }
  if (state->buffered_ready_queue) {
    vQueueDelete(state->buffered_ready_queue);
    state->buffered_ready_queue = NULL;
  }
  if (state->buffered_packet_pool) {
    heap_caps_free(state->buffered_packet_pool);
    state->buffered_packet_pool = NULL;
  }
}

static esp_err_t buffered_init_queues(audio_receiver_state_t *state) {
  buffered_free_queues(state);
  state->buffered_packet_pool =
      heap_caps_calloc(BUFFERED_SLOT_COUNT, sizeof(buffered_slot_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  state->buffered_free_queue =
      xQueueCreate(BUFFERED_SLOT_COUNT, sizeof(uint16_t));
  state->buffered_ready_queue =
      xQueueCreate(BUFFERED_SLOT_COUNT, sizeof(uint16_t));
  if (!state->buffered_packet_pool || !state->buffered_free_queue ||
      !state->buffered_ready_queue) {
    buffered_free_queues(state);
    return ESP_ERR_NO_MEM;
  }
  for (uint16_t i = 0; i < BUFFERED_SLOT_COUNT; i++) {
    xQueueSend(state->buffered_free_queue, &i, 0);
  }
  ESP_LOGI(TAG, "Compressed packet queue: %u slots x %u bytes in PSRAM",
           (unsigned)BUFFERED_SLOT_COUNT, (unsigned)sizeof(buffered_slot_t));
  return ESP_OK;
}

static esp_err_t buffered_start(audio_stream_t *stream, uint16_t port) {
  audio_receiver_state_t *state = audio_stream_state(stream);
  if (stream->running) {
    ESP_LOGI(TAG, "Buffered audio already running, continuing");
    return ESP_OK;
  }
  if (state->buffered_task_handle || state->buffered_decoder_task_handle) {
    ESP_LOGW(TAG, "Buffered audio tasks still stopping, waiting");
    if (!buffered_wait_for_tasks_stopped(state, 20)) {
      ESP_LOGW(TAG, "Buffered audio tasks still active");
      return ESP_ERR_INVALID_STATE;
    }
  }

  uint16_t bound_port = port;
  state->buffered_listen_socket =
      socket_utils_bind_tcp_listener(port, 1, true, &bound_port);
  if (state->buffered_listen_socket < 0) {
    return ESP_FAIL;
  }
  state->buffered_port = bound_port;

  esp_err_t err = buffered_init_queues(state);
  if (err != ESP_OK) {
    close(state->buffered_listen_socket);
    state->buffered_listen_socket = -1;
    return err;
  }

  state->buffered_connections = 0;
  state->buffered_stall_timeouts = 0;
  state->buffered_held_packets = 0;
  state->buffered_generation_drops = 0;
  state->buffered_pre_anchor_drops = 0;
  stream->running = true;

  state->buffered_task_handle = NULL;
  state->buffered_decoder_task_handle = NULL;
  BaseType_t decoder_ret = xTaskCreatePinnedToCore(
      buffered_decoder_task, "buff_decode", AUDIO_DECODER_STACK_SIZE, stream, 6,
      &state->buffered_decoder_task_handle, BUFFERED_DECODER_CORE);
  BaseType_t reader_ret =
      xTaskCreate(buffered_audio_task, "buff_audio", AUDIO_BUFFERED_STACK_SIZE,
                  stream, 5, &state->buffered_task_handle);
  if (decoder_ret != pdPASS || reader_ret != pdPASS ||
      !state->buffered_decoder_task_handle || !state->buffered_task_handle) {
    ESP_LOGE(TAG, "Failed to create buffered reader/decoder tasks");
    stream->running = false;
    close(state->buffered_listen_socket);
    state->buffered_listen_socket = -1;
    buffered_wait_for_tasks_stopped(state, 20);
    buffered_free_queues(state);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG,
           "Buffered start on port %u: free heap %lu internal (largest block "
           "%lu), %lu SPIRAM",
           bound_port,
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  return ESP_OK;
}

static void buffered_stop(audio_stream_t *stream) {
  audio_receiver_state_t *state = audio_stream_state(stream);
  if (!stream->running && !state->buffered_task_handle &&
      !state->buffered_decoder_task_handle) {
    return;
  }

  stream->running = false;

  if (state->buffered_client_socket >= 0) {
    // The reader owns close(); shutdown() is enough to wake its recv() and
    // avoids a cross-task double close if lwIP reuses the descriptor.
    shutdown(state->buffered_client_socket, SHUT_RDWR);
    state->buffered_client_socket = -1;
  }
  if (state->buffered_listen_socket >= 0) {
    close(state->buffered_listen_socket);
    state->buffered_listen_socket = -1;
  }

  if (!buffered_wait_for_tasks_stopped(state, 20)) {
    ESP_LOGW(TAG, "Buffered audio tasks did not exit within timeout");
    return;
  }

  if (state->buffered_recv_buffer) {
    heap_caps_free(state->buffered_recv_buffer);
    state->buffered_recv_buffer = NULL;
  }
  buffered_free_queues(state);
  state->buffered_port = 0;
  ESP_LOGI(TAG,
           "Buffered stream stopped: connections=%" PRIu32 " stalls=%" PRIu32
           " held=%" PRIu32 " pre_seek_drops=%" PRIu32
           " pre_anchor_drops=%" PRIu32,
           state->buffered_connections, state->buffered_stall_timeouts,
           state->buffered_held_packets, state->buffered_generation_drops,
           state->buffered_pre_anchor_drops);
}

static uint16_t buffered_get_port(audio_stream_t *stream) {
  audio_receiver_state_t *state = audio_stream_state(stream);
  return state->buffered_port;
}

static bool buffered_is_running(audio_stream_t *stream) {
  return stream->running;
}

static void buffered_destroy(audio_stream_t *stream) {
  if (!stream) {
    return;
  }

  buffered_stop(stream);
  audio_receiver_state_t *state = audio_stream_state(stream);
  if (state->buffered_task_handle || state->buffered_decoder_task_handle) {
    ESP_LOGW(TAG, "Leaking buffered stream because task shutdown timed out");
    return;
  }
  free(stream);
}

const audio_stream_ops_t audio_stream_buffered_ops = {
    .start = buffered_start,
    .stop = buffered_stop,
    .receive_packet = NULL,
    .decrypt_payload = NULL,
    .get_port = buffered_get_port,
    .is_running = buffered_is_running,
    .destroy = buffered_destroy};
