#pragma once

/**
 * Interface layout of the composite USB device.  The UAC2 speaker function
 * takes two interfaces (control + streaming); the HID keyboard used for PC
 * wake takes one.
 */
enum {
  USB_ITF_AUDIO_CONTROL = 0,
  USB_ITF_AUDIO_STREAMING_SPK,
  USB_ITF_HID_WAKE,
  USB_ITF_TOTAL,
};

#define USB_EP_AUDIO_OUT 0x01
#define USB_EP_AUDIO_FB  0x81
#define USB_EP_HID_IN    0x82
