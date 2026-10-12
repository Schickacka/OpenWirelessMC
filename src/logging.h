#ifndef _ESP_LOGGING_H
#define _ESP_LOGGING_H

#include <stddef.h>

extern const char *kLogPrefix;

void logging_init_ringbuffer();
size_t logging_get_logs(char *dest, size_t max_len);

#endif // _ESP_LOGGING_H