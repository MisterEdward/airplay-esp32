/**
 * USB descriptors: UAC2 stereo speaker (asynchronous, with feedback
 * endpoint) + HID boot keyboard used only to wake the host.  With
 * CONFIG_USB_AUDIO_CAPTURE the audio function also carries a stereo input
 * (asynchronous IN) that streams what the DAC plays, for measurements.
 *
 * Windows 10/11, macOS and Linux all ship class drivers for both, so the
 * device is driverless.  The configuration declares remote wakeup so a
 * suspended host (S3) can be resumed with tud_remote_wakeup().
 */

#include "usb_descriptors.h"
#include "usb_audio_source.h"

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
// A new revision when the capture input changes the configuration, so a
// host that cached the old layout (interface numbers) enumerates afresh.
#if CONFIG_USB_AUDIO_CAPTURE
#define USB_BCD 0x0511
#else
#define USB_BCD 0x0510 // 5.1
#endif

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

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return s_hid_report_descriptor;
}

#if CONFIG_USB_AUDIO_CAPTURE
#define USB_AUDIO_DESC_LEN USB_AUDIO_FUNC_DESC_LEN
#else
#define USB_AUDIO_DESC_LEN TUD_AUDIO_DEVICE_DESC_LEN
#endif
#define USB_CONFIG_TOTAL_LEN \
  (TUD_CONFIG_DESC_LEN + USB_AUDIO_DESC_LEN + TUD_HID_DESC_LEN)

// String indices used inside the configuration descriptor.
enum {
  USB_STR_LANGID = 0,
  USB_STR_MANUFACTURER,
  USB_STR_PRODUCT,
  USB_STR_SERIAL,
  USB_STR_AUDIO_CONTROL,
  USB_STR_SPEAKER,
  USB_STR_HID_WAKE,
  USB_STR_CAPTURE,
  USB_STR_COUNT,
};

#if CONFIG_USB_AUDIO_CAPTURE
// External terminal type "digital audio interface" (UAC2 Termt20, 0x0602):
// the capture is the DAC feed, not a microphone, and hosts that special-case
// microphones (headset detection, voice processing) have no reason to.
#define USB_TERM_TYPE_EXT_DIGITAL 0x0602
#define USB_CAP_CHANNEL_CFG \
  (AUDIO_CHANNEL_CONFIG_FRONT_LEFT | AUDIO_CHANNEL_CONFIG_FRONT_RIGHT)

/*
 * The speaker half is byte for byte the UAC component's
 * TUD_AUDIO_SPEAK_DESCRIPTOR (same entities, controls, endpoints and
 * strings); the capture half is an input terminal wired straight to a USB
 * streaming output terminal.  No feature unit on purpose: the host gets no
 * volume or mute control that could alter the samples, and the UAC
 * component (which answers entity requests) would reject them anyway.
 */
// clang-format off
#define USB_AUDIO_SPK_CAP_DESCRIPTOR(_itfnum, _stridx, _epout, _epfb, _epin) \
  /* Interface association: control + speaker + capture */                   \
  TUD_AUDIO_DESC_IAD(_itfnum, 3, 0x00),                                      \
  TUD_AUDIO_DESC_STD_AC(_itfnum, 0x00, _stridx),                             \
  TUD_AUDIO_DESC_CS_AC(0x0200, AUDIO_FUNC_DESKTOP_SPEAKER,                   \
                       USB_AUDIO_FUNC_CS_AC_LEN,                             \
                       AUDIO_CS_AS_INTERFACE_CTRL_LATENCY_POS),              \
  TUD_AUDIO_DESC_CLK_SRC(UAC2_ENTITY_CLOCK, 3, 7, 0x00, 0x00),               \
  /* Speaker: USB streaming -> feature unit -> speaker */                    \
  TUD_AUDIO_DESC_INPUT_TERM(UAC2_ENTITY_SPK_INPUT_TERMINAL,                  \
      AUDIO_TERM_TYPE_USB_STREAMING, 0x00, UAC2_ENTITY_CLOCK,                \
      SPEAK_CHANNEL_NUM, AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00,          \
      (AUDIO_CTRL_R << AUDIO_IN_TERM_CTRL_CONNECTOR_POS), 0x00),             \
  TUD_AUDIO_DESC_FEATURE_UNIT_N_CHANNEL(                                     \
      TUD_AUDIO_DESC_SPK_FEATURE_UNIT_N_CHANNEL_LEN,                         \
      UAC2_ENTITY_SPK_FEATURE_UNIT, UAC2_ENTITY_SPK_INPUT_TERMINAL, 0x00,    \
      INPUT_CTRL),                                                           \
  TUD_AUDIO_DESC_OUTPUT_TERM(UAC2_ENTITY_SPK_OUTPUT_TERMINAL,                \
      AUDIO_TERM_TYPE_OUT_GENERIC_SPEAKER, 0x00,                             \
      UAC2_ENTITY_SPK_FEATURE_UNIT, UAC2_ENTITY_CLOCK, 0x0000, 0x00),        \
  /* Capture: DAC feed -> USB streaming */                                   \
  TUD_AUDIO_DESC_INPUT_TERM(USB_ENTITY_CAP_INPUT_TERMINAL,                   \
      USB_TERM_TYPE_EXT_DIGITAL, 0x00, UAC2_ENTITY_CLOCK, USB_CAP_CHANNELS,  \
      USB_CAP_CHANNEL_CFG, 0x00, 0x0000, 0x00),                              \
  TUD_AUDIO_DESC_OUTPUT_TERM(USB_ENTITY_CAP_OUTPUT_TERMINAL,                 \
      AUDIO_TERM_TYPE_USB_STREAMING, 0x00, USB_ENTITY_CAP_INPUT_TERMINAL,    \
      UAC2_ENTITY_CLOCK, 0x0000, 0x00),                                      \
  /* Speaker streaming interface: alt 0 idle, alt 1 data + feedback */       \
  TUD_AUDIO_DESC_STD_AS_INT(_itfnum + 1, 0x00, 0x00, _stridx + 1),           \
  TUD_AUDIO_DESC_STD_AS_INT(_itfnum + 1, 0x01, 0x02, _stridx + 1),           \
  TUD_AUDIO_DESC_CS_AS_INT(UAC2_ENTITY_SPK_INPUT_TERMINAL, AUDIO_CTRL_NONE,  \
      AUDIO_FORMAT_TYPE_I, AUDIO_DATA_FORMAT_TYPE_I_PCM, SPEAK_CHANNEL_NUM,  \
      AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),                            \
  TUD_AUDIO_DESC_TYPE_I_FORMAT(                                              \
      CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX,                   \
      CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX),                          \
  TUD_AUDIO_DESC_STD_AS_ISO_EP(_epout,                                       \
      (TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS |                \
       TUSB_ISO_EP_ATT_DATA),                                                \
      CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT, 1),                           \
  TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,\
      AUDIO_CTRL_NONE, AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC,     \
      0x0001),                                                               \
  TUD_AUDIO_DESC_STD_AS_ISO_FB_EP(_epfb, 4, 1),                              \
  /* Capture streaming interface: alt 0 idle, alt 1 async data IN */         \
  TUD_AUDIO_DESC_STD_AS_INT(_itfnum + 2, 0x00, 0x00, USB_STR_CAPTURE),       \
  TUD_AUDIO_DESC_STD_AS_INT(_itfnum + 2, 0x01, 0x01, USB_STR_CAPTURE),       \
  TUD_AUDIO_DESC_CS_AS_INT(USB_ENTITY_CAP_OUTPUT_TERMINAL, AUDIO_CTRL_NONE,  \
      AUDIO_FORMAT_TYPE_I, AUDIO_DATA_FORMAT_TYPE_I_PCM, USB_CAP_CHANNELS,   \
      USB_CAP_CHANNEL_CFG, 0x00),                                            \
  TUD_AUDIO_DESC_TYPE_I_FORMAT(USB_CAP_BYTES_PER_SAMP,                       \
                               USB_CAP_BYTES_PER_SAMP * 8),                  \
  TUD_AUDIO_DESC_STD_AS_ISO_EP(_epin,                                        \
      (TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS |                \
       TUSB_ISO_EP_ATT_DATA),                                                \
      USB_CAP_EP_SIZE, 1),                                                   \
  TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,\
      AUDIO_CTRL_NONE, AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED,    \
      0x0000)
// clang-format on
#endif

static const uint8_t s_configuration_descriptor[] = {
    // 500 mA: the module and the DAC run from this port.
    TUD_CONFIG_DESCRIPTOR(1, USB_ITF_TOTAL, 0, USB_CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),
#if CONFIG_USB_AUDIO_CAPTURE
    // Audio function: control + speaker + capture, strings 4, 5 and 7.
    USB_AUDIO_SPK_CAP_DESCRIPTOR(USB_ITF_AUDIO_CONTROL, USB_STR_AUDIO_CONTROL,
                                 USB_EP_AUDIO_OUT, USB_EP_AUDIO_FB,
                                 USB_EP_AUDIO_CAP),
#else
    // Audio function: control itf + streaming itf, strings 4 and 5.
    TUD_AUDIO_DESCRIPTOR(USB_ITF_AUDIO_CONTROL, USB_STR_AUDIO_CONTROL,
                         USB_EP_AUDIO_OUT, 0, USB_EP_AUDIO_FB),
#endif
    // HID keyboard (boot protocol) on its own interrupt IN endpoint, 5 ms.
    TUD_HID_DESCRIPTOR(USB_ITF_HID_WAKE, USB_STR_HID_WAKE,
                       HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_hid_report_descriptor), USB_EP_HID_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 5),
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
    [USB_STR_CAPTURE] = "Bedroom Speakers Capture",
};

static uint16_t s_desc_str[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  size_t count;

  if (index == USB_STR_LANGID) {
    s_desc_str[1] = 0x0409; // English (US)
    count = 1;
  } else {
    if (index == 0xEE) {
      // Microsoft OS string descriptor: only Windows asks for it.  We have
      // none (STALL), but the request itself identifies the host.
      usb_audio_source_note_windows_host();
      return NULL;
    }
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
