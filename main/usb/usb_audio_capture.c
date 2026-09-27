#include "usb_audio_capture.h"

#include "audio_output.h"
#include "audio_tap_ring.h"
#include "usb_descriptors.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tusb.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "usb_capture";

#define OUTPUT_RATE   CONFIG_OUTPUT_SAMPLE_RATE_HZ
#define FRAME_BYTES   USB_CAP_FRAME_BYTES
#define FRAMES_PER_MS (OUTPUT_RATE / 1000)

// Tap ring: 8192 frames = 171 ms at 48 kHz, 32 KiB in PSRAM.  Only the
// render task writes and only the pump reads, so PSRAM latency never sits
// in an ISR.
#define RING_FRAMES 8192

// Pump period: 1 ms while the host streams (one USB frame), 10 ms while
// idle, where it only watches for the host to start.
#define PUMP_STREAM_US 1000
#define PUMP_IDLE_US   10000

// Per tick the pump forwards at most one packet's worth plus one frame: the
// render task delivers ~8 ms blocks (352-383 frames), and passing them on
// at 49 frames/ms smooths that sawtooth out of the FIFO level TinyUSB's
// flow control steers by, while still outrunning any I2S clock.  A backlog
// beyond one such block (a late pump, a long alignment-silence block) is
// forwarded at once.
#define PUMP_MAX_PER_TICK   (FRAMES_PER_MS + 1)
#define PUMP_BACKLOG_FRAMES 512

// FIFO level the pump keeps when it supplies silence (idle priming, tap
// off): half the FIFO, the flow control's target, so the host gets
// nominal 48-frame packets from the first one.
#define FIFO_TARGET_BYTES (CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 2)
// Below one minimum (47-frame) packet TinyUSB sends an empty one.
#define FIFO_LOW_BYTES ((FRAMES_PER_MS - 1) * FRAME_BYTES)
// A streaming host that has not taken anything for this long is gone.
#define STALL_TICKS 100

#define CHUNK_FRAMES 256

static audio_tap_ring_t s_ring;
static esp_timer_handle_t s_pump;
static volatile bool s_enabled;
static volatile bool s_streaming; // written by the pump only
static uint16_t s_last_count;     // FIFO bytes after the last pump tick
static uint32_t s_stall_ticks;
static bool s_low;
static usb_audio_capture_stats_t s_stats;
static int16_t s_chunk[CHUNK_FRAMES * 2];
static const int16_t s_zeros[64 * 2];

/* ------------------------------------------------------------------ */
/*  Render side                                                        */
/* ------------------------------------------------------------------ */

// Called by the render task with every block given to the DAC.  Copy or
// drop, never wait.
static void capture_tap(const int16_t *pcm, size_t frames) {
  if (!s_enabled || !s_streaming) {
    return;
  }
  size_t n = audio_tap_ring_write(&s_ring, pcm, frames);
  s_stats.tapped_frames += (uint32_t)n;
  if (n < frames) {
    s_stats.dropped_frames += (uint32_t)(frames - n);
  }
}

/* ------------------------------------------------------------------ */
/*  Pump (esp_timer task)                                              */
/* ------------------------------------------------------------------ */

static void fifo_write_zeros(tu_fifo_t *ff, uint32_t frames) {
  const uint32_t zero_frames = sizeof(s_zeros) / FRAME_BYTES;
  while (frames > 0) {
    uint32_t n = frames < zero_frames ? frames : zero_frames;
    tu_fifo_write_n(ff, s_zeros, (uint16_t)(n * FRAME_BYTES));
    s_stats.silence_frames += n;
    frames -= n;
  }
}

static void fifo_forward(tu_fifo_t *ff, uint32_t frames) {
  while (frames > 0) {
    uint32_t want = frames < CHUNK_FRAMES ? frames : CHUNK_FRAMES;
    size_t got = audio_tap_ring_read(&s_ring, s_chunk, want);
    if (got == 0) {
      return;
    }
    tu_fifo_write_n(ff, s_chunk, (uint16_t)(got * FRAME_BYTES));
    s_stats.sent_frames += (uint32_t)got;
    frames -= (uint32_t)got;
  }
}

// Silence up to the flow-control target.  Only the pump writes the FIFO,
// and never more than is free, so it cannot overrun the reader (the IN
// endpoint ISR).
static void fifo_top_up(tu_fifo_t *ff) {
  uint16_t count = tu_fifo_count(ff);
  if (count < FIFO_TARGET_BYTES) {
    fifo_write_zeros(ff, (FIFO_TARGET_BYTES - count) / FRAME_BYTES);
  }
}

static void stream_start(void) {
  audio_tap_ring_discard(&s_ring);
  s_stall_ticks = 0;
  s_low = false;
  s_streaming = true; // the render task taps from its next block
  s_stats.streams++;
  esp_timer_restart(s_pump, PUMP_STREAM_US);
  ESP_LOGI(TAG, "Host started capture stream #%" PRIu32 " (tap %s)",
           s_stats.streams, s_enabled ? "on" : "off: sending silence");
}

static void stream_stop(const char *why) {
  s_streaming = false;
  esp_timer_restart(s_pump, PUMP_IDLE_US);
  ESP_LOGI(TAG,
           "Host stopped capture stream (%s): tapped=%" PRIu32 " sent=%" PRIu32
           " silence=%" PRIu32 " dropped=%" PRIu32 " underruns=%" PRIu32,
           why, s_stats.tapped_frames, s_stats.sent_frames,
           s_stats.silence_frames, s_stats.dropped_frames, s_stats.underruns);
}

// Idle: keep half a FIFO of silence queued and watch it.  While the
// interface is closed nobody reads the FIFO, so the level only falls when
// the host has opened it and taken a packet.  Zero is not a start: TinyUSB
// empties the FIFO on close and on bus reset, and a host that opened it
// cannot drain 4 KiB (21 ms) between two 10 ms polls.
static void pump_idle(tu_fifo_t *ff) {
  audio_tap_ring_discard(&s_ring);
  uint16_t count = tu_fifo_count(ff);
  if (count > 0 && count < s_last_count) {
    stream_start();
    s_last_count = count;
    return;
  }
  fifo_top_up(ff);
  s_last_count = tu_fifo_count(ff);
}

static void pump_stream(tu_fifo_t *ff) {
  uint16_t count = tu_fifo_count(ff);
  if (count == 0 && s_last_count >= FIFO_TARGET_BYTES / 2) {
    // Half a FIFO cannot drain in one tick: TinyUSB emptied it because the
    // host closed the interface (alt 0) or reset the bus.  (The close
    // callback itself belongs to the UAC component.)
    stream_stop("interface closed");
    s_last_count = 0;
    return;
  }
  if (count < s_last_count) {
    s_stall_ticks = 0;
  } else if (count >= FIFO_LOW_BYTES && ++s_stall_ticks >= STALL_TICKS) {
    // Data was there to take and the host did not: it stopped polling
    // without closing the interface (suspend, unplug, a crashed app).
    stream_stop("host stopped reading");
    s_last_count = count;
    return;
  }
  bool low = count < FIFO_LOW_BYTES;
  if (low && !s_low) {
    s_stats.underruns++;
  }
  s_low = low;

  if (s_enabled) {
    uint32_t space = tu_fifo_remaining(ff) / FRAME_BYTES;
    uint32_t avail = audio_tap_ring_level(&s_ring);
    uint32_t n = avail < PUMP_MAX_PER_TICK ? avail : PUMP_MAX_PER_TICK;
    if (avail - n > PUMP_BACKLOG_FRAMES) {
      n = avail - PUMP_BACKLOG_FRAMES;
    }
    if (n > space) {
      n = space;
    }
    fifo_forward(ff, n);
  } else {
    // Tap off: keep the stream alive with silence.  Whatever the tap left
    // behind when it was switched off is stale by now.
    audio_tap_ring_discard(&s_ring);
    fifo_top_up(ff);
  }
  s_last_count = tu_fifo_count(ff);
}

static void pump_cb(void *arg) {
  (void)arg;
  tu_fifo_t *ff = tud_mounted() ? tud_audio_n_get_ep_in_ff(0) : NULL;
  if (!ff) {
    if (s_streaming) {
      stream_stop("bus down");
    }
    s_last_count = 0;
    audio_tap_ring_discard(&s_ring);
    return;
  }
  if (s_streaming) {
    pump_stream(ff);
  } else {
    pump_idle(ff);
  }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t usb_audio_capture_init(void) {
  if (s_pump) {
    return ESP_OK;
  }
  int16_t *buf = heap_caps_calloc(RING_FRAMES, FRAME_BYTES,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    buf = heap_caps_calloc(RING_FRAMES, FRAME_BYTES, MALLOC_CAP_8BIT);
  }
  if (!buf || !audio_tap_ring_init(&s_ring, buf, RING_FRAMES)) {
    ESP_LOGE(TAG, "No memory for the capture ring");
    return ESP_ERR_NO_MEM;
  }
  const esp_timer_create_args_t args = {.callback = pump_cb,
                                        .name = "usb_capture"};
  esp_err_t err = esp_timer_create(&args, &s_pump);
  if (err == ESP_OK) {
    err = esp_timer_start_periodic(s_pump, PUMP_IDLE_US);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Pump timer: %s", esp_err_to_name(err));
    return err;
  }
  audio_output_set_tap(capture_tap);
  ESP_LOGI(TAG,
           "USB capture ready: UAC2 input %d Hz stereo 16-bit, ring=%d ms, "
           "fifo=%d ms, tap off",
           OUTPUT_RATE, RING_FRAMES * 1000 / OUTPUT_RATE,
           CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / FRAME_BYTES * 1000 /
               OUTPUT_RATE);
  return ESP_OK;
}

void usb_audio_capture_set_enabled(bool enabled) {
  if (enabled == s_enabled) {
    return;
  }
  s_enabled = enabled;
  ESP_LOGI(TAG, "Capture tap %s (host %s)", enabled ? "ON" : "off",
           s_streaming ? "streaming" : "not streaming");
}

bool usb_audio_capture_enabled(void) {
  return s_enabled;
}

void usb_audio_capture_get_stats(usb_audio_capture_stats_t *out) {
  if (!out) {
    return;
  }
  *out = s_stats;
  out->enabled = s_enabled;
  out->host_streaming = s_streaming;
  out->ring_frames = s_pump ? audio_tap_ring_level(&s_ring) : 0;
  tu_fifo_t *ff = tud_mounted() ? tud_audio_n_get_ep_in_ff(0) : NULL;
  out->fifo_frames = ff ? tu_fifo_count(ff) / FRAME_BYTES : 0;
}
