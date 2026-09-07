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
 * Stall handling: the sender normally streams continuously (it pre-buffers
 * seconds ahead).  If no bytes arrive for BUFFERED_STALL_TIMEOUT_S while we
 * are playing, only the data connection is closed; the listener stays up so
 * the sender can reconnect without a new RTSP session.
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
// new anchor arrives, and while the free queue is empty the reader stops
// taking from TCP, which closes the window towards the sender.  The sender
// does not anchor until its post-seek burst has been accepted (observed:
// 6.3 s of audio pushed before the first anchor), so 48 slots (1.1 s) was a
// deadlock: the phone waited for window, we waited for the anchor, and the
// session went silent for good.  384 slots ≈ 8.9 s of AAC, ~800 KB PSRAM.
#define BUFFERED_SLOT_PAYLOAD 2048
// 384 slots is 8.9 s of compressed audio.  When the phone already has the
// next track queued it pushes more than that after a flush before it sends
// SETRATEANCHORTIME, the queue fills, the reader stops taking from TCP, and
// both sides wait forever — seek goes silent until the session is torn down.
// 900 slots is 20.9 s, which covers the sender's full lead.  Costs 1.86 MB
// of PSRAM, of which there is plenty (4.4 MB free with the stream running).
#define BUFFERED_SLOT_COUNT 900
// Frames in the PCM ring below which a silent data socket really is a stall
// rather than the sender simply running ahead.  ~3 s at 352-frame blocks.
#define BUFFERED_STALL_MIN_FRAMES 130
// How long to hold a post-seek segment waiting for SETRATEANCHORTIME before
// playing it unscheduled.  A healthy sender anchors in 350-500 ms, so this
// only ever fires when the sender has decided not to anchor at all.
#define BUFFERED_ANCHOR_WAIT_US (8 * 1000 * 1000)
// Shorter deadline for the case where we KNOW the sender stopped reading
// its RTSP socket: it will not anchor, so there is nothing left to wait
// for.  A healthy sender's replies leave in microseconds, so this cannot
// fire on the path that makes multiroom sync instant.
#define BUFFERED_ANCHOR_WAIT_DEAF_US (1200 * 1000)
#define BUFFERED_STALL_TIMEOUT_S     8
#define BUFFERED_HOLD_POLL_MS        2

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

// Read exactly `len` bytes.  Returns bytes read, 0 = peer closed, -1 = error
// or stall.  While paused the timeout is not a stall: the sender has nothing
// to send, so keep waiting.
static ssize_t read_exact(audio_stream_t *stream, audio_receiver_state_t *state,
                          int sock, uint8_t *buf, size_t len) {
  size_t total = 0;
  int64_t started_us = esp_timer_get_time();
  bool idle_logged = false;
  bool stall_logged = false;
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
        // A sender that runs ~20 s ahead sends in bursts with idle gaps
        // longer than this timeout.  While the PCM ring still holds audio
        // the quiet socket is not a stall, and closing it here is what
        // actually breaks the session: measured after a crossfade, the
        // socket went quiet for 8 s with 899 frames (7 s) still buffered
        // and playout error at +415 us — we closed the connection anyway
        // and the phone answered with TEARDOWN 20 ms later.
        int buffered_frames = audio_buffer_get_frame_count(&state->buffer);
        if (buffered_frames > BUFFERED_STALL_MIN_FRAMES) {
          if (!idle_logged) {
            idle_logged = true;
            ESP_LOGD(TAG,
                     "Data socket idle with %d frames buffered; waiting "
                     "rather than reopening",
                     buffered_frames);
          }
          continue;
        }
        int64_t now_us = esp_timer_get_time();
        state->buffered_stall_timeouts++;
        /*
         * Do not close the socket.  Closing was meant as a recovery — drop
         * the connection and let the sender reopen it — but the sender does
         * not reopen: every capture of this path ends with
         * `Buffered stream stopped: connections=1`, one connection for the
         * whole session, and the speaker silent for good while the same
         * stream keeps playing elsewhere.  So the "recovery" is what kills
         * the session.
         *
         * Waiting costs nothing by comparison. If the sender resumes we
         * resume with it; if it is really gone, the RTSP session tears down
         * and that path stops the stream properly.
         */
        if (!stall_logged) {
          stall_logged = true;
          ESP_LOGW(
              TAG,
              "Data socket quiet %lld ms while playing (last packet "
              "%lld ms ago, %d frames buffered) — waiting, not closing",
              (long long)((now_us - started_us) / 1000LL),
              (long long)((now_us - state->buffered_last_packet_us) / 1000LL),
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
    int64_t hold_started_us = esp_timer_get_time();
    while (stream->running && state->discard_all_until_anchor &&
           slot->generation == state->buffered_generation) {
      if (!held) {
        held = true;
        state->buffered_held_packets++;
      }
      /*
       * Give up eventually.  Measured on a seek near the end of a track:
       * the reader was demonstrably running at full rate — the recycle
       * counter advanced 101 packets every 2 s, so fifty a second against
       * the forty-three the sender produces, TCP window wide open — and the
       * sender still never sent SETRATEANCHORTIME.  It simply streamed on
       * while we held everything and stayed silent forever.
       *
       * We cannot make the sender anchor.  We can refuse to wait for it
       * indefinitely: play what we hold unscheduled and let the timing
       * engine re-acquire when an anchor does arrive.  Eight seconds is far
       * beyond the 350-500 ms a healthy sender takes, so this never fires
       * on the path that makes multiroom sync instant — it only converts
       * permanent silence into a few seconds of it.
       */
      int64_t hold_us = esp_timer_get_time() - hold_started_us;
      bool sender_deaf = audio_receiver_sender_unresponsive_since() != 0 &&
                         hold_us > BUFFERED_ANCHOR_WAIT_DEAF_US;
      if (sender_deaf || hold_us > BUFFERED_ANCHOR_WAIT_US) {
        state->discard_all_until_anchor = false;
        state->timing.quick_start = true;
        ESP_LOGW(TAG,
                 "No anchor %lld ms after the flush%s: playing the held "
                 "segment unscheduled (rtp=%" PRIu32 ")",
                 (long long)(hold_us / 1000LL),
                 sender_deaf ? " and the sender is not reading our replies"
                             : "",
                 slot->timestamp);
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

      // Take a free slot; while the decoder is holding/back-pressured the
      // reader waits here, which closes the TCP window towards the sender.
      uint16_t index = 0;
      /* Rate limit for the recycle warning; static so it survives across
       * packets, which is the point — one line per packet would be a flood
       * from the very task that has to keep draining TCP. */
      static int64_t last_recycle_log_us = 0;
      int64_t wait_started_us = esp_timer_get_time();
      bool wait_logged = false;
      bool got_slot = false;
      bool drop_packet = false;
      /*
       * Waiting for the anchor: never wait for a slot, and never throw the
       * packet away.  Take a free one if there is one, otherwise evict the
       * OLDEST held packet and reuse its slot, both with a zero timeout.
       *
       * The rate is the whole point.  The sender will not send
       * SETRATEANCHORTIME until its post-flush burst — about twenty seconds
       * of audio — has been written, so anything that drains the socket
       * slower than the sender fills it postpones the anchor indefinitely.
       * Waiting 20 ms per packet allows fifty a second while the sender
       * produces forty-three, a net seven: minutes for that burst, which is
       * why seek used to hang.  Draining at line rate makes it about a
       * second.
       *
       * Dropping the packets outright also drains at line rate and also
       * fixes the hang, but it throws away the sender's twenty-second lead,
       * so playout restarts at the live edge with no cushion: measured, the
       * ring sawtoothed from 540 frames down to 40 and punched a 23 ms hole
       * every few seconds, twenty-five of them, which is audible as popping.
       * Retaining the newest packets keeps the cushion and starts playout at
       * the anchor instead of half a second past it.
       */
      while (stream->running) {
        if (state->discard_all_until_anchor) {
          if (xQueueReceive(state->buffered_free_queue, &index, 0) != pdTRUE) {
            uint16_t victim = 0;
            if (xQueueReceive(state->buffered_ready_queue, &victim, 0) ==
                pdTRUE) {
              state->buffered_pre_anchor_drops++;
              index = victim;
            } else {
              drop_packet = true;
              break;
            }
          }
          got_slot = true;
          break;
        }
        if (xQueueReceive(state->buffered_free_queue, &index,
                          pdMS_TO_TICKS(20)) == pdTRUE) {
          got_slot = true;
          break;
        }
        int64_t now_us = esp_timer_get_time();
        int64_t waited_us = now_us - wait_started_us;
        // Waiting for the anchor: never close the TCP window.  The sender
        // does not anchor until its whole post-flush burst has been
        // accepted (with a queued next track that burst exceeded 8.9 s), so
        // stalling it here is a deadlock: it waits for window, we wait for
        // the anchor.  Recycle the oldest queued packet instead — the
        // anchor sits at the END of the burst, so the newest packets are
        // the ones the RTP gate will want.
        // No delay before recycling.  Waiting 200 ms per packet let through
        // five packets a second against the forty-three the sender produces,
        // which IS a closed window however large the queue is: the sender
        // blocks on its own write, never finishes the burst, and never
        // anchors.  Measured: `recycling oldest` every 220 ms for as long as
        // the seek lasted, with not one RTSP request from the sender in
        // between — not even its two-second /feedback.
        if (state->discard_all_until_anchor) {
          uint16_t victim = 0;
          if (xQueueReceive(state->buffered_ready_queue, &victim, 0) ==
              pdTRUE) {
            state->buffered_pre_anchor_drops++;
            if (!wait_logged && now_us - last_recycle_log_us > 2000000) {
              wait_logged = true;
              last_recycle_log_us = now_us;
              ESP_LOGW(TAG,
                       "Hold queue full before anchor (%" PRIu32
                       " recycled): still reading so the sender can finish "
                       "its burst",
                       state->buffered_pre_anchor_drops);
            }
            index = victim;
            got_slot = true;
            break;
          }
        }
        if (!wait_logged && waited_us > 1000000) {
          wait_logged = true;
          ESP_LOGW(TAG,
                   "Reader out of packet slots for 1 s (held=%" PRIu32
                   "); sender's TCP window is closed",
                   state->buffered_held_packets);
        }
      }
      if (drop_packet) {
        state->stats.packets_dropped++;
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
