#include "pc_wake.h"

#include "settings.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef CONFIG_USB_AUDIO_SOURCE
#include "usb_audio_source.h"
#endif

static const char *TAG = "pc_wake";

#define WOL_PORT    9
#define WOL_REPEATS 3

bool pc_wake_parse_mac(const char *text, uint8_t mac[6]) {
  if (!text) {
    return false;
  }
  uint8_t out[6];
  int n = 0;
  int nibble = -1;
  for (const char *p = text; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == ':' || c == '-' || c == ' ' || c == '.') {
      if (nibble >= 0) {
        return false; // odd number of hex digits in a group
      }
      continue;
    }
    if (!isxdigit(c)) {
      return false;
    }
    int v = isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10);
    if (nibble < 0) {
      nibble = v;
    } else {
      if (n >= 6) {
        return false;
      }
      out[n++] = (uint8_t)((nibble << 4) | v);
      nibble = -1;
    }
  }
  if (n != 6 || nibble >= 0) {
    return false;
  }
  // All-zero and broadcast are not host addresses.
  bool all_zero = true;
  bool all_ff = true;
  for (int i = 0; i < 6; i++) {
    all_zero = all_zero && out[i] == 0;
    all_ff = all_ff && out[i] == 0xFF;
  }
  if (all_zero || all_ff) {
    return false;
  }
  memcpy(mac, out, 6);
  return true;
}

void pc_wake_format_mac(const uint8_t mac[6], char *out, size_t out_size) {
  snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t send_to(int sock, const uint8_t *pkt, size_t len,
                         uint32_t addr_be) {
  struct sockaddr_in dst = {0};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(WOL_PORT);
  dst.sin_addr.s_addr = addr_be;
  ssize_t r = sendto(sock, pkt, len, 0, (struct sockaddr *)&dst, sizeof(dst));
  return r == (ssize_t)len ? ESP_OK : ESP_FAIL;
}

esp_err_t pc_wake_send_wol(const uint8_t mac[6]) {
  uint8_t pkt[6 + 16 * 6];
  memset(pkt, 0xFF, 6);
  for (int i = 0; i < 16; i++) {
    memcpy(pkt + 6 + i * 6, mac, 6);
  }

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "socket failed: %d", errno);
    return ESP_FAIL;
  }
  int on = 1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

  // Limited broadcast plus the directed subnet broadcast of the active
  // interface: some routers only forward one of the two.
  uint32_t subnet_bcast = 0;
  esp_netif_t *netif = esp_netif_get_default_netif();
  if (netif) {
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
      subnet_bcast = (ip.ip.addr & ip.netmask.addr) | ~ip.netmask.addr;
    }
  }

  esp_err_t err = ESP_OK;
  for (int i = 0; i < WOL_REPEATS; i++) {
    if (send_to(sock, pkt, sizeof(pkt), htonl(INADDR_BROADCAST)) != ESP_OK) {
      err = ESP_FAIL;
    }
    if (subnet_bcast != 0 && subnet_bcast != htonl(INADDR_BROADCAST)) {
      send_to(sock, pkt, sizeof(pkt), subnet_bcast);
    }
    if (i + 1 < WOL_REPEATS) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  close(sock);

  char mac_str[18];
  pc_wake_format_mac(mac, mac_str, sizeof(mac_str));
  ESP_LOGI(TAG, "WoL magic packet for %s sent x%d (%s)", mac_str, WOL_REPEATS,
           err == ESP_OK ? "ok" : "send error");
  return err;
}

esp_err_t pc_wake_trigger(pc_wake_result_t *result) {
  pc_wake_result_t r = {0};
  esp_err_t overall = ESP_ERR_NOT_FOUND;

#ifdef CONFIG_USB_AUDIO_SOURCE
  r.usb_attempted = true;
  esp_err_t usb = usb_audio_source_wake_host();
  if (usb == ESP_OK) {
    r.usb_resume_sent = true;
    overall = ESP_OK;
    strlcat(r.detail, "usb resume sent; ", sizeof(r.detail));
  } else if (usb == ESP_ERR_INVALID_STATE) {
    strlcat(r.detail, "usb host not suspended; ", sizeof(r.detail));
  } else if (usb == ESP_ERR_NOT_ALLOWED) {
    strlcat(r.detail, "usb remote wakeup not allowed by host; ",
            sizeof(r.detail));
  } else {
    strlcat(r.detail, "usb resume failed; ", sizeof(r.detail));
  }
#endif

  char mac_text[24] = {0};
  uint8_t mac[6];
  if (settings_get_wol_mac(mac_text, sizeof(mac_text)) == ESP_OK &&
      pc_wake_parse_mac(mac_text, mac)) {
    r.wol_attempted = true;
    if (pc_wake_send_wol(mac) == ESP_OK) {
      r.wol_sent = true;
      overall = ESP_OK;
      strlcat(r.detail, "wol sent", sizeof(r.detail));
    } else {
      strlcat(r.detail, "wol send failed", sizeof(r.detail));
    }
  } else {
    strlcat(r.detail, "no WoL MAC configured", sizeof(r.detail));
  }

  ESP_LOGI(TAG, "Wake trigger: %s", r.detail);
  if (result) {
    *result = r;
  }
  return overall;
}
