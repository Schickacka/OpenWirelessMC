#pragma once
#include "FreeRTOS.h"
#include <unistd.h>
static inline void vTaskDelay(TickType_t t){ usleep(t*1000); }
static inline void vTaskDelete(void*){}
typedef void* TaskHandle_t; typedef void (*TaskFunction_t)(void*);
static inline int xTaskCreate(TaskFunction_t, const char*, unsigned, void*, unsigned, TaskHandle_t*){ return 1; }
