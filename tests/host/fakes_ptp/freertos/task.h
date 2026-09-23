#pragma once
#include "freertos/FreeRTOS.h"
extern int64_t fake_now_us;
static inline TickType_t xTaskGetTickCount(void) {
  return (TickType_t)(fake_now_us / 1000);
}
static inline void vTaskDelay(TickType_t t) {
  (void)t;
}
static inline void vTaskDelete(TaskHandle_t t) {
  (void)t;
}
