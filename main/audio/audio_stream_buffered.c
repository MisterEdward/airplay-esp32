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
#include "esp_timer.h"

#include "audio_crypto.h"
#include "network/socket_utils.h"
#include "ntp_clock.h"
#include "ptp_clock.h"

#define BUFFERED_AUDIO_PACKET_SIZE 8192
#define AUDIO_BUFFERED_STACK_SIZE  4096
#define PRODUCER_LATE_SKIP_US      60000LL

static StaticTask_t s_buffered_tcb;
static StackType_t *s_buffered_stack;

static const char *TAG = "audio_buf";

static bool buffered_packet_target_us(const audio_receiver_state_t *state,
                                      uint32_t timestamp,
                                      int64_t *target_us) {
  if (!state || !state->timing.anchor_valid || !target_us) {
    return false;
  }

  int sr = state->stream->format.sample_rate;
  if (sr <= 0) {
    sr = 44100;
  }

  int32_t diff = (int32_t)(timestamp - state->timing.anchor_rtp_time);
  int64_t frame_offset_ns = ((int64_t)diff * 1000000000LL) / sr;
  int64_t target_ns;

  if (ptp_clock_is_locked()) {
    target_ns = (int64_t)state->timing.anchor_network_time_ns -
                ptp_clock_get_offset_ns() + frame_offset_ns;
  } else if (ntp_clock_is_locked()) {
    target_ns = (int64_t)state->timing.anchor_network_time_ns -
                ntp_clock_get_offset_ns() + frame_offset_ns;
  } else {
    target_ns = state->timing.anchor_local_time_ns + frame_offset_ns;
  }

  target_ns -= (int64_t)audio_timing_get_hardware_latency() * 1000LL;
  *target_us = target_ns / 1000LL;
  return true;
}

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
      // Back-pressure: if buffer is nearly full, pause reading to let TCP
      // flow control slow down the sender. This prevents buffer overflow
      // and keeps frames in order.
      while (audio_buffer_is_nearly_full(&state->buffer) && stream->running) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }

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

      // Pre-decode stale-packet skip during post-seek catch-up.
      //
      // After a FLUSH/seek the OS TCP socket can hold several seconds of
      // pre-flush audio that the sender pushed before learning of the seek.
      // If we decrypt+decode every byte, the decoder feeds the ring buffer
      // with old-position PCM that audio_timing_read then has to discard
      // frame-by-frame.  Even with the in-loop drain, this stalls the user
      // for several seconds per seek (the bottleneck becomes decoder
      // throughput rather than network throughput).
      //
      // Instead, while the seek-drain window is open, peek at the
      // packet's RTP timestamp BEFORE doing any crypto/decoder work and
      // drop packets whose RTP is far outside the keep window around the
      // new anchor.  Skipping decrypt+decode on stale packets drains the
      // TCP backlog at network speed (a few hundred ms instead of
      // seconds) and lets the first genuinely current packet hit the
      // ring buffer cleanly.
      //
      // The window is TIME-BOUNDED via state->seek_drain_until_us (3 s
      // post-seek), NOT tied to timing.post_flush — post_flush can stay
      // true much longer than the actual drain (it only exits on 10
      // consecutive ±60 ms frames; if steady-state drift settles outside
      // that band, post_flush persists indefinitely and an anchor-RTP
      // window would eventually reject all real-time packets as wall
      // clock advances, killing playback dead a few seconds after every
      // seek).
      //
      // The receiver-level fields written by the RTSP task and read here
      // are aligned 32-bit (anchor_rtp_time, anchor_valid) or 64-bit
      // (seek_drain_until_us); reads may transiently see torn 64-bit
      // values during a write but the worst case is one packet
      // misclassified, which is harmless.
      if (state->timing.anchor_valid &&
          esp_timer_get_time() < state->seek_drain_until_us) {
        int sr = state->stream->format.sample_rate;
        if (sr <= 0) {
          sr = 44100;
        }
        int32_t diff = (int32_t)(timestamp - state->timing.anchor_rtp_time);
        // Keep window: -2 s before anchor (small slack for slightly-stale
        // pre-flush data the sender legitimately re-sends) to +15 s after
        // (covers the pre-buffer burst AirPlay 2 typically sends after a
        // seek).  Anything outside is unambiguously from the wrong song
        // position.
        const int32_t k_before = -2 * sr;
        const int32_t k_after = 15 * sr;
        if (diff < k_before || diff > k_after) {
          state->stats.packets_dropped++;
          continue;
        }
      }

      // Pre-decode wall-clock late skip.
      //
      // Mirrors shairport-sync's lead_time gate in
      // ap2_buffered_audio_processor.c: a packet whose scheduled playback
      // time has already passed in wall-clock terms cannot reach the
      // speakers on time no matter how fast we decode it.  Decoding it
      // anyway burns ~20 ms of CPU per AAC frame for output the consumer
      // (audio_timing.c) will drop a moment later — and crucially, that
      // CPU time is what blocks the decoder from reaching genuinely
      // on-time packets sitting further down the TCP socket.
      //
      // After a seek, the iPhone delivers a pre-buffer burst over TCP
      // covering the new song position.  Several hundred ms of audio in
      // that burst are "already in the past" relative to wall-clock by
      // the time we receive them (TCP buffering + RTSP-to-data-channel
      // latency).  Without this skip, the AAC decoder grinds through the
      // entire past portion at ~1× real-time, producing 3 s of consumer
      // underruns ("set_playing -> playing" -> first audible frame).
      // Skipping decrypt+decode on these packets drains the past portion
      // at network speed (microseconds per packet), so the decoder
      // reaches genuinely on-time packets within ~100 ms of the seek.
      //
      // Threshold is -60 ms (matches TIMING_THRESHOLD_LATE_US in
      // audio_timing.c).  Packets within the consumer's tolerable
      // lateness still get decoded — the consumer has finer state
      // (pending-frame buffer, post_flush bypass) and is better placed
      // to decide than we are here.
      //
      // Always-on (not gated by seek_drain_until_us) so a sustained
      // WiFi stall during normal playback also gets producer-side
      // relief.  Shairport-sync does the same.
      //
      // Uses the same clock universe as audio_timing.c: PTP/NTP-adjusted
      // network anchor when locked, local anchor only as fallback. This keeps
      // producer-side drops aligned with the consumer's early/late decision.
      if (state->timing.anchor_valid) {
        int64_t target_us = 0;
        if (buffered_packet_target_us(state, timestamp, &target_us)) {
          int64_t lead_us = target_us - esp_timer_get_time();
          if (lead_us < -PRODUCER_LATE_SKIP_US) {
            state->stats.packets_dropped++;
            continue;
          }
        }
      }


      uint8_t *decrypted = state->decrypt_buffer;
      size_t decrypt_capacity = state->decrypt_buffer_size;
      if (!decrypted) {
        decrypted = packet + 12;
        decrypt_capacity = packet_len > 12 ? packet_len - 12 : 0;
      }

      int decrypted_len = audio_crypto_decrypt_buffered(
          &stream->encrypt, packet, packet_len, decrypted, decrypt_capacity);
      if (decrypted_len < 0) {
        state->stats.decrypt_errors++;
        state->stats.packets_dropped++;
        continue;
      }

      state->stats.last_seq = (uint16_t)(seq_no & 0xFFFF);
      state->stats.last_timestamp = timestamp;

      state->blocks_read++;
      state->blocks_read_in_sequence++;

      if (!audio_stream_process_frame(state, timestamp, decrypted,
                                      (size_t)decrypted_len)) {
        state->stats.packets_dropped++;
      }
    }

    close(client_sock);
    state->buffered_client_socket = -1;
  }

  state->buffered_task_handle = NULL;
  vTaskDelete(NULL);
}

static esp_err_t buffered_start(audio_stream_t *stream, uint16_t port) {
  audio_receiver_state_t *state = audio_stream_state(stream);
  if (stream->running) {
    ESP_LOGI(TAG, "Buffered audio already running, continuing");
    return ESP_OK;
  }

  uint16_t bound_port = port;
  state->buffered_listen_socket =
      socket_utils_bind_tcp_listener(port, 1, true, &bound_port);
  if (state->buffered_listen_socket < 0) {
    return ESP_FAIL;
  }
  state->buffered_port = bound_port;

  stream->running = true;

  // Allocate stack from SPIRAM on first use — the buffered task only does
  // socket I/O and decryption, no SPI flash access, so SPIRAM is safe.
  // This avoids competing with BT/WiFi/display for scarce internal DRAM.
  if (!s_buffered_stack) {
    s_buffered_stack = heap_caps_malloc(AUDIO_BUFFERED_STACK_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buffered_stack) {
      // Fallback to any available memory
      s_buffered_stack = malloc(AUDIO_BUFFERED_STACK_SIZE);
    }
    if (!s_buffered_stack) {
      ESP_LOGE(TAG, "Failed to allocate buffered audio stack");
      close(state->buffered_listen_socket);
      state->buffered_listen_socket = -1;
      stream->running = false;
      return ESP_ERR_NO_MEM;
    }
  }

  state->buffered_task_handle =
      xTaskCreateStatic(buffered_audio_task, "buff_audio",
                        AUDIO_BUFFERED_STACK_SIZE / sizeof(StackType_t), stream,
                        5, s_buffered_stack, &s_buffered_tcb);
  if (!state->buffered_task_handle) {
    ESP_LOGE(TAG, "Failed to create buffered audio task");
    close(state->buffered_listen_socket);
    state->buffered_listen_socket = -1;
    stream->running = false;
    return ESP_FAIL;
  }

  return ESP_OK;
}

static void buffered_stop(audio_stream_t *stream) {
  audio_receiver_state_t *state = audio_stream_state(stream);
  if (!stream->running) {
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

  if (state->buffered_task_handle) {
    vTaskDelay(pdMS_TO_TICKS(300));
    state->buffered_task_handle = NULL;
  }
  task_free_spiram(&state->buffered_task_mem);

  if (state->buffered_recv_buffer) {
    heap_caps_free(state->buffered_recv_buffer);
    state->buffered_recv_buffer = NULL;
  }

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
