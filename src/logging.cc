#include "logging.h"
#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

const char *kLogPrefix = "openwirelessmc";

#define LOG_BUFFER_SIZE (8 * 1024)
static char s_log_buffer[LOG_BUFFER_SIZE];
static size_t s_log_head = 0;
static size_t s_log_count = 0;
static SemaphoreHandle_t s_log_mux = NULL;
static vprintf_like_t s_default_vprintf = NULL;

static int log_vprintf_hook(const char *fmt, va_list args)
{
    // Duplicate args for buffer vs stdout
    va_list args_copy;
    va_copy(args_copy, args);

    char temp[256];
    int len = vsnprintf(temp, sizeof(temp), fmt, args_copy);
    va_end(args_copy);

    if (len > 0)
    {
        if (s_log_mux && xSemaphoreTake(s_log_mux, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            size_t to_write = (len < (int)sizeof(temp)) ? (size_t)len : (sizeof(temp) - 1);
            for (size_t i = 0; i < to_write; i++)
            {
                s_log_buffer[s_log_head] = temp[i];
                s_log_head = (s_log_head + 1) % LOG_BUFFER_SIZE;
                if (s_log_count < LOG_BUFFER_SIZE)
                {
                    s_log_count++;
                }
            }
            xSemaphoreGive(s_log_mux);
        }
    }

    if (s_default_vprintf)
    {
        return s_default_vprintf(fmt, args);
    }
    return vprintf(fmt, args);
}

void logging_init_ringbuffer()
{
    if (!s_log_mux)
    {
        s_log_mux = xSemaphoreCreateMutex();
        s_default_vprintf = esp_log_set_vprintf(log_vprintf_hook);
    }
}

size_t logging_get_logs(char *dest, size_t max_len)
{
    if (!dest || max_len == 0) return 0;
    size_t copied = 0;

    if (s_log_mux && xSemaphoreTake(s_log_mux, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        size_t available = s_log_count;
        if (available >= max_len)
        {
            available = max_len - 1;
        }

        size_t start_idx = (s_log_head + LOG_BUFFER_SIZE - available) % LOG_BUFFER_SIZE;
        for (size_t i = 0; i < available; i++)
        {
            dest[i] = s_log_buffer[(start_idx + i) % LOG_BUFFER_SIZE];
        }
        dest[available] = '\0';
        copied = available;

        xSemaphoreGive(s_log_mux);
    }
    else
    {
        dest[0] = '\0';
    }

    return copied;
}