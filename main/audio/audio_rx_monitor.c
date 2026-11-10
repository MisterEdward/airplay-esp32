/**
 * Dropout monitor: makes a silent "playing" speaker visible.
 *
 * Until now nothing was logged once the PCM ring ran dry: the Playout line
 * needs a played frame, the reader's stall warning needs 8 s without a
 * single byte.  The journal of 2026-11-10 shows 16 s of silence while
 * "playing" without one line, on a data connection that trickled a few
 * packets every 5 s.
 *
 * A low-priority task watches the ring and reports, once a second while it is
 * low or dry, what tells the possible causes apart:
 *
 *   sender / link:   pkt_age, byte_age, rx (bytes per second off the socket)
 *   our reader:      reader=recv|slot_wait, free/ready slots
 *   our decoder:     decoder state and the age of its last frame
 *   TCP itself:      the data connection's receive window as lwIP announces
 *                    it, out-of-order segments waiting for a retransmission,
 *                    and how fast in-order data arrives
 *   clock / radio:   PTP gap and last Sync age, RSSI
 *
 * It only reads.  Nothing here changes what is played or when.
 */

#include <inttypes.h>
#include <string.h>

#include "audio_receiver_internal.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "ptp_clock.h"

static const char *TAG = "audio_rxmon";

#define MONITOR_PERIOD_MS 250
// Buffered stream: the ring normally sits at its cap (900 frames, ~7 s).
// Below half of it while playing, after it has been full, the sender is not
// keeping up.  At 800 it is healthy again.
#define RING_OK_FRAMES  800
#define RING_LOW_FRAMES 450
// An empty ring is normal for a moment after a seek or at session start
// (sound comes 0.4-0.9 s after the flush; the first audio of a session took
// 2.8 s), so a dry ring is reported after this long once audio has played
// under the current anchor, and after DRY_GRACE_START_MS if it never did.
#define DRY_GRACE_MS       2000
#define DRY_GRACE_START_MS 6000
// Hovering above the low mark this long also ends an episode (a sender that
// keeps a short lead never reaches RING_OK_FRAMES).
#define STEADY_MS 5000

typedef struct {
  bool valid;
  uint32_t rcv_wnd;
  uint32_t ann_wnd;
  uint32_t ooseq_segs;
  uint32_t ooseq_bytes;
  bool refused;
  uint32_t rcv_nxt;
  uint32_t taken_ms;
  uint32_t bytes_per_s; // in-order data since the previous snapshot
} tcp_snapshot_t;

static audio_receiver_state_t *s_state;
static struct tcpip_callback_msg *s_tcp_msg;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static tcp_snapshot_t s_tcp; // written by the tcpip thread under s_mux

// Monitor task state; the counters are also read by the status API.
static struct {
  uint32_t last_packets;
  uint32_t last_packet_change_ms; // either stream type, 250 ms resolution
  uint32_t rate_ms;
  uint32_t rate_bytes;
  uint32_t rx_bytes_per_s;

  uint32_t generation;
  bool armed;  // the ring has been full since the last seek/pause
  bool played; // audio has played since the last seek/pause
  uint32_t dry_since_ms;
  uint32_t steady_since_ms;
  uint32_t prev_tick_ms;

  bool active;
  bool was_dry;
  uint32_t start_ms;
  uint32_t dry_ms;
  int min_frames;
  uint32_t last_line_ms;
  uint32_t lines;

  volatile uint32_t episodes;
  volatile uint32_t dry_episodes;
  volatile uint32_t last_episode_ms;
  volatile uint32_t last_dry_ms;
  volatile bool episode_active;
} m;

static const char *reader_name(uint8_t s) {
  switch (s) {
  case BUFFERED_READER_RECV:
    return "recv";
  case BUFFERED_READER_SLOT_WAIT:
    return "slot_wait";
  case BUFFERED_READER_DEBUG_STALL:
    return "debug_stall";
  default:
    return "idle";
  }
}

static const char *decoder_name(uint8_t s) {
  switch (s) {
  case BUFFERED_DECODER_HOLD_ANCHOR:
    return "hold_anchor";
  case BUFFERED_DECODER_RING_FULL:
    return "ring_full";
  case BUFFERED_DECODER_DECODING:
    return "decoding";
  default:
    return "idle";
  }
}

static uint32_t age_ms(uint32_t now_ms, uint32_t then_ms) {
  return then_ms ? now_ms - then_ms : UINT32_MAX;
}

// Runs in the tcpip thread, the only place lwIP's PCBs may be looked at.
// The data connection is the active PCB on the buffered listener's port.
static void tcp_snapshot_cb(void *ctx) {
  audio_receiver_state_t *state = (audio_receiver_state_t *)ctx;
  tcp_snapshot_t snap = {0};
  uint16_t port = state->buffered_port;
  if (port != 0 && state->buffered_client_socket >= 0) {
    for (struct tcp_pcb *pcb = tcp_active_pcbs; pcb; pcb = pcb->next) {
      if (pcb->local_port != port) {
        continue;
      }
      snap.valid = true;
      snap.rcv_wnd = pcb->rcv_wnd;
      snap.ann_wnd = pcb->rcv_ann_wnd;
      snap.rcv_nxt = pcb->rcv_nxt;
      snap.taken_ms = rx_now_ms();
      snap.refused = pcb->refused_data != NULL;
#if TCP_QUEUE_OOSEQ
      for (struct tcp_seg *seg = pcb->ooseq; seg; seg = seg->next) {
        snap.ooseq_segs++;
        snap.ooseq_bytes += seg->len;
      }
#endif
      break;
    }
  }
  // Only this thread writes the numbers in s_tcp (the monitor just clears
  // `valid`), so the previous snapshot can be read unlocked.
  uint32_t span_ms = snap.taken_ms - s_tcp.taken_ms;
  if (snap.valid && s_tcp.valid && span_ms > 0) {
    snap.bytes_per_s =
        (uint32_t)((uint64_t)(snap.rcv_nxt - s_tcp.rcv_nxt) * 1000 / span_ms);
  }
  portENTER_CRITICAL(&s_mux);
  s_tcp = snap;
  portEXIT_CRITICAL(&s_mux);
}

static tcp_snapshot_t tcp_snapshot_get(void) {
  portENTER_CRITICAL(&s_mux);
  tcp_snapshot_t snap = s_tcp;
  portEXIT_CRITICAL(&s_mux);
  return snap;
}

void audio_receiver_get_rx_diag(audio_rx_diag_t *out) {
  memset(out, 0, sizeof(*out));
  out->stream = "none";
  out->reader = "idle";
  out->decoder = "idle";
  out->free_slots = -1;
  out->ready_packets = -1;
  out->last_packet_age_ms = UINT32_MAX;
  out->last_byte_age_ms = UINT32_MAX;
  out->decoder_age_ms = UINT32_MAX;
  audio_receiver_state_t *state = s_state;
  if (!state) {
    return;
  }
  uint32_t now_ms = rx_now_ms();
  audio_stream_t *stream = state->stream;
  bool running = stream && stream->running;
  bool buffered = running && stream->type == AUDIO_STREAM_BUFFERED;
  if (running) {
    out->stream = buffered ? "buffered" : "realtime";
  }
  out->ring_frames = audio_buffer_get_frame_count(&state->buffer);
  out->late_frames = state->stats.late_frames;
  if (buffered) {
    out->data_connected = state->buffered_client_socket >= 0;
    out->last_packet_age_ms = age_ms(now_ms, state->buffered_last_packet_ms);
    out->last_byte_age_ms = age_ms(now_ms, state->buffered_last_byte_ms);
    out->decoder_age_ms = age_ms(now_ms, state->buffered_decoder_progress_ms);
    out->reader = reader_name(state->buffered_reader_state);
    out->decoder = decoder_name(state->buffered_decoder_state);
    audio_stream_buffered_queue_depths(state, &out->free_slots,
                                       &out->ready_packets);
    uint32_t until_ms = state->buffered_debug_stall_until_ms;
    if (until_ms != 0 && (int32_t)(until_ms - now_ms) > 0) {
      out->debug_stall_left_ms = until_ms - now_ms;
    }
    tcp_snapshot_t tcp = tcp_snapshot_get();
    out->tcp_valid = tcp.valid;
    out->tcp_rcv_wnd = tcp.rcv_wnd;
    out->tcp_ann_wnd = tcp.ann_wnd;
    out->tcp_ooseq_segs = tcp.ooseq_segs;
    out->tcp_ooseq_bytes = tcp.ooseq_bytes;
    out->tcp_refused = tcp.refused;
    out->tcp_bytes_per_s = tcp.bytes_per_s;
  } else if (running) {
    out->last_packet_age_ms = age_ms(now_ms, m.last_packet_change_ms);
  }
  out->bytes_received = state->buffered_bytes_received;
  out->rx_bytes_per_s = m.rx_bytes_per_s;
  out->stall_timeouts = state->buffered_stall_timeouts;
  out->debug_stalls = state->buffered_debug_stalls;
  out->episodes = m.episodes;
  out->dry_episodes = m.dry_episodes;
  out->episode_active = m.episode_active;
  out->last_episode_ms = m.last_episode_ms;
  out->last_dry_ms = m.last_dry_ms;
}

// One line with everything; `what` says why it is being logged.
static void log_state(const char *what) {
  audio_rx_diag_t d;
  audio_receiver_get_rx_diag(&d);
  ptp_stats_t ps;
  ptp_clock_get_stats(&ps);
  int rssi = 0;
  esp_wifi_sta_get_rssi(&rssi);
  uint32_t now_ms = rx_now_ms();
  // -1 for "never" keeps the line short.
  ESP_LOGW(
      TAG,
      "%s +%" PRIu32 " ms dry=%" PRIu32 " ms: ring=%d min=%d acq=%d "
      "pkt_age=%ld byte_age=%ld rx=%" PRIu32 " B/s reader=%s free=%d "
      "ready=%d decoder=%s dec_age=%ld late=%" PRIu32 " | tcp wnd=%" PRIu32
      " ann=%" PRIu32 " ooseq=%" PRIu32 "/%" PRIu32 "B refused=%d in=%" PRIu32
      " B/s | ptp gap=%lld us age=%ld lock=%d | rssi=%d",
      what, now_ms - m.start_ms, m.dry_ms, d.ring_frames, m.min_frames,
      s_state->timing.acquired, (long)(int32_t)d.last_packet_age_ms,
      (long)(int32_t)d.last_byte_age_ms, d.rx_bytes_per_s, d.reader,
      d.free_slots, d.ready_packets, d.decoder, (long)(int32_t)d.decoder_age_ms,
      d.late_frames, d.tcp_rcv_wnd, d.tcp_ann_wnd, d.tcp_ooseq_segs,
      d.tcp_ooseq_bytes, d.tcp_refused, d.tcp_bytes_per_s,
      (long long)((ps.last_offset_ns - ps.filtered_offset_ns) / 1000LL),
      (long)(int32_t)ps.last_sync_age_ms, ptp_clock_is_locked(), rssi);
}

static void end_episode(uint32_t now_ms) {
  m.active = false;
  m.episode_active = false;
  m.last_episode_ms = now_ms - m.start_ms;
  m.last_dry_ms = m.dry_ms;
}

static void update_rates(audio_receiver_state_t *state, uint32_t now_ms) {
  uint32_t packets = state->stats.packets_received;
  if (packets != m.last_packets) {
    m.last_packets = packets;
    m.last_packet_change_ms = now_ms;
  }
  uint32_t span_ms = now_ms - m.rate_ms;
  if (span_ms < 1000) {
    return;
  }
  uint32_t bytes = state->buffered_bytes_received;
  m.rx_bytes_per_s =
      (uint32_t)((uint64_t)(bytes - m.rate_bytes) * 1000 / span_ms);
  m.rate_bytes = bytes;
  m.rate_ms = now_ms;

  // Ask for the next snapshot while a data connection exists.  One message a
  // second to the tcpip thread; if its mailbox is full the old snapshot
  // simply stays.  The message is allocated here, not at init: a connection
  // proves lwIP is up.
  if (state->buffered_client_socket < 0) {
    portENTER_CRITICAL(&s_mux);
    s_tcp.valid = false;
    portEXIT_CRITICAL(&s_mux);
    return;
  }
  if (!s_tcp_msg) {
    s_tcp_msg = tcpip_callbackmsg_new(tcp_snapshot_cb, state);
  }
  if (s_tcp_msg) {
    tcpip_callbackmsg_trycallback(s_tcp_msg);
  }
}

static void update_episode(audio_receiver_state_t *state, uint32_t now_ms) {
  audio_stream_t *stream = state->stream;
  bool running = stream && stream->running;
  bool buffered = running && stream->type == AUDIO_STREAM_BUFFERED;
  // "Playing" here means the sender has scheduled audio: rate 1, a valid
  // anchor, and not between a flush and its anchor.  Without a schedule an
  // empty ring is the sender's choice (Music pauses at a track change with a
  // flush and no rate=0), and the decoder's own anchor timeout reports it.
  bool playing = running && state->timing.playing &&
                 state->timing.anchor_valid && !state->discard_all_until_anchor;
  int frames = audio_buffer_get_frame_count(&state->buffer);
  bool dry = frames == 0 && !state->timing.pending_valid;
  uint32_t generation = state->buffered_generation;
  uint32_t tick_ms = now_ms - m.prev_tick_ms;
  m.prev_tick_ms = now_ms;

  // A pause, a seek or the end of the stream is the sender's decision, not a
  // dropout: whatever was going on ends here, and the ring starts over.
  if (!playing || generation != m.generation) {
    if (m.active) {
      log_state("Episode ended by pause, seek or stop");
      end_episode(now_ms);
    }
    m.generation = generation;
    m.armed = false;
    m.played = false;
    m.dry_since_ms = 0;
    m.steady_since_ms = 0;
    return;
  }

  if (buffered && frames >= RING_OK_FRAMES) {
    m.armed = true;
  }
  if (state->timing.acquired && frames > 0) {
    m.played = true;
  }
  if (!dry) {
    m.dry_since_ms = 0;
  } else if (m.dry_since_ms == 0) {
    m.dry_since_ms = now_ms;
  }
  bool low = buffered && m.armed && frames < RING_LOW_FRAMES;
  bool dry_long = dry && now_ms - m.dry_since_ms >=
                             (m.played ? DRY_GRACE_MS : DRY_GRACE_START_MS);

  if (!m.active) {
    if (!low && !dry_long) {
      return;
    }
    m.active = true;
    m.episode_active = true;
    m.episodes++;
    m.was_dry = false;
    m.start_ms = dry_long ? m.dry_since_ms : now_ms;
    m.dry_ms = dry_long ? now_ms - m.dry_since_ms : 0;
    m.min_frames = frames;
    m.steady_since_ms = 0;
    m.lines = 0;
    m.last_line_ms = now_ms;
    if (dry_long) {
      m.was_dry = true;
      m.dry_episodes++;
    }
    log_state(dry_long ? "Ring DRY while playing" : "Ring low while playing");
    return;
  }

  if (frames < m.min_frames) {
    m.min_frames = frames;
  }
  if (dry) {
    m.dry_ms += tick_ms;
    if (!m.was_dry) {
      m.was_dry = true;
      m.dry_episodes++;
      m.last_line_ms = now_ms;
      log_state("Ring DRY while playing");
    }
  }

  bool steady_level = buffered ? frames >= RING_LOW_FRAMES : !dry;
  if (!steady_level) {
    m.steady_since_ms = 0;
  } else if (m.steady_since_ms == 0) {
    m.steady_since_ms = now_ms;
  }
  bool recovered = (buffered && frames >= RING_OK_FRAMES) ||
                   (steady_level && now_ms - m.steady_since_ms >=
                                        (buffered ? STEADY_MS : 1000U));
  if (recovered) {
    log_state("Ring recovered");
    end_episode(now_ms);
    m.armed = buffered && frames >= RING_OK_FRAMES;
    m.steady_since_ms = 0;
    return;
  }

  // A line a second for the first 20 s, then every 10 s for two minutes,
  // then every minute: a session left "playing" with nothing to play must
  // not fill the journal.
  uint32_t interval_ms = m.lines < 20 ? 1000 : (m.lines < 32 ? 10000 : 60000);
  if (now_ms - m.last_line_ms >= interval_ms) {
    m.last_line_ms = now_ms;
    m.lines++;
    log_state(dry ? "Ring dry" : "Ring low");
  }
}

static void monitor_task(void *arg) {
  audio_receiver_state_t *state = (audio_receiver_state_t *)arg;
  m.prev_tick_ms = rx_now_ms();
  m.rate_ms = m.prev_tick_ms;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));
    uint32_t now_ms = rx_now_ms();
    update_rates(state, now_ms);
    update_episode(state, now_ms);
  }
}

void audio_rx_monitor_start(audio_receiver_state_t *state) {
  if (s_state) {
    return;
  }
  s_state = state;
  // Priority 2: below every audio and network task.  It only logs.
  if (xTaskCreate(monitor_task, "rx_monitor", 4096, state, 2, NULL) != pdPASS) {
    ESP_LOGW(TAG, "Dropout monitor task not started");
  }
}
