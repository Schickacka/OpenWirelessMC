#include "wifi/client.h"
#include "wifi/wifi.h"

#include "logging.h"
#include "system/settings.h"

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <lwip/err.h>
#include <lwip/sys.h>

// To-do: Save/load this to SD, WiFi AP for configuration mode?
#define EXAMPLE_ESP_MAXIMUM_RETRY 5

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

namespace openmc
{
    namespace wifi
    {
        namespace client
        {
            // FreeRTOS event group to signal when we are connected
            static EventGroupHandle_t s_wifi_event_group;

            static int s_retry_num = 0;

            static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
            {
                wifi_config_t wifi_config;
                if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
                {
                    esp_wifi_connect();
                }
                else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
                {
                    wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
                    ESP_LOGW(kLogPrefix, "Disconnected from AP, reason=%d (%s)", 
                             disconn ? disconn->reason : -1,
                             disconn ? (disconn->reason == 202 ? "AUTH_FAIL" : (disconn->reason == 201 ? "NO_AP_FOUND" : "DISCONNECT")) : "");
                    if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
                    {
                        esp_wifi_connect();
                        s_retry_num++;
                        ESP_LOGI(kLogPrefix, "retry to connect to the AP (%d/%d)", s_retry_num, EXAMPLE_ESP_MAXIMUM_RETRY);
                    }
                    else
                    {
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    }
                }
                else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
                {
                    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                    ESP_LOGI(kLogPrefix, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
                    sprintf(wifi::ip_address, IPSTR, IP2STR(&event->ip_info.ip));
                    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) == ESP_OK)
                    {
                        strcpy((char *)&wifi::ssid, (char *)&wifi_config.sta.ssid);
                    }

                    s_retry_num = 0;
                    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                }
            }

            bool Init(void)
            {
                // Initialize NVS
                esp_err_t ret = nvs_flash_init();
                if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
                {
                    ESP_ERROR_CHECK(nvs_flash_erase());
                    ret = nvs_flash_init();
                }
                ESP_ERROR_CHECK(ret);

                ESP_LOGI(kLogPrefix, "ESP_WIFI_MODE_STA");

                s_wifi_event_group = xEventGroupCreate();

                ret = esp_netif_init();
                if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
                {
                    ESP_ERROR_CHECK(ret);
                }

                ret = esp_event_loop_create_default();
                if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
                {
                    ESP_ERROR_CHECK(ret);
                }

                esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
                if (sta_netif)
                {
                    esp_netif_set_hostname(sta_netif, "wirelessmc");
                }

                wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
                ESP_ERROR_CHECK(esp_wifi_init(&cfg));

                esp_event_handler_instance_t instance_any_id;
                esp_event_handler_instance_t instance_got_ip;
                ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
                ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

                wifi_config_t wifi_config;
                memset(&wifi_config, 0, sizeof(wifi_config));

                strncpy((char *)wifi_config.sta.ssid, (const char *)system::settings.ssid, sizeof(wifi_config.sta.ssid));
                strncpy((char *)wifi_config.sta.password, (const char *)system::settings.password, sizeof(wifi_config.sta.password));
                wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
                wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
                wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
                wifi_config.sta.pmf_cfg.capable = true;
                wifi_config.sta.pmf_cfg.required = false;

                ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
                ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
                s_retry_num = 0;
                ESP_ERROR_CHECK(esp_wifi_start());

                ESP_LOGI(kLogPrefix, "Init finished. Waiting for connection...");

                // Waiting up to 45 seconds for connection or failure
                EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(45000));

                bool connected = false;
                if (bits & WIFI_CONNECTED_BIT)
                {
                    ESP_LOGI(kLogPrefix, "Connected to AP SSID:%s", wifi_config.sta.ssid);
                    wifi::ready = true;
                    connected = true;
                }
                else
                {
                    ESP_LOGW(kLogPrefix, "Failed or timed out connecting to SSID:%s", wifi_config.sta.ssid);
                    wifi::ready = false;
                    connected = false;
                }

                // The event will not be processed after unregister
                ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
                ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
                vEventGroupDelete(s_wifi_event_group);

                if (!connected)
                {
                    ESP_LOGI(kLogPrefix, "Cleaning up STA interface for fallback...");
                    esp_wifi_stop();
                    esp_wifi_deinit();
                    if (sta_netif)
                    {
                        esp_netif_destroy(sta_netif);
                    }
                }

                return connected;
            }
        } // client

    } // wifi
} // openmc