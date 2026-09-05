/**
 * USB descriptors: UAC2 stereo speaker (asynchronous, with feedback
 * endpoint) + HID boot keyboard used only to wake the host.
 *
 * Windows 10/11, macOS and Linux all ship class drivers for both, so the
 * device is driverless.  The configuration declares remote wakeup so a
 * suspended host (S3) can be resumed with tud_remote_wakeup().
 */

#include "usb_descriptors.h"

#include "esp_mac.h"
#include "tusb.h"
#include "uac_descriptors.h"

#include <stdio.h>
#include <string.h>

// Espressif's vendor id with a private product id: this is a personal
// device, not a distributed product.  (USB-IF never allocates PIDs under
// someone else's VID; if this ever ships, get a proper pair.)
#define USB_VID 0x303A
#define USB_PID 0xF5A1
#define USB_BCD 0x0510 // 5.1

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // Interface Association Descriptors need the "misc / common / IAD" class
    // triple at device level for Windows to bind the audio function.
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

static const uint8_t s_hid_report_descriptor[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return s_hid_report_descriptor;
}

#define USB_CONFIG_TOTAL_LEN \
  (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN + TUD_HID_DESC_LEN)

// String indices used inside the configuration descriptor.
enum {
  USB_STR_LANGID = 0,
  USB_STR_MANUFACTURER,
  USB_STR_PRODUCT,
  USB_STR_SERIAL,
  USB_STR_AUDIO_CONTROL,
  USB_STR_SPEAKER,
  USB_STR_HID_WAKE,
  USB_STR_COUNT,
};

static const uint8_t s_configuration_descriptor[] = {
    // 500 mA: the module and the DAC run from this port.
    TUD_CONFIG_DESCRIPTOR(1, USB_ITF_TOTAL, 0, USB_CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),
    // Audio function: control itf + streaming itf, strings 4 and 5.
    TUD_AUDIO_DESCRIPTOR(USB_ITF_AUDIO_CONTROL, USB_STR_AUDIO_CONTROL,
                         USB_EP_AUDIO_OUT, 0, USB_EP_AUDIO_FB),
    // HID keyboard (boot protocol) on its own interrupt IN endpoint, 5 ms.
    TUD_HID_DESCRIPTOR(USB_ITF_HID_WAKE, USB_STR_HID_WAKE,
                       HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_hid_report_descriptor),
                       USB_EP_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};

_Static_assert(sizeof(s_configuration_descriptor) == USB_CONFIG_TOTAL_LEN,
               "USB configuration descriptor length mismatch");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return s_configuration_descriptor;
}

static const char *const s_strings[USB_STR_COUNT] = {
    [USB_STR_LANGID] = NULL,
    [USB_STR_MANUFACTURER] = "Fable",
    [USB_STR_PRODUCT] = "Bedroom Speakers",
    [USB_STR_SERIAL] = NULL, // derived from the MAC
    [USB_STR_AUDIO_CONTROL] = "Bedroom Speakers Audio",
    [USB_STR_SPEAKER] = "Bedroom Speakers",
    [USB_STR_HID_WAKE] = "Bedroom Speakers PC Wake",
};

static uint16_t s_desc_str[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  size_t count;

  if (index == USB_STR_LANGID) {
    s_desc_str[1] = 0x0409; // English (US)
    count = 1;
  } else {
    if (index >= USB_STR_COUNT) {
      return NULL;
    }
    char serial[13];
    const char *value = s_strings[index];
    if (index == USB_STR_SERIAL) {
      uint8_t mac[6] = {0};
      esp_efuse_mac_get_default(mac);
      snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0],
               mac[1], mac[2], mac[3], mac[4], mac[5]);
      value = serial;
    }
    if (!value) {
      return NULL;
    }
    count = strlen(value);
    if (count > 32) {
      count = 32;
    }
    for (size_t i = 0; i < count; i++) {
      s_desc_str[1 + i] = (uint8_t)value[i];
    }
  }

  s_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
  return s_desc_str;
}

// HID: the host never reads or writes reports from a wake-only keyboard;
// LED (caps lock) output reports are ignored.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;
  return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)bufsize;
}
