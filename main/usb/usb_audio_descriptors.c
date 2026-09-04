#include "usb_audio_descriptors.h"

#include "esp_mac.h"
#include "tusb.h"
#include "uac_descriptors.h"

#include <stdio.h>
#include <string.h>

#define USB_VID 0x303A
// Development PID for this personal device. It is not an allocated USB-IF
// product ID and must be replaced before distributing hardware commercially.
#define USB_PID 0x82A1
#define USB_BCD 0x0100

#define USB_EP_AUDIO_OUT 0x01
#define USB_EP_AUDIO_FB  0x81
#define USB_EP_HID_IN    0x82

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = USB_BCD,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *)&s_device_descriptor;
}

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return s_hid_report_descriptor;
}

#define USB_CONFIG_TOTAL_LEN \
  (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, USB_ITF_TOTAL, 0, USB_CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_AUDIO_DESCRIPTOR(USB_ITF_AUDIO_CONTROL, 4, USB_EP_AUDIO_OUT, 0,
                         USB_EP_AUDIO_FB),
    TUD_HID_DESCRIPTOR(USB_ITF_HID_WAKE, 6, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_hid_report_descriptor), USB_EP_HID_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 5),
};

_Static_assert(sizeof(s_configuration_descriptor) == USB_CONFIG_TOTAL_LEN,
               "USB configuration descriptor length mismatch");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return s_configuration_descriptor;
}

enum {
  USB_STR_LANGID = 0,
  USB_STR_MANUFACTURER,
  USB_STR_PRODUCT,
  USB_STR_SERIAL,
  USB_STR_AUDIO_CONTROL,
  USB_STR_SPEAKER,
  USB_STR_HID_WAKE,
};

static const char *const s_string_descriptors[] = {
    NULL,      "MisterEdward", "Bedroom Speakers USB",
    NULL,      "USB Audio",    "Bedroom Speakers",
    "PC Wake",
};

static uint16_t s_string_descriptor[32];
static char s_serial[13];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  size_t count;

  if (index == USB_STR_LANGID) {
    s_string_descriptor[1] = 0x0409;
    count = 1;
  } else {
    if (index >=
        sizeof(s_string_descriptors) / sizeof(s_string_descriptors[0])) {
      return NULL;
    }

    const char *value = s_string_descriptors[index];
    if (index == USB_STR_SERIAL) {
      uint8_t mac[6];
      if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        return NULL;
      }
      snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0],
               mac[1], mac[2], mac[3], mac[4], mac[5]);
      value = s_serial;
    }

    count = strlen(value);
    if (count > 31) {
      count = 31;
    }
    for (size_t i = 0; i < count; ++i) {
      s_string_descriptor[1 + i] = (uint8_t)value[i];
    }
  }

  s_string_descriptor[0] =
      (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
  return s_string_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t requested_len) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)requested_len;
  return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t buffer_size) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)buffer_size;
}
