#include "iot_board.h"

#include "dac.h"
#include "dac_es8388.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "rtsp_events.h"
#include "settings.h"

static const char TAG[] = "ESP32-A1S";

static bool s_board_initialized = false;
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)data;
  (void)user_data;

  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
  case RTSP_EVENT_PAUSED:
    dac_set_power_mode(DAC_POWER_STANDBY);
    break;
  case RTSP_EVENT_PLAYING:
    dac_set_power_mode(DAC_POWER_ON);
    break;
  case RTSP_EVENT_DISCONNECTED:
    dac_set_power_mode(DAC_POWER_OFF);
    break;
  case RTSP_EVENT_METADATA:
    break;
  }
}

const char *iot_board_get_info(void) {
  return BOARD_NAME;
}

bool iot_board_is_init(void) {
  return s_board_initialized;
}

board_res_handle_t iot_board_get_handle(int id) {
  switch (id) {
  case BOARD_I2C_DAC_ID:
  case BOARD_I2C_DISP_ID:
    return (board_res_handle_t)s_i2c_bus_handle;
  default:
    return NULL;
  }
}

esp_err_t iot_board_init(void) {
  if (s_board_initialized) {
    ESP_LOGW(TAG, "Board already initialized");
    return ESP_OK;
  }

  dac_register(&dac_es8388_ops);

  i2c_master_bus_config_t i2c_cfg = {
      .i2c_port = BOARD_I2C_PORT,
      .sda_io_num = BOARD_I2C_SDA_GPIO,
      .scl_io_num = BOARD_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus_handle), TAG,
                      "Failed to initialize I2C bus");
  ESP_LOGI(TAG, "I2C bus initialized: sda=%d, scl=%d", BOARD_I2C_SDA_GPIO,
           BOARD_I2C_SCL_GPIO);

  ESP_RETURN_ON_ERROR(dac_init(s_i2c_bus_handle), TAG,
                      "Failed to initialize ES8388");
  rtsp_events_register(on_rtsp_event, NULL);

  dac_set_power_mode(DAC_POWER_OFF);

  float vol_db;
  if (settings_get_volume(&vol_db) == ESP_OK) {
    dac_set_volume(vol_db);
  }

  s_board_initialized = true;
  ESP_LOGI(TAG, "ESP32-A1S initialized");
  return ESP_OK;
}

esp_err_t iot_board_deinit(void) {
  if (!s_board_initialized) {
    return ESP_OK;
  }

  rtsp_events_unregister(on_rtsp_event);
  dac_set_power_mode(DAC_POWER_OFF);
  dac_deinit();

  if (s_i2c_bus_handle != NULL) {
    i2c_del_master_bus(s_i2c_bus_handle);
    s_i2c_bus_handle = NULL;
  }

  s_board_initialized = false;
  return ESP_OK;
}
