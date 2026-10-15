#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms)/10)
#define portMAX_DELAY 0xffffffff
#define portTICK_PERIOD_MS 10
#define pdTRUE 1
