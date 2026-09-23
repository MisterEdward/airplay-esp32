#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdPASS             1
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms)  (ms)
