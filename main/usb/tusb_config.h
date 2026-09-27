#pragma once

/**
 * TinyUSB configuration for the composite "USB speaker + wake keyboard"
 * device.  CONFIG_USB_DEVICE_UAC_AS_PART=y hands descriptor ownership to the
 * application (usb_descriptors.c); the audio class settings themselves come
 * from Espressif's usb_device_uac component headers so the speaker path is
 * configured exactly the way its rx/feedback code expects.
 */

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error "CFG_TUSB_MCU must be defined by the build (see main/CMakeLists.txt)"
#endif

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_OS           OPT_OS_FREERTOS
// clang-format off
#define CFG_TUSB_OS_INC_PATH  freertos/
// clang-format on
// TinyUSB's own diagnostics (level 1: errors, class request rejections,
// interface open/close) go to the wireless journal via usb_tusb_printf().
#define CFG_TUSB_DEBUG        1
#define CFG_TUSB_DEBUG_PRINTF usb_tusb_printf
#define ESP_PLATFORM          1

#define CFG_TUD_ENABLED        1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

// Class drivers: audio (from the UAC component headers below) + HID.
#define CFG_TUD_HID            1
#define CFG_TUD_CDC            0
#define CFG_TUD_MSC            0
#define CFG_TUD_MIDI           0
#define CFG_TUD_VENDOR         0
#define CFG_TUD_HID_EP_BUFSIZE 16

#include "uac_config.h"
#include "uac_descriptors.h"
#include "tusb_config_uac.h"

#if CONFIG_USB_AUDIO_CAPTURE
/*
 * Debug capture input (usb_audio_capture.c).  The UAC component's
 * microphone path stays compiled out (CONFIG_UAC_MIC_CHANNEL_NUM=0); only
 * TinyUSB's IN side is sized here, on top of the component's settings.
 * The frame and endpoint sizes derive from N_CHANNELS_TX in
 * tusb_config_uac.h, so redefining it is enough for those.
 */
#include "usb_descriptors.h"

#undef CFG_TUD_AUDIO_FUNC_1_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN USB_AUDIO_FUNC_DESC_LEN
#undef CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX USB_CAP_CHANNELS
#undef CFG_TUD_AUDIO_FUNC_1_N_AS_INT
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT 2
// Software FIFO in front of the IN endpoint: 8 KiB = 2048 frames = 42.7 ms.
// TinyUSB's flow control keeps it near half full by sending 47/48/49-frame
// packets, which is what matches our I2S clock to the host's; the half that
// is free absorbs the pump's scheduling jitter.
#undef CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ 8192
#ifndef __cplusplus
_Static_assert(USB_CAP_EP_SIZE == CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN,
               "capture endpoint size mismatch");
#endif
#endif

#ifdef __cplusplus
}
#endif
