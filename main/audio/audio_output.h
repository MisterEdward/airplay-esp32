#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"

/**
 * Priority of the playback task in every output backend.
 *
 * It MUST outrank every audio source task (realtime UDP receiver = 8,
 * control receiver = 7, buffered TCP reader = 5) because the source tasks
 * are pinned to the same core.  A source task that outranks playback starves
 * it during a receive burst; the DMA ring (~40 ms) then runs dry and
 * auto_clear emits silence, so wall-clock advances while no audio is
 * consumed and the playout position slips permanently late.  That was the
 * mechanism behind the realtime-stream drift in issue #122 — the buffered
 * path was unaffected only because its reader task sits at priority 5.
 *
 * Playback cannot starve the sources in return: it blocks on the DMA write
 * for all but a few hundred microseconds of each ~8 ms frame period.
 */
#define AUDIO_PLAYBACK_TASK_PRIORITY 9

/** Nominal render block and maximum request to the external pull source. */
#define AUDIO_OUTPUT_BLOCK_FRAMES 352

/**
 * Output channel mode. LEFT/RIGHT route the chosen source channel to both
 * speakers; MONO plays the (L+R)/2 downmix on both speakers; STEREO (default)
 * plays the normal left/right mix.
 */
typedef enum {
  AUDIO_CHANNEL_STEREO = 0,
  AUDIO_CHANNEL_LEFT,
  AUDIO_CHANNEL_RIGHT,
  AUDIO_CHANNEL_MONO,
} audio_channel_mode_t;

/**
 * Audio sources the render task can pull from.  Exactly one is active; the
 * arbiter (audio_arbiter.c) decides which, the render task performs the
 * hand-over with a fade-out of the old source and a fade-in of the new one.
 */
typedef enum {
  AUDIO_SOURCE_NONE = 0,
  AUDIO_SOURCE_AIRPLAY,  // pulled via audio_receiver_read_ex()
  AUDIO_SOURCE_EXTERNAL, // pulled via the registered callback (USB speaker)
} audio_source_t;

/**
 * Pull callback for the external source.  Must copy up to `max_frames`
 * interleaved stereo int16 frames AT THE OUTPUT RATE into `pcm` and return
 * the number of frames written (0 = nothing available: the render task
 * emits silence).  Called from the render task; must not block for longer
 * than a fraction of a frame.
 */
typedef size_t (*audio_output_pull_fn)(int16_t *pcm, size_t max_frames,
                                       void *ctx);

/** Render-task statistics for the status API and diagnostics. */
typedef struct {
  uint32_t dma_underruns;  // DMA clocked out descriptors nobody filled
  uint32_t source_starved; // pull returned 0 while a source was active
  uint32_t fades_in;
  uint32_t fades_out;
  uint32_t flushes;
  uint32_t source_switches;
  uint64_t frames_rendered; // media frames written since boot
  audio_source_t active_source;
  int envelope_state; // envelope_state_t
  bool pause_pending;
} audio_output_stats_t;

/** Initialize the audio output backend (I2S / SPDIF / USB UAC). */
esp_err_t audio_output_init(void);

/** Start the render task. */
void audio_output_start(void);

/** Stop the render task (for yielding I2S to Bluetooth A2DP). */
void audio_output_stop(void);

/**
 * Seek / track change: whatever is queued for the current source is stale.
 * The render task fades the block it holds to zero over that block, closes
 * the envelope, resets the resampler and cancels a pending pause.  The next
 * media block fades in.  The I2S clock keeps running throughout — there is
 * no channel disable/enable, so the DMA cursor stays valid and the DAC never
 * sees a clock glitch.
 */
void audio_output_flush(void);

/**
 * Pause with a fade-out.  The render task keeps pulling from the AirPlay
 * receiver while the envelope ramps down (~100 ms), then calls
 * audio_receiver_pause() itself.  Cancelled by audio_output_resume() or
 * audio_output_flush() if the sender changes its mind first (a seek is
 * "pause, flush, new anchor" on the wire, ~50-100 ms apart).
 */
void audio_output_pause(void);

/** Cancel a pending pause; media arriving afterwards fades back in. */
void audio_output_resume(void);

/** Register (or clear, with fn=NULL) the external pull source. */
void audio_output_register_external_source(audio_output_pull_fn fn, void *ctx);

/**
 * Ask the render task to switch sources.  If audio is playing the current
 * source fades out first; the new source fades in with its first media.
 */
void audio_output_select_source(audio_source_t source);

/** The source the render task is currently pulling from. */
audio_source_t audio_output_active_source(void);

/**
 * Set the volume for a source (Q15, 32768 = unity).  Ramped by the envelope
 * unless `immediate`, which snaps (used when a session starts so the first
 * sample is already at the remembered level).
 */
void audio_output_set_source_volume(audio_source_t source, int32_t volume_q15,
                                    bool immediate);

/** Snapshot of render statistics. */
void audio_output_get_stats(audio_output_stats_t *out);

/**
 * Write raw PCM data to the I2S output, bypassing the render task.
 * Used by Bluetooth A2DP when the AirPlay render task is stopped.
 */
esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait);

/** Change the I2S sample rate (Bluetooth only; the render task must be
 * stopped). */
void audio_output_set_sample_rate(uint32_t rate);

/**
 * Notify the output of the AirPlay source sample rate (from ANNOUNCE/SETUP).
 * The resampler is re-initialized if the rate changes.
 */
void audio_output_set_source_rate(int rate);

/**
 * Modelled I2S DMA pipeline latency in microseconds (steady-state ring
 * occupancy).  Fallback for backends without a completion cursor.
 */
uint32_t audio_output_get_hardware_latency_us(void);

/**
 * Sample the live output pipeline delay: how long from now until the first
 * sample of the NEXT block handed to the render task is heard.  Includes the
 * DMA ring occupancy (interpolated inside the current descriptor) and the
 * block the render task is holding back for transition shaping.
 *
 * @param now_us      out: esp_timer_get_time() sampled with the queue depth.
 * @param pipeline_us out: delay in microseconds.
 * @return false if the backend has no completion cursor.
 */
bool audio_output_get_pipeline_us(int64_t *now_us, uint32_t *pipeline_us);

/** Number of DMA underrun episodes since boot. */
uint32_t audio_output_get_underruns(void);

/** Cycle STEREO -> LEFT -> RIGHT -> MONO -> STEREO (persisted). */
audio_channel_mode_t audio_output_cycle_channel_mode(void);
void audio_output_set_channel_mode(audio_channel_mode_t mode);
audio_channel_mode_t audio_output_get_channel_mode(void);
bool audio_output_channel_mode_locked(void);
bool audio_output_channel_mode_in_dsp(void);
