#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * USB speaker source: the PC plays into us over the native USB port and the
 * audio is pulled by the render task (audio_output.c) like any other source.
 *
 *   UAC rx ISR → usb_device_uac spk task → output_cb (memcpy only) → ring
 *   render task ← pull ← ring (with a one-sample clock adapter)
 *
 * The host's clock and our I2S crystal differ by tens of ppm.  The UAC
 * feedback endpoint keeps TinyUSB's own FIFO stable against the HOST's
 * frame clock, but nothing upstream sees our DAC clock, so our ring would
 * slowly fill or drain (≈1 sample/s at 20 ppm; an audible glitch roughly
 * once an hour).  The pull path therefore trims or duplicates one sample —
 * at the block's quietest point — whenever the smoothed ring depth leaves
 * its target band.  That is the same mechanism the AirPlay servo uses and
 * is inaudible.
 */

typedef enum {
  USB_HOST_DISCONNECTED = 0, // no VBUS / not enumerated (PC off)
  USB_HOST_ACTIVE,           // enumerated, bus active
  USB_HOST_SUSPENDED,        // enumerated, host asleep (S3)
} usb_host_state_t;

typedef struct {
  usb_host_state_t host_state;
  bool bus_connected;       // host has talked to us (SETUP seen) since attach
  bool mounted;             // host completed SET_CONFIGURATION
  bool streaming;           // audio data received within the last 500 ms
  bool remote_wakeup_armed; // host allowed remote wakeup when it suspended
  bool host_is_windows;     // host asked for the MS OS string (index 0xEE)
  char feedback_mode[8];    // "auto" | "windows" | "mac"
  bool feedback_10_14;      // format actually chosen at the last stream open
  uint32_t ring_frames;     // frames currently buffered
  uint32_t ring_target;     // target depth
  uint32_t underruns;       // pull found the ring empty while streaming
  uint32_t overruns;        // output_cb found the ring full
  uint32_t discarded_bytes; // bytes dropped while USB was not the source
  uint32_t packets;         // output_cb calls (host audio packets received)
  uint32_t trims;           // clock-adapter one-sample corrections
  int32_t volume_q15;
  bool muted;
  uint32_t mounts;
  uint32_t suspends;
} usb_audio_stats_t;

/** Bring up TinyUSB (composite UAC2 + HID) and register the pull source. */
esp_err_t usb_audio_source_init(void);

/** Tell the source whether the render task is pulling from it. */
void usb_audio_source_set_active(bool active);

usb_host_state_t usb_audio_source_host_state(void);
void usb_audio_source_get_stats(usb_audio_stats_t *out);

/**
 * Wake a suspended host over USB: remote-wakeup signalling, then a single
 * harmless Left-Ctrl press/release once the bus is back.  Returns ESP_OK if
 * the resume signal was sent, ESP_ERR_INVALID_STATE if the host is not
 * suspended (nothing to do), ESP_ERR_NOT_ALLOWED if the host did not permit
 * remote wakeup.
 */
esp_err_t usb_audio_source_wake_host(void);

/**
 * Feedback-endpoint format.  The USB spec says a full-speed device reports
 * Ff as 10.14 in three bytes; macOS insists on exactly that, while the
 * Windows UAC2 class driver wants 16.16 in four bytes.  "auto" picks
 * Windows when the host requested the Microsoft OS string descriptor
 * (index 0xEE, which only Windows does) and the spec format otherwise.
 * Takes effect the next time the host opens the streaming interface.
 */
esp_err_t usb_audio_source_set_feedback_mode(const char *mode);
/** Called from the string-descriptor callback when index 0xEE is seen. */
void usb_audio_source_note_windows_host(void);
