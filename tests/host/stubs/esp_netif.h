#pragma once
#include <stdint.h>
typedef struct { uint32_t ip; } esp_ip4_addr_t; typedef struct { esp_ip4_addr_t ip; } esp_netif_ip_info_t; typedef void esp_netif_t;
esp_netif_t* esp_netif_get_handle_from_ifkey(const char*); esp_err_t esp_netif_get_ip_info(esp_netif_t*, esp_netif_ip_info_t*); char* esp_ip4addr_ntoa(const esp_ip4_addr_t*, char*, int);
