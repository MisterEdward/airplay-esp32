#include "dac_es8388.h"

#include "audio_codec_ctrl_if.h"
#include "audio_codec_gpio_if.h"
#include "audio_codec_if.h"
#include "es8388_codec.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TAG "ES8388 DAC"

#define AIRPLAY_MIN_DB      (-30.0f)
#define AIRPLAY_MID_DB      (-15.0f)
#define AIRPLAY_MAX_DB      0.0f
#define ES8388_MUTED_DB     (-96.0f)
#define ES8388_MIN_PLAY_DB  (-48.0f)
#define ES8388_MID_PLAY_DB  (-30.0f)
#define ES8388_MAX_PLAY_DB  (-6.0f)

static const audio_codec_ctrl_if_t *s_ctrl_if = NULL;
static const audio_codec_gpio_if_t *s_gpio_if = NULL;
static const audio_codec_if_t *s_codec_if = NULL;
static bool s_enabled = false;
static float s_volume_db = 0.0f;
static bool s_volume_muted = false;

static esp_err_t codec_ret_to_esp(int ret) {
  return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t dac_es8388_init(void *i2c_bus) {
  if (i2c_bus == NULL) {
    ESP_LOGE(TAG, "I2C bus is required");
    return ESP_ERR_INVALID_ARG;
  }

  audio_codec_i2c_cfg_t i2c_cfg = {
      .port = 0,
      .addr = (uint8_t)(CONFIG_ES8388_I2C_ADDR << 1),
      .bus_handle = i2c_bus,
  };
  s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
  if (s_ctrl_if == NULL) {
    ESP_LOGE(TAG, "Failed to create ES8388 I2C control interface");
    return ESP_FAIL;
  }

  s_gpio_if = audio_codec_new_gpio();
  if (s_gpio_if == NULL) {
    ESP_LOGE(TAG, "Failed to create ES8388 GPIO interface");
    return ESP_FAIL;
  }

  es8388_codec_cfg_t codec_cfg = {
      .ctrl_if = s_ctrl_if,
      .gpio_if = s_gpio_if,
      .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
      .master_mode = false,
      .pa_pin = CONFIG_MUTE_GPIO,
      .pa_reverted = false,
  };
  s_codec_if = es8388_codec_new(&codec_cfg);
  if (s_codec_if == NULL) {
    ESP_LOGE(TAG, "Failed to create ES8388 codec");
    return ESP_FAIL;
  }

  esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 16,
      .channel = 2,
      .sample_rate = CONFIG_OUTPUT_SAMPLE_RATE_HZ,
  };
  ESP_RETURN_ON_ERROR(codec_ret_to_esp(s_codec_if->set_fs(s_codec_if, &fs)),
                      TAG, "Failed to configure ES8388 sample format");
  ESP_RETURN_ON_ERROR(codec_ret_to_esp(s_codec_if->set_vol(s_codec_if,
                                                           s_volume_db)),
                      TAG, "Failed to set ES8388 volume");
  ESP_RETURN_ON_ERROR(codec_ret_to_esp(s_codec_if->mute(s_codec_if, true)),
                      TAG, "Failed to mute ES8388");

  ESP_LOGI(TAG, "ES8388 initialized at I2C 0x%02X", CONFIG_ES8388_I2C_ADDR);
  return ESP_OK;
}

static esp_err_t dac_es8388_deinit(void) {
  if (s_codec_if != NULL) {
    if (s_enabled && s_codec_if->enable) {
      s_codec_if->enable(s_codec_if, false);
    }
    if (s_codec_if->close) {
      s_codec_if->close(s_codec_if);
    }
    audio_codec_delete_codec_if(s_codec_if);
    s_codec_if = NULL;
  }
  if (s_gpio_if != NULL) {
    audio_codec_delete_gpio_if(s_gpio_if);
    s_gpio_if = NULL;
  }
  if (s_ctrl_if != NULL) {
    audio_codec_delete_ctrl_if(s_ctrl_if);
    s_ctrl_if = NULL;
  }
  s_enabled = false;
  s_volume_muted = false;
  return ESP_OK;
}

static float dac_es8388_map_volume(float airplay_db, bool *mute) {
  if (airplay_db <= AIRPLAY_MIN_DB) {
    *mute = true;
    return ES8388_MUTED_DB;
  }

  *mute = false;
  if (airplay_db >= AIRPLAY_MAX_DB) {
    return ES8388_MAX_PLAY_DB;
  }

  if (airplay_db <= AIRPLAY_MID_DB) {
    float normalized =
        (airplay_db - AIRPLAY_MIN_DB) / (AIRPLAY_MID_DB - AIRPLAY_MIN_DB);
    return ES8388_MIN_PLAY_DB +
           normalized * (ES8388_MID_PLAY_DB - ES8388_MIN_PLAY_DB);
  }

  float normalized =
      (airplay_db - AIRPLAY_MID_DB) / (AIRPLAY_MAX_DB - AIRPLAY_MID_DB);
  return ES8388_MID_PLAY_DB +
         normalized * (ES8388_MAX_PLAY_DB - ES8388_MID_PLAY_DB);
}

static void dac_es8388_set_volume(float volume_db) {
  bool muted = false;
  float codec_db = dac_es8388_map_volume(volume_db, &muted);
  s_volume_db = volume_db;
  s_volume_muted = muted;

  if (s_codec_if != NULL && s_codec_if->set_vol) {
    s_codec_if->set_vol(s_codec_if, codec_db);
  }
  if (s_codec_if != NULL && s_codec_if->mute && s_enabled) {
    s_codec_if->mute(s_codec_if, muted);
  }
}

static void dac_es8388_set_power_mode(dac_power_mode_t mode) {
  if (s_codec_if == NULL || s_codec_if->enable == NULL) {
    return;
  }

  bool enable = (mode == DAC_POWER_ON);
  if (enable != s_enabled) {
    if (s_codec_if->mute) {
      s_codec_if->mute(s_codec_if, true);
    }
    if (!enable) {
      vTaskDelay(pdMS_TO_TICKS(4));
    }

    s_codec_if->enable(s_codec_if, enable);
    
    if (enable && s_ctrl_if && s_ctrl_if->write_reg) {
      vTaskDelay(pdMS_TO_TICKS(8));

      // Route DAC straight to the output mixers at 0 dB.
      uint8_t mix_vol = 0x80;
      s_ctrl_if->write_reg(s_ctrl_if, 0x27, 1, &mix_vol, 1); // ES8388_DACCONTROL17
      s_ctrl_if->write_reg(s_ctrl_if, 0x2A, 1, &mix_vol, 1); // ES8388_DACCONTROL20

      // Some ESP32-A1S boards route the jack to OUT2, so boost both pairs.
      uint8_t out_vol = 0x21;
      s_ctrl_if->write_reg(s_ctrl_if, 0x2E, 1, &out_vol, 1); // ES8388_DACCONTROL24 LOUT1
      s_ctrl_if->write_reg(s_ctrl_if, 0x2F, 1, &out_vol, 1); // ES8388_DACCONTROL25 ROUT1
      s_ctrl_if->write_reg(s_ctrl_if, 0x30, 1, &out_vol, 1); // ES8388_DACCONTROL26 LOUT2
      s_ctrl_if->write_reg(s_ctrl_if, 0x31, 1, &out_vol, 1); // ES8388_DACCONTROL27 ROUT2
    }
    
    s_enabled = enable;
  }
  if (s_codec_if->mute) {
    s_codec_if->mute(s_codec_if, !enable || s_volume_muted);
  }
}

static void dac_es8388_enable_speaker(bool enable) {
  if (s_codec_if != NULL && s_codec_if->mute) {
    s_codec_if->mute(s_codec_if, !enable);
  }
}

static void dac_es8388_enable_line_out(bool enable) {
  dac_es8388_enable_speaker(enable);
}

const dac_ops_t dac_es8388_ops = {
    .init = dac_es8388_init,
    .deinit = dac_es8388_deinit,
    .set_volume = dac_es8388_set_volume,
    .set_power_mode = dac_es8388_set_power_mode,
    .enable_speaker = dac_es8388_enable_speaker,
    .enable_line_out = dac_es8388_enable_line_out,
};
