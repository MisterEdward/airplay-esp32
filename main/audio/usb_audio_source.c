#include "usb_audio_source.h"

#include "usb_audio_descriptors.h"
#include "usb_device_uac.h"

#include "esp_log.h"
#include "tusb.h"

static const char *TAG = "usb_audio";

static volatile usb_pc_state_t s_pc_state = USB_PC_DISCONNECTED;
static volatile bool s_remote_wakeup_allowed;

void tud_mount_cb(void) {
  s_pc_state = USB_PC_ACTIVE;
  ESP_LOGI(TAG, "PC state: active");
}

void tud_umount_cb(void) {
  s_pc_state = USB_PC_DISCONNECTED;
  s_remote_wakeup_allowed = false;
  ESP_LOGI(TAG, "PC state: disconnected/off");
}

void tud_suspend_cb(bool remote_wakeup_en) {
  s_pc_state = USB_PC_SUSPENDED;
  s_remote_wakeup_allowed = remote_wakeup_en;
  ESP_LOGI(TAG, "PC state: suspended, remote_wakeup=%d", remote_wakeup_en);
}

void tud_resume_cb(void) {
  s_pc_state = tud_mounted() ? USB_PC_ACTIVE : USB_PC_DISCONNECTED;
  s_remote_wakeup_allowed = false;
  ESP_LOGI(TAG, "PC state: %s",
           s_pc_state == USB_PC_ACTIVE ? "active" : "disconnected/off");
}

esp_err_t usb_audio_source_init(void) {
  uac_device_config_t config = {
      .skip_tinyusb_init = false,
      .output_cb = NULL,
      .input_cb = NULL,
      .set_mute_cb = NULL,
      .set_volume_cb = NULL,
      .cb_ctx = NULL,
      .spk_itf_num = USB_ITF_AUDIO_STREAMING_SPK,
      .mic_itf_num = -1,
  };

  ESP_LOGI(TAG, "Starting USB UAC2 speaker + HID wake interface");
  esp_err_t err = uac_device_init(&config);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "USB composite device ready");
  }
  return err;
}

usb_pc_state_t usb_audio_source_get_pc_state(void) {
  return s_pc_state;
}
