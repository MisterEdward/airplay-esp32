#pragma once

#include "esp_err.h"

typedef enum {
  USB_PC_DISCONNECTED = 0,
  USB_PC_ACTIVE,
  USB_PC_SUSPENDED,
} usb_pc_state_t;

esp_err_t usb_audio_source_init(void);
usb_pc_state_t usb_audio_source_get_pc_state(void);
