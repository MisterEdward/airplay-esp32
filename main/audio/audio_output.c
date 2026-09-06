/**
 * I2S output backend — the single render task that owns the DAC.
 *
 * Design
 * ------
 *  - The I2S channel is enabled once at init and NEVER disabled again.  Every
 *    transition (start, pause, seek, source switch) is shaped in software by
 *    the envelope; the DAC sees an uninterrupted bit clock and a continuous
 *    sample stream, so nothing can pop.  v0.2.0 disabled/enabled the channel
 *    on every flush, which both clicked and invalidated the DMA cursor.
 *
 *  - One block of look-ahead.  The render task holds the most recent block
 *    and writes the PREVIOUS one to DMA.  When a flush arrives, the held
 *    block is still in software and can be faded to zero before it is
 *    written, so even an abrupt seek ends with a short ramp instead of a
 *    truncated waveform.  The cost is one block (~8 ms) of extra latency,
 *    which audio_output_get_pipeline_us() reports to the timing engine, so
 *    scheduling accuracy is unaffected.
 *
 *  - Sources are pulled, not pushed.  AirPlay comes from the timing engine
 *    (audio_receiver_read_ex), the USB speaker from a registered callback.
 *    Both go through the same resample → envelope → channel-mode → LED path,
 *    so volume, fades and routing behave identically whatever is playing.
 *
 *  - The DMA completion ISR maintains a frame cursor.  The queue depth
 *    (submitted − sent) is interpolated inside the current descriptor using
 *    the time since the last completion, which turns a 0..5 ms sawtooth into
 *    a sub-millisecond estimate — that precision is what lets the timing
 *    engine align the first sample after a seek exactly.
 */

#include "audio_output.h"
#include "rtsp_server.h"

#include "audio_envelope.h"
#include "audio_receiver.h"
#include "audio_resample.h"
#include "dac.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "settings.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#ifdef CONFIG_DAC_TAS58XX
#include "dac_tas58xx.h"
#endif
#ifdef CONFIG_DAC_TAS57XX
#include "dac_tas57xx.h"
#endif

// SIDE NOTE; providing power from GPIO pins is capped ~20mA.
#if CONFIG_I2S_GND_IO >= 0
#define I2S_GND_PIN CONFIG_I2S_GND_IO
#endif
#if CONFIG_I2S_VCC_IO >= 0
#define I2S_VCC_PIN CONFIG_I2S_VCC_IO
#endif

#define TAG          "audio_output"
#define I2S_SCK_PIN  CONFIG_I2S_SCK_IO
#define I2S_BCK_PIN  CONFIG_I2S_BCK_IO
#define I2S_LRCK_PIN CONFIG_I2S_WS_IO
#define I2S_DOUT_PIN CONFIG_I2S_DO_IO
#define OUTPUT_RATE  CONFIG_OUTPUT_SAMPLE_RATE_HZ

// Nominal block the sources produce (one AAC/ALAC chunk).  A silence block
// written on source starvation has this length at the OUTPUT rate.
#define FRAME_SAMPLES 352
// Maximum frames one pull may return: the timing engine may hand back up to
// this much alignment silence in one call, or one chunk plus a servo sample.
#define READ_CAPACITY_FRAMES 1025

// DMA ring: total depth is I2S_DMA_DESC_NUM × I2S_DMA_FRAME_NUM frames.
// 8 × 256 = 2048 frames = 42.7 ms at 48 kHz.  Deep enough that an NVS write
// (cache disabled for tens of ms) or a web-server burst does not run it dry.
#define I2S_DMA_DESC_NUM  8
#define I2S_DMA_FRAME_NUM 256

// Fade lengths.  150 ms in feels like a HomePod coming to life rather than
// a switch being thrown; 100 ms out is short enough that a pause still
// feels immediate but long enough to be a clean ramp on bass-heavy content.
#define FADE_IN_MS  150
#define FADE_OUT_MS 100

/* Max output frames after resampling one input block. */
#define MAX_RESAMPLE_FRAMES \
  ((size_t)((READ_CAPACITY_FRAMES + 2) * ((double)OUTPUT_RATE / 44100) + 16))

#if CONFIG_FREERTOS_UNICORE
#define PLAYBACK_CORE 0
#else
#define PLAYBACK_CORE 1
#endif

static i2s_chan_handle_t tx_handle;
static volatile bool playback_running = false;
static TaskHandle_t playback_task_handle = NULL;
static volatile int source_rate = 44100;
static volatile bool resample_reinit_needed = false;
static volatile audio_channel_mode_t channel_mode = AUDIO_CHANNEL_STEREO;

/* Control plane → render task.  All 32-bit aligned scalars, written by the
 * RTSP/USB/web tasks and polled once per block by the render task.  Ordering
 * between them is not critical: each is a self-contained request. */
static volatile uint32_t s_flush_epoch;
static volatile bool s_pause_pending;
static volatile audio_source_t s_requested_source = AUDIO_SOURCE_AIRPLAY;
static volatile audio_source_t s_active_source = AUDIO_SOURCE_AIRPLAY;
static volatile int32_t s_volume_q15[3] = {32768, 32768, 32768};
static volatile bool s_volume_snap[3];
static audio_output_pull_fn s_external_pull;
static void *s_external_ctx;

/* Render state, owned by the render task. */
static audio_envelope_t s_env;
static size_t s_held_frames; // block held back for transition shaping
static audio_output_stats_t s_stats;

/* Live output cursor.  output_submitted_frames advances after a successful
 * i2s_channel_write(); output_sent_frames is advanced by the TX DMA
 * completion ISR, which also stamps the completion time so the depth can be
 * interpolated inside the current descriptor.
 *
 * auto_clear keeps the DMA clocking descriptors even when the writer stalls,
 * so sent can overtake submitted.  The excess is output time that was played
 * as silence and can never be recovered; it is folded into
 * output_lost_frames so that the queue depth stays non-negative and the
 * cursor keeps a stable meaning across a starvation episode. */
static uint64_t output_submitted_frames;
static uint64_t output_sent_frames;
static uint64_t output_lost_frames;
static int64_t output_last_sent_us;
static uint32_t output_underruns;
static portMUX_TYPE output_cursor_mux = portMUX_INITIALIZER_UNLOCKED;

static bool IRAM_ATTR audio_output_on_sent(i2s_chan_handle_t handle,
                                           i2s_event_data_t *event,
                                           void *user_ctx) {
  (void)handle;
  (void)user_ctx;
  if (event && event->size > 0) {
    portENTER_CRITICAL_ISR(&output_cursor_mux);
    output_sent_frames += event->size / (2U * sizeof(int16_t));
    output_last_sent_us = esp_timer_get_time();
    portEXIT_CRITICAL_ISR(&output_cursor_mux);
  }
  return false;
}

static void output_cursor_reset(void) {
  portENTER_CRITICAL(&output_cursor_mux);
  output_submitted_frames = 0;
  output_sent_frames = 0;
  output_lost_frames = 0;
  output_last_sent_us = 0;
  portEXIT_CRITICAL(&output_cursor_mux);
}

/* Frames queued in the DMA ring ahead of the next write, interpolated inside
 * the descriptor currently being clocked out. */
static uint32_t output_queued_frames(int64_t *sampled_us) {
  portENTER_CRITICAL(&output_cursor_mux);
  uint64_t submitted = output_submitted_frames;
  uint64_t lost = output_lost_frames;
  uint64_t sent = output_sent_frames;
  int64_t last_sent_us = output_last_sent_us;
  int64_t now_us = esp_timer_get_time();
  if (sent > submitted + lost) {
    /* The ring ran dry: rebase so queued reads 0 and remember how much
     * output time went out as silence. */
    output_lost_frames = sent - submitted;
    output_underruns++;
    portEXIT_CRITICAL(&output_cursor_mux);
    *sampled_us = now_us;
    return 0;
  }
  portEXIT_CRITICAL(&output_cursor_mux);
  *sampled_us = now_us;

  uint64_t queued = submitted + lost - sent;
  /* The ISR only reports whole descriptors.  Frames clocked out since the
   * last completion are already gone from the ring, so subtract them
   * (bounded by one descriptor — if the ISR is late the estimate stays
   * conservative). */
  uint64_t partial = 0;
  if (last_sent_us != 0 && now_us > last_sent_us) {
    partial = ((uint64_t)(now_us - last_sent_us) * OUTPUT_RATE) / 1000000ULL;
    if (partial > I2S_DMA_FRAME_NUM) {
      partial = I2S_DMA_FRAME_NUM;
    }
  }
  queued = queued > partial ? queued - partial : 0;
  const uint64_t ring = (uint64_t)I2S_DMA_DESC_NUM * I2S_DMA_FRAME_NUM;
  return queued > ring ? (uint32_t)ring : (uint32_t)queued;
}

static void dma_write(const int16_t *pcm, size_t frames) {
  size_t written = 0;
  if (i2s_channel_write(tx_handle, pcm, frames * 2 * sizeof(int16_t), &written,
                        portMAX_DELAY) == ESP_OK) {
    portENTER_CRITICAL(&output_cursor_mux);
    output_submitted_frames += written / (2U * sizeof(int16_t));
    portEXIT_CRITICAL(&output_cursor_mux);
  }
}

/* A bi-amp hybrid flow drives one output per crossover way, so there is no
 * left and right to pick from downstream — the DSP's input mixer makes the
 * selection instead, and the software downmix has to stand aside. */
bool audio_output_channel_mode_in_dsp(void) {
#ifdef CONFIG_DAC_TAS57XX
  return dac_tas57xx_has_input_mix();
#else
  return false;
#endif
}

static void push_channel_mode_to_dsp(audio_channel_mode_t mode) {
#ifdef CONFIG_DAC_TAS57XX
  dac_tas57xx_set_input_source(mode == AUDIO_CHANNEL_LEFT ? TAS57XX_INPUT_LEFT
                               : mode == AUDIO_CHANNEL_RIGHT
                                   ? TAS57XX_INPUT_RIGHT
                                   : TAS57XX_INPUT_MIX);
#else
  (void)mode;
#endif
}

// Apply the selected channel mode to an interleaved stereo buffer (L,R,...).
static void apply_channel_mode(int16_t *buf, size_t frames) {
  if (audio_output_channel_mode_in_dsp()) {
    return;
  }
  audio_channel_mode_t mode = channel_mode;
  if (mode == AUDIO_CHANNEL_STEREO) {
    return;
  }
  if (mode == AUDIO_CHANNEL_MONO) {
    for (size_t i = 0; i < frames; i++) {
      int16_t m = (int16_t)(((int32_t)buf[i * 2] + buf[i * 2 + 1]) / 2);
      buf[i * 2] = m;
      buf[i * 2 + 1] = m;
    }
    return;
  }
  size_t src = (mode == AUDIO_CHANNEL_RIGHT) ? 1 : 0;
  for (size_t i = 0; i < frames; i++) {
    int16_t s = buf[i * 2 + src];
    buf[i * 2] = s;
    buf[i * 2 + 1] = s;
  }
}

/* Linear ramp of a block to zero, used only on the block the render task is
 * holding when a flush arrives: the content after it is gone, so this is the
 * last ~8 ms of the old material.  A linear ramp is fine here — the block
 * is short and the alternative is a hard cut. */
static void fade_tail(int16_t *pcm, size_t frames) {
  if (frames < 2) {
    if (frames == 1) {
      pcm[0] = pcm[1] = 0;
    }
    return;
  }
  for (size_t i = 0; i < frames; i++) {
    int32_t gain =
        (int32_t)(((uint64_t)(frames - 1 - i) * 32768ULL) / (frames - 1));
    pcm[2 * i] = (int16_t)(((int32_t)pcm[2 * i] * gain) / 32768);
    pcm[2 * i + 1] = (int16_t)(((int32_t)pcm[2 * i + 1] * gain) / 32768);
  }
}

static int32_t current_volume_target(audio_source_t src) {
#ifdef CONFIG_DAC_CONTROLS_VOLUME
  (void)src;
  return 32768;
#else
  return s_volume_q15[src];
#endif
}

static void playback_task(void *arg) {
  (void)arg;
  // Deadline-sensitive scratch stays off the shared PSRAM cache.
  const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  int16_t *pcm = heap_caps_malloc(
      (size_t)READ_CAPACITY_FRAMES * 2 * sizeof(int16_t), caps);
  int16_t *resample_buf =
      heap_caps_malloc(MAX_RESAMPLE_FRAMES * 2 * sizeof(int16_t), caps);
  int16_t *held =
      heap_caps_malloc(MAX_RESAMPLE_FRAMES * 2 * sizeof(int16_t), caps);
  if (!pcm || !resample_buf || !held) {
    ESP_LOGE(TAG, "Failed to allocate render buffers");
    free(pcm);
    free(resample_buf);
    free(held);
    playback_running = false;
    playback_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  audio_envelope_init(&s_env, OUTPUT_RATE, FADE_IN_MS, FADE_OUT_MS);
  s_held_frames = 0;
  bool held_media = false;
  uint32_t seen_epoch = s_flush_epoch;
  audio_source_t active = s_requested_source;
  s_active_source = active;
  bool switch_pending = false;

  ESP_LOGI(TAG, "Render task up: rate=%d fade_in=%dms fade_out=%dms hold=1blk",
           OUTPUT_RATE, FADE_IN_MS, FADE_OUT_MS);

  while (playback_running) {
    if (resample_reinit_needed) {
      resample_reinit_needed = false;
      audio_resample_init((uint32_t)source_rate, OUTPUT_RATE, 2);
    }

    /* ---- control requests ------------------------------------------ */
    uint32_t epoch = s_flush_epoch;
    if (epoch != seen_epoch) {
      seen_epoch = epoch;
      if (s_held_frames && held_media) {
        fade_tail(held, s_held_frames);
        dma_write(held, s_held_frames);
        s_held_frames = 0;
      }
      held_media = false;
      audio_envelope_cut(&s_env);
      audio_resample_reset();
      s_pause_pending = false;
      s_stats.flushes++;
      ESP_LOGD(TAG, "Flush: epoch=%" PRIu32 " (clock continuous)", epoch);
    }

    if (s_pause_pending && !audio_envelope_is_silent(&s_env) &&
        audio_envelope_state(&s_env) != ENVELOPE_FADING_OUT) {
      audio_envelope_fade_out(&s_env);
      s_stats.fades_out++;
      ESP_LOGD(TAG, "Pause: fading out");
    }

    audio_source_t requested = s_requested_source;
    if (requested != active) {
      if (!switch_pending) {
        switch_pending = true;
        ESP_LOGI(TAG, "Source switch %d -> %d requested", active, requested);
      }
      if (!audio_envelope_is_silent(&s_env)) {
        audio_envelope_fade_out(&s_env);
      } else {
        // Old source is silent: hand over.  The resampler is only used by
        // the AirPlay path; reset it so the next stream starts clean.
        if (s_held_frames && held_media) {
          fade_tail(held, s_held_frames);
          dma_write(held, s_held_frames);
          s_held_frames = 0;
          held_media = false;
        }
        active = requested;
        s_active_source = active;
        switch_pending = false;
        audio_resample_reset();
        audio_envelope_set_volume_now(&s_env, current_volume_target(active));
        s_stats.source_switches++;
        ESP_LOGI(TAG, "Source switch complete: active=%d", active);
      }
    }

    /* ---- pull one block from the active source ---------------------- */
    size_t frames = 0;
    bool media = false;
    int16_t *play = pcm;
    if (active == AUDIO_SOURCE_AIRPLAY) {
      frames = audio_receiver_read_ex(pcm, READ_CAPACITY_FRAMES, &media);
      if (frames > 0 && audio_resample_is_active()) {
        frames = audio_resample_process(pcm, frames, resample_buf,
                                        MAX_RESAMPLE_FRAMES);
        play = resample_buf;
      }
    } else if (active == AUDIO_SOURCE_EXTERNAL && s_external_pull) {
      frames = s_external_pull(pcm, FRAME_SAMPLES, s_external_ctx);
      media = frames > 0;
    }
    if (frames == 0) {
      // Nothing to play: one block of silence keeps the DMA fed and paces
      // the loop (the write blocks until ring space frees).
      if (active != AUDIO_SOURCE_NONE) {
        s_stats.source_starved++;
      }
      frames = FRAME_SAMPLES;
      memset(pcm, 0, frames * 2 * sizeof(int16_t));
      play = pcm;
      media = false;
    }

    /* ---- envelope -------------------------------------------------- */
    if (media) {
      if (!s_pause_pending && !switch_pending &&
          audio_envelope_state(&s_env) != ENVELOPE_OPEN &&
          audio_envelope_state(&s_env) != ENVELOPE_FADING_IN) {
        if (s_volume_snap[active]) {
          s_volume_snap[active] = false;
          audio_envelope_set_volume_now(&s_env, current_volume_target(active));
        }
        audio_envelope_fade_in(&s_env);
        s_stats.fades_in++;
        ESP_LOGD(TAG, "Media after silence: fading in (src=%d)", active);
      }
      audio_envelope_apply(&s_env, play, frames, current_volume_target(active));
      apply_channel_mode(play, frames);
      s_stats.frames_rendered += frames;
    } else if (!audio_envelope_is_silent(&s_env)) {
      // Scheduled/alignment silence while the envelope is open: the ramp
      // state must not advance on synthetic zeros, and there is nothing to
      // scale.  Leave the envelope where it is; real media continues it —
      // UNLESS a pause or a source switch is waiting for the fade-out to
      // finish: nothing audible is left to fade, so close immediately or
      // the request would wait for media that may never come.
      if (s_pause_pending || switch_pending) {
        audio_envelope_cut(&s_env);
      }
    }

    // A pause fade that has completed: now really stop the receiver.  Doing
    // it here (not in the RTSP task) means the 100 ms of ramp were fed with
    // actual audio instead of being cut by playing=false.
    if (s_pause_pending && audio_envelope_is_silent(&s_env)) {
      s_pause_pending = false;
      audio_receiver_pause();
      ESP_LOGD(TAG, "Pause: fade complete, receiver paused");
    }

    /* ---- write the previously held block, hold this one -------------- */
    if (s_held_frames) {
      led_audio_feed(held, s_held_frames);
      dma_write(held, s_held_frames);
    }
    memcpy(held, play, frames * 2 * sizeof(int16_t));
    s_held_frames = frames;
    held_media = media;

    s_stats.active_source = active;
    s_stats.envelope_state = (int)audio_envelope_state(&s_env);
    s_stats.pause_pending = s_pause_pending;
  }

  // Leaving: drain the held block with a ramp so the DAC does not see a cut.
  if (s_held_frames) {
    if (held_media) {
      fade_tail(held, s_held_frames);
    }
    dma_write(held, s_held_frames);
    s_held_frames = 0;
  }
  free(pcm);
  free(resample_buf);
  free(held);
  playback_task_handle = NULL;
  vTaskDelete(NULL);
}

esp_err_t audio_output_init(void) {
  uint8_t saved_mode;
  if (settings_get_channel_mode(&saved_mode) == ESP_OK &&
      saved_mode <= AUDIO_CHANNEL_MONO) {
    channel_mode = (audio_channel_mode_t)saved_mode;
    ESP_LOGI(TAG, "Loaded channel mode: %d", saved_mode);
  }

  if (channel_mode != AUDIO_CHANNEL_STEREO &&
      audio_output_channel_mode_locked()) {
    ESP_LOGI(TAG, "Dual DAC output: ignoring saved channel mode");
    channel_mode = AUDIO_CHANNEL_STEREO;
  }
  push_channel_mode_to_dsp(channel_mode);

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
  chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
  // Zero each DMA descriptor after it is sent.  Without this, a writer
  // stall longer than the DMA ring makes the hardware REPLAY the stale ring
  // contents in a loop: a loud stutter, then a second discontinuity on
  // recovery.  With auto_clear an underrun degrades to plain silence.
  chan_cfg.auto_clear = true;

  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_handle, NULL), TAG,
                      "channel create failed");

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(OUTPUT_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_SCK_PIN,
              .bclk = I2S_BCK_PIN,
              .ws = I2S_LRCK_PIN,
              .dout = I2S_DOUT_PIN,
              .din = I2S_GPIO_UNUSED,
          },
  };
#ifdef I2S_GND_PIN
  gpio_reset_pin(I2S_GND_PIN);
  gpio_set_direction(I2S_GND_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(I2S_GND_PIN, 0);
#endif
#ifdef I2S_VCC_PIN
  gpio_reset_pin(I2S_VCC_PIN);
  gpio_set_direction(I2S_VCC_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(I2S_VCC_PIN, 1);
#endif

  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_handle, &std_cfg), TAG,
                      "std mode init failed");

  const i2s_event_callbacks_t callbacks = {
      .on_recv = NULL,
      .on_recv_q_ovf = NULL,
      .on_sent = audio_output_on_sent,
      .on_send_q_ovf = NULL,
  };
  ESP_RETURN_ON_ERROR(
      i2s_channel_register_event_callback(tx_handle, &callbacks, NULL), TAG,
      "event callback registration failed");
  output_cursor_reset();

  ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_handle), TAG,
                      "channel enable failed");
  ESP_LOGI(
      TAG, "I2S initialized: Rate=%u, DMA_Desc=%d, DMA_Frame=%d (%u ms)",
      (unsigned int)OUTPUT_RATE, I2S_DMA_DESC_NUM, I2S_DMA_FRAME_NUM,
      (unsigned)(I2S_DMA_DESC_NUM * I2S_DMA_FRAME_NUM * 1000 / OUTPUT_RATE));

  // MCLK/BCLK/LRCK are now running.  Some codecs need this edge to finish
  // their clock setup.
  dac_on_i2s_started();

  audio_resample_init(44100, OUTPUT_RATE, 2);

  return ESP_OK;
}

void audio_output_start(void) {
  if (playback_task_handle != NULL) {
    return; // already running
  }
  playback_running = true;
  // The DMA has been free-running since the last session (A2DP, or plain
  // silence), so the cursor carries an arbitrary submitted/sent skew.
  output_cursor_reset();
  // 6 KB: the pull path (timing engine + resampler glue + USB adapter) and
  // the diagnostic log formatting need more than v0.2.0's 4 KB with margin.
  if (xTaskCreatePinnedToCore(playback_task, "audio_play", 6144, NULL,
                              AUDIO_PLAYBACK_TASK_PRIORITY,
                              &playback_task_handle, PLAYBACK_CORE) != pdPASS) {
    playback_running = false;
    playback_task_handle = NULL;
    ESP_LOGE(TAG, "Cannot create render task");
  }
}

void audio_output_stop(void) {
  if (playback_task_handle == NULL) {
    return;
  }
  playback_running = false;
  int timeout = 40;
  while (playback_task_handle != NULL && timeout-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (playback_task_handle != NULL) {
    ESP_LOGW(TAG, "Render task did not exit within timeout");
  } else {
    ESP_LOGI(TAG, "Render task stopped");
  }
}

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  size_t written = 0;
  esp_err_t err = i2s_channel_write(tx_handle, data, bytes, &written, wait);
  portENTER_CRITICAL(&output_cursor_mux);
  output_submitted_frames += written / (2U * sizeof(int16_t));
  portEXIT_CRITICAL(&output_cursor_mux);
  return err;
}

void audio_output_set_sample_rate(uint32_t rate) {
  // Only safe to call when no writer task is actively using I2S (Bluetooth
  // calls this with the render task stopped).
  ESP_LOGI(TAG, "Setting sample rate to %" PRIu32 " Hz", rate);
  i2s_channel_disable(tx_handle);
  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
  output_cursor_reset();
  i2s_channel_enable(tx_handle);
  dac_on_i2s_started();
}

void audio_output_flush(void) {
  __atomic_add_fetch(&s_flush_epoch, 1, __ATOMIC_RELEASE);
}

void audio_output_pause(void) {
  s_pause_pending = true;
}

void audio_output_resume(void) {
  s_pause_pending = false;
}

void audio_output_register_external_source(audio_output_pull_fn fn, void *ctx) {
  s_external_ctx = ctx;
  s_external_pull = fn;
}

void audio_output_select_source(audio_source_t source) {
  if (source > AUDIO_SOURCE_EXTERNAL) {
    return;
  }
  s_requested_source = source;
}

audio_source_t audio_output_active_source(void) {
  return s_active_source;
}

void audio_output_set_source_volume(audio_source_t source, int32_t volume_q15,
                                    bool immediate) {
  if (source > AUDIO_SOURCE_EXTERNAL) {
    return;
  }
  if (volume_q15 < 0) {
    volume_q15 = 0;
  }
  if (volume_q15 > 32768) {
    volume_q15 = 32768;
  }
  s_volume_q15[source] = volume_q15;
  if (immediate) {
    s_volume_snap[source] = true;
  }
}

void audio_output_get_stats(audio_output_stats_t *out) {
  if (!out) {
    return;
  }
  *out = s_stats;
  out->dma_underruns = audio_output_get_underruns();
}

void audio_output_set_source_rate(int rate) {
  if (rate > 0 && rate != source_rate) {
    source_rate = rate;
    resample_reinit_needed = true;
  }
}

uint32_t audio_output_get_hardware_latency_us(void) {
  // Steady-state occupancy oscillates between (DESC_NUM - 1) and DESC_NUM
  // descriptors; model the midpoint.  Only used by backends without a live
  // cursor and for the advertised-latency diagnostic.
  return (uint32_t)((((uint64_t)(2 * I2S_DMA_DESC_NUM - 1) * I2S_DMA_FRAME_NUM *
                      1000000ULL) /
                     2) /
                    OUTPUT_RATE);
}

bool audio_output_get_pipeline_us(int64_t *now_us, uint32_t *pipeline_us) {
  int64_t sampled_us = 0;
  uint32_t queued = output_queued_frames(&sampled_us);
  if (now_us) {
    *now_us = sampled_us;
  }
  if (pipeline_us) {
    // The held block is written AFTER the block being requested is
    // produced, so it sits between "now" and the requested block's first
    // sample exactly like the DMA queue does.
    *pipeline_us =
        (uint32_t)(((uint64_t)(queued + s_held_frames) * 1000000ULL) /
                   OUTPUT_RATE);
  }
  return true;
}

uint32_t audio_output_get_underruns(void) {
  portENTER_CRITICAL(&output_cursor_mux);
  uint32_t n = output_underruns;
  portEXIT_CRITICAL(&output_cursor_mux);
  return n;
}

/* With two amplifiers the DAC configuration already fixes the routing, so a
 * channel selection on top of that would only mute a speaker. */
bool audio_output_channel_mode_locked(void) {
#ifdef CONFIG_DAC_TAS58XX
  if (dac_tas58xx_get_device_count() > 1) {
    return true;
  }
#endif
#ifdef CONFIG_DAC_TAS57XX
  if (dac_tas57xx_get_device_count() > 1) {
    return true;
  }
#endif
  return false;
}

audio_channel_mode_t audio_output_cycle_channel_mode(void) {
  if (audio_output_channel_mode_locked()) {
    return channel_mode;
  }
  audio_channel_mode_t next;
  switch (channel_mode) {
  case AUDIO_CHANNEL_STEREO:
    next = AUDIO_CHANNEL_LEFT;
    break;
  case AUDIO_CHANNEL_LEFT:
    next = AUDIO_CHANNEL_RIGHT;
    break;
  case AUDIO_CHANNEL_RIGHT:
    next = AUDIO_CHANNEL_MONO;
    break;
  default:
    next = AUDIO_CHANNEL_STEREO;
    break;
  }
  audio_output_set_channel_mode(next);
  return next;
}

void audio_output_set_channel_mode(audio_channel_mode_t mode) {
  if (audio_output_channel_mode_locked()) {
    return;
  }
  if (mode > AUDIO_CHANNEL_MONO) {
    mode = AUDIO_CHANNEL_STEREO;
  }
  channel_mode = mode;
  settings_set_channel_mode((uint8_t)mode);
  push_channel_mode_to_dsp(mode);
  ESP_LOGI(TAG, "Channel mode: %s",
           mode == AUDIO_CHANNEL_LEFT    ? "LEFT only"
           : mode == AUDIO_CHANNEL_RIGHT ? "RIGHT only"
           : mode == AUDIO_CHANNEL_MONO  ? "MONO (L+R)"
                                         : "STEREO");
}

audio_channel_mode_t audio_output_get_channel_mode(void) {
  return channel_mode;
}
