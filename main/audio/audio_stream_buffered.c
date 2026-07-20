#include <errno.h>
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

#include "audio_crypto.h"
#include "network/socket_utils.h"

#define BUFFERED_AUDIO_PACKET_SIZE 8192
#define AUDIO_BUFFERED_STACK_SIZE  4096
#define AUDIO_DECODER_STACK_SIZE   6144
#define BUFFERED_PACKET_QUEUE_LEN  32

#if CONFIG_FREERTOS_UNICORE
#define BUFFERED_DECODER_CORE 0
#else
#define BUFFERED_DECODER_CORE 1
#endif

typedef struct {
  uint32_t seq_no;
  uint32_t timestamp;
  uint32_t seek_generation;
  uint16_t packet_len;
  uint8_t data[BUFFERED_AUDIO_PACKET_SIZE];
} buffered_packet_slot_t;

static const char *TAG = "audio_buf";

// Read exact number of bytes, but keep waiting on timeout if paused
// Returns: positive = bytes read, 0 = connection closed, -1 = error
static ssize_t read_exact(audio_stream_t *stream, audio_receiver_state_t *state,
                          int sock, uint8_t *buf, size_t len) {
  size_t total = 0;
  while (total < len && stream->running) {
    ssize_t n = recv(sock, buf + total, len - total, 0);
    if (n > 0) {
      total += (size_t)n;
    } else if (n == 0) {
      // Connection closed by peer
      ESP_LOGI(TAG, "Buffered audio connection closed by peer");
      return 0;
    } else {
      // n < 0: error or timeout
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Timeout - if we're paused, keep waiting for resume
        if (!state->timing.playing) {
          // Still paused, keep the connection alive
          vTaskDelay(pdMS_TO_TICKS(100));
          continue;
        }
        // Playing but timed out - connection may be dead
        ESP_LOGW(TAG, "Buffered audio timeout while playing");
        return -1;
      }
      ESP_LOGE(TAG, "Buffered audio recv error: %d", errno);
      return -1;
    }
  }
  return stream->running ? (ssize_t)total : -1;
}

static void buffered_decoder_task(void *pvParameters) {
  audio_stream_t *stream = (audio_stream_t *)pvParameters;
  audio_receiver_state_t *state = audio_stream_state(stream);
  buffered_packet_slot_t *slots =
      (buffered_packet_slot_t *)state->buffered_packet_pool;

  while (stream->running) {
    uint8_t slot_index = 0;
    if (xQueueReceive(state->buffered_ready_queue, &slot_index,
                      pdMS_TO_TICKS(20)) != pdTRUE) {
      continue;
    }

    buffered_packet_slot_t *slot = &slots[slot_index];

    // During FLUSH -> anchor, retain compressed packets instead of throwing
    // away the selected word. The 32-slot queue fills, then TCP back-pressure
    // safely holds the rest in lwIP until the anchor defines the RTP window.
    while (stream->running && state->discard_all_until_anchor &&
           slot->seek_generation == audio_receiver_get_seek_generation()) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (!stream->running ||
        slot->seek_generation != audio_receiver_get_seek_generation()) {
      xQueueSend(state->buffered_free_queue, &slot_index, 0);
      continue;
    }

    while (audio_buffer_is_nearly_full(&state->buffer) && stream->running &&
           slot->seek_generation == audio_receiver_get_seek_generation()) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!stream->running ||
        slot->seek_generation != audio_receiver_get_seek_generation()) {
      xQueueSend(state->buffered_free_queue, &slot_index, 0);
      continue;
    }

    // Filter future deferred-flush packets before decrypt/AAC decode. Frames
    // decoded before the RTSP request arrived carry the same packet sequence
    // in their PCM header and are filtered by audio_timing_read.
    if (audio_timing_deferred_flush_contains_sequence(&state->timing,
                                                      slot->seq_no)) {
      state->stats.packets_dropped++;
      xQueueSend(state->buffered_free_queue, &slot_index, 0);
      continue;
    }

    // Decode task owns the shared decrypt buffer. Stale RTP is rejected by
    // audio_stream_process_frame before AAC decode, after cheap decryption.
    uint8_t *decrypted = state->decrypt_buffer;
    size_t decrypt_capacity = state->decrypt_buffer_size;
    if (!decrypted) {
      decrypted = slot->data + 12;
      decrypt_capacity =
          slot->packet_len > 12 ? slot->packet_len - 12 : 0;
    }

    int decrypted_len = audio_crypto_decrypt_buffered(
        &stream->encrypt, slot->data, slot->packet_len, decrypted,
        decrypt_capacity);
    if (decrypted_len < 0) {
      state->stats.decrypt_errors++;
      state->stats.packets_dropped++;
      xQueueSend(state->buffered_free_queue, &slot_index, 0);
      continue;
    }

    state->stats.last_seq = (uint16_t)(slot->seq_no & 0xFFFF);
    state->stats.last_timestamp = slot->timestamp;
    state->blocks_read++;
    state->blocks_read_in_sequence++;

    uint32_t generation_before = audio_receiver_get_seek_generation();
    bool queued = audio_stream_process_frame(
        state, slot->seq_no, slot->timestamp, decrypted,
        (size_t)decrypted_len);
    if (generation_before != audio_receiver_get_seek_generation()) {
      // FLUSH raced the decode/queue operation. This decoder is serial, so
      // flushing here can only remove the old generation just inserted.
      audio_buffer_flush(&state->buffer);
      queued = false;
    }
    if (!queued) {
      state->stats.packets_dropped++;
    }

    xQueueSend(state->buffered_free_queue, &slot_index, 0);
  }

  state->buffered_decoder_task_handle = NULL;
  vTaskDelete(NULL);
}

static void buffered_audio_task(void *pvParameters) {
  audio_stream_t *stream = (audio_stream_t *)pvParameters;
  audio_receiver_state_t *state = audio_stream_state(stream);

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

    struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Socket receive buffer: match lwIP's TCP receive window so the kernel
    // buffer can hold exactly what the TCP window allows in flight.  A larger
    // SO_RCVBUF (e.g. the old 65536) accumulates stale audio data that must
    // drain through the RTP gates on every track skip, adding transition
    // latency.  Keeping it at TCP_WND ties both knobs to a single sdkconfig
    // value (CONFIG_LWIP_TCP_WND_DEFAULT).
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

      uint32_t seq_no = (packet[1] << 16) | (packet[2] << 8) | packet[3];
      uint32_t timestamp =
          (packet[4] << 24) | (packet[5] << 16) | (packet[6] << 8) | packet[7];
      audio_receiver_diag_note_packet(state, timestamp);

      uint8_t slot_index = 0;
      while (stream->running &&
             xQueueReceive(state->buffered_free_queue, &slot_index,
                           pdMS_TO_TICKS(20)) != pdTRUE) {
      }
      if (!stream->running) {
        break;
      }

      buffered_packet_slot_t *slots =
          (buffered_packet_slot_t *)state->buffered_packet_pool;
      buffered_packet_slot_t *slot = &slots[slot_index];
      slot->seq_no = seq_no;
      slot->timestamp = timestamp;
      slot->seek_generation = audio_receiver_get_seek_generation();
      slot->packet_len = (uint16_t)packet_len;
      memcpy(slot->data, packet, packet_len);

      if (xQueueSend(state->buffered_ready_queue, &slot_index,
                     pdMS_TO_TICKS(20)) != pdTRUE) {
        state->stats.packets_dropped++;
        xQueueSend(state->buffered_free_queue, &slot_index, 0);
      }
    }

    close(client_sock);
    state->buffered_client_socket = -1;
  }

  state->buffered_task_handle = NULL;
  vTaskDelete(NULL);
}

static bool buffered_wait_for_tasks_stopped(audio_receiver_state_t *state,
                                            int timeout_ticks) {
  while ((state->buffered_task_handle ||
          state->buffered_decoder_task_handle) &&
         timeout_ticks-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return state->buffered_task_handle == NULL &&
         state->buffered_decoder_task_handle == NULL;
}

static void buffered_free_packet_queue(audio_receiver_state_t *state) {
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

static esp_err_t buffered_init_packet_queue(audio_receiver_state_t *state) {
  buffered_free_packet_queue(state);
  state->buffered_packet_pool = heap_caps_calloc(
      BUFFERED_PACKET_QUEUE_LEN, sizeof(buffered_packet_slot_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  state->buffered_free_queue =
      xQueueCreate(BUFFERED_PACKET_QUEUE_LEN, sizeof(uint8_t));
  state->buffered_ready_queue =
      xQueueCreate(BUFFERED_PACKET_QUEUE_LEN, sizeof(uint8_t));
  if (!state->buffered_packet_pool || !state->buffered_free_queue ||
      !state->buffered_ready_queue) {
    buffered_free_packet_queue(state);
    return ESP_ERR_NO_MEM;
  }
  for (uint8_t i = 0; i < BUFFERED_PACKET_QUEUE_LEN; i++) {
    xQueueSend(state->buffered_free_queue, &i, 0);
  }
  ESP_LOGI(TAG, "Compressed packet queue: %u slots, %u bytes PSRAM",
           BUFFERED_PACKET_QUEUE_LEN,
           (unsigned)(BUFFERED_PACKET_QUEUE_LEN *
                      sizeof(buffered_packet_slot_t)));
  return ESP_OK;
}

static esp_err_t buffered_start(audio_stream_t *stream, uint16_t port) {
  audio_receiver_state_t *state = audio_stream_state(stream);
  if (stream->running) {
    ESP_LOGI(TAG, "Buffered audio already running, continuing");
    return ESP_OK;
  }
  if (state->buffered_task_handle) {
    ESP_LOGW(TAG, "Buffered audio task still stopping, waiting");
    if (!buffered_wait_for_tasks_stopped(state, 20)) {
      ESP_LOGW(TAG, "Buffered audio task still active");
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

  esp_err_t queue_err = buffered_init_packet_queue(state);
  if (queue_err != ESP_OK) {
    close(state->buffered_listen_socket);
    state->buffered_listen_socket = -1;
    return queue_err;
  }

  stream->running = true;

  state->buffered_task_handle = NULL;
  state->buffered_decoder_task_handle = NULL;
  BaseType_t decoder_ret = xTaskCreatePinnedToCore(
      buffered_decoder_task, "buff_decode", AUDIO_DECODER_STACK_SIZE, stream,
      6, &state->buffered_decoder_task_handle, BUFFERED_DECODER_CORE);
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
    buffered_free_packet_queue(state);
    return ESP_FAIL;
  }

  return ESP_OK;
}

static void buffered_stop(audio_stream_t *stream) {
  audio_receiver_state_t *state = audio_stream_state(stream);
  if (!stream->running && !state->buffered_task_handle &&
      !state->buffered_decoder_task_handle) {
    return;
  }

  stream->running = false;

  if (state->buffered_client_socket > 0) {
    close(state->buffered_client_socket);
    state->buffered_client_socket = -1;
  }

  if (state->buffered_listen_socket > 0) {
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

  buffered_free_packet_queue(state);

  state->buffered_port = 0;
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
