#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * USB debug capture: the host records, bit-exact, every block the render
 * task hands to the DAC (CONFIG_USB_AUDIO_CAPTURE).
 *
 *   render task ─dma_write()─▶ tap ─▶ tap ring (PSRAM, ~170 ms, lock-free)
 *                                         │ pump, esp_timer, 1 ms
 *                                         ▼
 *                  TinyUSB IN FIFO (8 KiB) ─▶ isochronous IN ─▶ host
 *
 * The IN endpoint is asynchronous: TinyUSB's flow control sends 47, 48 or
 * 49 frames per USB frame so that its FIFO stays half full, i.e. the host
 * follows OUR I2S clock and nothing is dropped or duplicated for clock
 * drift.  Frames are only lost if the host stops reading for longer than
 * the buffers hold; `dropped_frames` counts that.
 *
 * Host streaming is detected from the FIFO itself (TinyUSB's set-interface
 * callbacks belong to the UAC component): while idle the pump keeps half a
 * FIFO of silence queued, and the first packet the host takes from it marks
 * the start of a stream.  Closing the interface (alt 0) makes TinyUSB empty
 * the FIFO, which ends it at once; a host that just stops polling ends it
 * after 100 ms.
 *
 * The tap only copies while it is enabled AND the host is streaming; with
 * the tap off a streaming host receives silence.  Off at boot.
 */

typedef struct {
  bool enabled;            // tap switch (runtime, off at boot)
  bool host_streaming;     // host has the capture interface open and reads
  uint32_t streams;        // stream starts seen since boot
  uint32_t tapped_frames;  // frames copied by the render task
  uint32_t dropped_frames; // render blocks that did not fit (host too slow)
  uint32_t sent_frames;    // tap frames handed to TinyUSB
  uint32_t silence_frames; // filler frames (tap off / idle priming)
  uint32_t underruns;      // FIFO fell below one packet while streaming
  uint32_t ring_frames;    // tap ring level now
  uint32_t fifo_frames;    // TinyUSB IN FIFO level now
} usb_audio_capture_stats_t;

/** Create the tap ring and the pump, register the render tap.  Call after
 * the TinyUSB stack is up. */
esp_err_t usb_audio_capture_init(void);

/** Switch the tap on or off (logged). */
void usb_audio_capture_set_enabled(bool enabled);
bool usb_audio_capture_enabled(void);

void usb_audio_capture_get_stats(usb_audio_capture_stats_t *out);
