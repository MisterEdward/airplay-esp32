#pragma once

#include "sdkconfig.h"

/**
 * Interface layout of the composite USB device.  The UAC2 function takes
 * two interfaces (control + speaker streaming), three with the debug
 * capture input; the HID keyboard used for PC wake follows it.  The audio
 * function's interfaces must be contiguous (one IAD), which is why the
 * capture interface pushes HID from 2 to 3.
 */
enum {
  USB_ITF_AUDIO_CONTROL = 0,
  USB_ITF_AUDIO_STREAMING_SPK,
#if CONFIG_USB_AUDIO_CAPTURE
  USB_ITF_AUDIO_STREAMING_CAP,
#endif
  USB_ITF_HID_WAKE,
  USB_ITF_TOTAL,
};

#define USB_EP_AUDIO_OUT 0x01
#define USB_EP_AUDIO_FB  0x81
#define USB_EP_HID_IN    0x82
#define USB_EP_AUDIO_CAP 0x83

#if CONFIG_USB_AUDIO_CAPTURE
// Entities of the capture path.  The speaker path keeps 0x01-0x03 and both
// share clock 0x04 (the UAC component answers the clock requests).
#define USB_ENTITY_CAP_INPUT_TERMINAL  0x11
#define USB_ENTITY_CAP_OUTPUT_TERMINAL 0x13

// Capture format: the render task's output, 48 kHz stereo 16-bit.
#define USB_CAP_CHANNELS       2
#define USB_CAP_BYTES_PER_SAMP 2
#define USB_CAP_FRAME_BYTES    (USB_CAP_CHANNELS * USB_CAP_BYTES_PER_SAMP)
// Asynchronous IN: TinyUSB's flow control sends 47, 48 or 49 frames per
// 1 ms frame, so the endpoint must fit 49.
#define USB_CAP_EP_SIZE \
  ((CONFIG_OUTPUT_SAMPLE_RATE_HZ / 1000 + 1) * USB_CAP_FRAME_BYTES)

// Length of the whole audio function (IAD included), for TinyUSB.
#define USB_AUDIO_FUNC_CS_AC_LEN                                    \
  (TUD_AUDIO_DESC_CLK_SRC_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN +     \
   TUD_AUDIO_DESC_SPK_FEATURE_UNIT_N_CHANNEL_LEN +                  \
   TUD_AUDIO_DESC_OUTPUT_TERM_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN + \
   TUD_AUDIO_DESC_OUTPUT_TERM_LEN)
// Speaker streaming interface: alt 0, alt 1 + data EP + feedback EP.
#define USB_AUDIO_SPK_AS_LEN                                             \
  (2 * TUD_AUDIO_DESC_STD_AS_INT_LEN + TUD_AUDIO_DESC_CS_AS_INT_LEN +    \
   TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN + \
   TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN + TUD_AUDIO_DESC_STD_AS_ISO_FB_EP_LEN)
// Capture streaming interface: alt 0, alt 1 + data EP.
#define USB_AUDIO_CAP_AS_LEN                                             \
  (2 * TUD_AUDIO_DESC_STD_AS_INT_LEN + TUD_AUDIO_DESC_CS_AS_INT_LEN +    \
   TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN + \
   TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN)
#define USB_AUDIO_FUNC_DESC_LEN                          \
  (TUD_AUDIO_DESC_IAD_LEN + TUD_AUDIO_DESC_STD_AC_LEN +  \
   TUD_AUDIO_DESC_CS_AC_LEN + USB_AUDIO_FUNC_CS_AC_LEN + \
   USB_AUDIO_SPK_AS_LEN + USB_AUDIO_CAP_AS_LEN)
#endif
