#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "playstation/sio_memory_card.h"
#include "playstation/sio.h"
#include "playstation/spi.h"
#include "system/settings.h"

int g_log_quiet = 0;
const char *kLogPrefix = "T";

namespace openmc {
 namespace spi { volatile uint32_t sel_glitch_count; }
 namespace system {
   namespace timer { uint64_t timestamp; }
   Settings settings;
 }
 namespace sio {
   uint64_t event_ticks; bool memory_card_enabled = true; bool pad_enabled; bool net_yaroze_enabled;
   int set_calls = 0;
   void SetMemoryCardEmulation(bool e){ memory_card_enabled = e; set_calls++; }
   namespace memory_card {
     uint8_t memory_card_ram[131072]; uint16_t sector_version[1024]; uint8_t flag; uint16_t sector;
     uint8_t game_id[256]; uint8_t game_id_length; volatile bool game_id_updated;
     uint64_t volatile last_tick; uint64_t volatile last_write_tick; bool committed_to_storage = true; bool skip_ack;
     McTraceEntry trace_ring[32]; volatile uint32_t trace_write_idx, trace_read_idx, pad_poll_count, abort_byte0_count;
     void GoIdle(){}
   }
 }
 namespace storage { namespace sdcard {
   bool sd_ok = true; bool sd_mounted = false; int unmounts = 0;
   esp_err_t Init(void){ sd_mounted = sd_ok; return sd_ok ? ESP_OK : ESP_FAIL; }
   esp_err_t unmount_sdcard(void){ unmounts++; if (!sd_ok) return ESP_FAIL; sd_mounted = false; return ESP_OK; }
 }
 namespace config {
   bool __attribute__((weak)) LoadConfig(void) { return true; }
 }
 }
}

struct HttpMockState {
  char last_status[64];
  char last_content_type[64];
  char response_body[8192];
  size_t response_body_len;
  int error_code;
  const char *request_body;
  size_t request_body_len;
  size_t request_body_read;
  void Reset() {
    last_status[0] = '\0';
    last_content_type[0] = '\0';
    response_body[0] = '\0';
    response_body_len = 0;
    error_code = 0;
    request_body = NULL;
    request_body_len = 0;
    request_body_read = 0;
  }
};
HttpMockState g_http_mock;

esp_err_t httpd_resp_set_status(httpd_req_t*, const char* status) {
  if (status) strlcpy(g_http_mock.last_status, status, sizeof(g_http_mock.last_status));
  return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t*, const char* type) {
  if (type) strlcpy(g_http_mock.last_content_type, type, sizeof(g_http_mock.last_content_type));
  return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t*, const char* buf, ssize_t len) {
  if (!buf) return ESP_OK;
  size_t n = (len < 0) ? strlen(buf) : (size_t)len;
  if (g_http_mock.response_body_len + n < sizeof(g_http_mock.response_body)) {
    memcpy(g_http_mock.response_body + g_http_mock.response_body_len, buf, n);
    g_http_mock.response_body_len += n;
    g_http_mock.response_body[g_http_mock.response_body_len] = '\0';
  }
  return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char* str) {
  return httpd_resp_send(req, str, str ? strlen(str) : 0);
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char* buf, ssize_t len) {
  return httpd_resp_send(req, buf, len);
}
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *req, const char* str) {
  return httpd_resp_sendstr(req, str);
}
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t code, const char *msg) {
  g_http_mock.error_code = (int)code;
  const char *status_str = "500 Internal Server Error";
  switch(code) {
    case HTTPD_400_BAD_REQUEST: status_str = "400 Bad Request"; break;
    case HTTPD_403_FORBIDDEN: status_str = "403 Forbidden"; break;
    case HTTPD_404_NOT_FOUND: status_str = "404 Not Found"; break;
    case HTTPD_500_INTERNAL_SERVER_ERROR: status_str = "500 Internal Server Error"; break;
    default: break;
  }
  snprintf(g_http_mock.last_status, sizeof(g_http_mock.last_status), "%s", status_str);
  return ESP_FAIL;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) {
  return ESP_OK;
}
int httpd_req_recv(httpd_req_t*, char* buf, size_t max_len) {
  if (!g_http_mock.request_body || g_http_mock.request_body_read >= g_http_mock.request_body_len) {
    return 0;
  }
  size_t avail = g_http_mock.request_body_len - g_http_mock.request_body_read;
  size_t to_copy = (avail < max_len) ? avail : max_len;
  memcpy(buf, g_http_mock.request_body + g_http_mock.request_body_read, to_copy);
  g_http_mock.request_body_read += to_copy;
  return (int)to_copy;
}
esp_err_t httpd_start(httpd_handle_t*, const httpd_config_t*) {
  return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*) {
  return ESP_OK;
}
bool httpd_uri_match_wildcard(const char*, const char*, size_t) {
  return true;
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *buf, size_t buf_len) {
  if (!req || !req->uri || !buf || buf_len == 0) return ESP_FAIL;
  const char *q = strchr(req->uri, '?');
  if (!q) return ESP_ERR_NOT_FOUND;
  strlcpy(buf, q + 1, buf_len);
  return ESP_OK;
}
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size) {
  if (!qry || !key || !val || val_size == 0) return ESP_FAIL;
  size_t klen = strlen(key);
  const char *p = qry;
  while (*p) {
    if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char *v = p + klen + 1;
      const char *end = strchr(v, '&');
      size_t vlen = end ? (size_t)(end - v) : strlen(v);
      size_t to_copy = (vlen < val_size - 1) ? vlen : (val_size - 1);
      memcpy(val, v, to_copy);
      val[to_copy] = '\0';
      return ESP_OK;
    }
    const char *next = strchr(p, '&');
    if (!next) break;
    p = next + 1;
  }
  return ESP_ERR_NOT_FOUND;
}

#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "soc/rtc.h"
#include "ff.h"
#include "esp_netif.h"

int64_t esp_timer_get_time() { return 1000000; }
FRESULT f_getfree(const char*, DWORD* fre, FATFS** fs) { if (fre) *fre = 1000; return FR_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *info) { if (info) info->rssi = -50; return ESP_OK; }
esp_netif_t* esp_netif_get_handle_from_ifkey(const char*) { return (esp_netif_t*)1; }
esp_err_t esp_netif_get_ip_info(void*, esp_netif_ip_info_t *ip) { return ESP_OK; }
char* esp_ip4addr_ntoa(const esp_ip4_addr_t*, char *buf, int buflen) { if (buf) strlcpy(buf, "192.168.4.1", buflen); return buf; }
void esp_chip_info(esp_chip_info_t *ci) { if (ci) { ci->cores = 2; ci->revision = 3; } }
void rtc_clk_cpu_freq_get_config(rtc_cpu_freq_config_t *cfg) { if (cfg) cfg->freq_mhz = 240; }
esp_err_t esp_flash_get_size(void*, uint32_t *sz) { if (sz) *sz = 4194304; return ESP_OK; }
uint32_t heap_caps_get_total_size(int) { return 327680; }
uint32_t esp_get_minimum_free_heap_size() { return 45000; }
uint32_t esp_get_free_heap_size() { return 50000; }
void esp_restart() {}
int logging_get_logs(char*, size_t) { return 0; }
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) { return NULL; }
esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t*) { return ESP_FAIL; }
esp_err_t esp_ota_write(esp_ota_handle_t, const void*, size_t) { return ESP_FAIL; }
esp_err_t esp_ota_end(esp_ota_handle_t) { return ESP_FAIL; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*) { return ESP_FAIL; }
esp_err_t esp_ota_get_partition_description(const esp_partition_t*, esp_app_desc_t*) { return ESP_FAIL; }

extern const unsigned char upload_script_html_data[] asm("_binary_upload_script_html_start");
const unsigned char upload_script_html_data[] = "<html>";
extern const unsigned char upload_script_html_end_data[] asm("_binary_upload_script_html_end");
const unsigned char upload_script_html_end_data[1] = {0};

extern const unsigned char footer_html_data[] asm("_binary_footer_html_start");
const unsigned char footer_html_data[] = "</html>";
extern const unsigned char footer_html_end_data[] asm("_binary_footer_html_end");
const unsigned char footer_html_end_data[1] = {0};

extern const unsigned char favicon_ico_data[] asm("_binary_favicon_ico_start");
const unsigned char favicon_ico_data[] = "ico";
extern const unsigned char favicon_ico_end_data[] asm("_binary_favicon_ico_end");
const unsigned char favicon_ico_end_data[1] = {0};
