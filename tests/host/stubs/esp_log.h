#pragma once
#include "sdkconfig.h"
#include <stdio.h>
extern int g_log_quiet;
#define ESP_LOGI(tag, fmt, ...) do{ if(!g_log_quiet) printf("I " fmt "\n", ##__VA_ARGS__);}while(0)
#define ESP_LOGW(tag, fmt, ...) do{ if(!g_log_quiet) printf("W " fmt "\n", ##__VA_ARGS__);}while(0)
#define ESP_LOGE(tag, fmt, ...) do{ if(!g_log_quiet) printf("E " fmt "\n", ##__VA_ARGS__);}while(0)
