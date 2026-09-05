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
#define CFG_TUSB_OS_INC_PATH  freertos /
#define CFG_TUSB_DEBUG        0
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

#ifdef __cplusplus
}
#endif
