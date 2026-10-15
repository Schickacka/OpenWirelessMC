#pragma once
#include <stdint.h>
typedef struct { const char* label; uint32_t address; uint32_t size; } esp_partition_t; typedef uint32_t esp_ota_handle_t;
#define OTA_WITH_SEQUENTIAL_WRITES 0xFFFFFFFF
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*); esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t, const void*, size_t); esp_err_t esp_ota_end(esp_ota_handle_t); esp_err_t esp_ota_set_boot_partition(const esp_partition_t*);
