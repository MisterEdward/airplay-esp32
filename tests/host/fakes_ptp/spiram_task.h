#pragma once
#include "freertos/task.h"
typedef struct {
  void *stack;
  void *tcb;
} spiram_task_mem_t;
static inline BaseType_t task_create_spiram(TaskFunction_t fn, const char *name,
                                            uint32_t depth, void *param,
                                            UBaseType_t prio,
                                            TaskHandle_t *handle,
                                            spiram_task_mem_t *mem) {
  (void)fn;
  (void)name;
  (void)depth;
  (void)param;
  (void)prio;
  (void)handle;
  (void)mem;
  return pdPASS;
}
static inline void task_free_spiram(spiram_task_mem_t *mem) {
  (void)mem;
}
