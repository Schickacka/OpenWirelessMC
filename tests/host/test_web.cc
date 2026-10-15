#define ENABLE_OLED 0
#include "web/file_server.cc"
#include <string>
using namespace openmc::web::file_server;
// ---- link stubs for everything file_server.cc references
static std::string g_out; static int g_zero_chunks = 0;
esp_err_t httpd_resp_send_chunk(httpd_req_t*, const char* b, ssize_t n){ if(n==0) g_zero_chunks++; g_out.append(b,n); return 0; }
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t* r, const char* s){ return httpd_resp_send_chunk(r,s,s?strlen(s):0); }
esp_err_t httpd_resp_send(httpd_req_t*, const char*, ssize_t){return 0;} esp_err_t httpd_resp_sendstr(httpd_req_t*, const char*){return 0;}
esp_err_t httpd_resp_send_err(httpd_req_t*, httpd_err_code_t, const char*){return 0;} esp_err_t httpd_resp_set_type(httpd_req_t*, const char*){return 0;}
esp_err_t httpd_resp_set_status(httpd_req_t*, const char*){return 0;} esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*){return 0;}
int httpd_req_recv(httpd_req_t*, char*, size_t){return 0;} esp_err_t httpd_start(httpd_handle_t*, const httpd_config_t*){return 0;}
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*){return 0;} bool httpd_uri_match_wildcard(const char*, const char*, size_t){return true;}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t*){return 0;} int64_t esp_timer_get_time(){return 0;}
esp_netif_t* esp_netif_get_handle_from_ifkey(const char*){return 0;} esp_err_t esp_netif_get_ip_info(esp_netif_t*, esp_netif_ip_info_t*){return 0;} char* esp_ip4addr_ntoa(const esp_ip4_addr_t*, char*, int){return 0;}
uint32_t esp_get_free_heap_size(){return 0;} uint32_t esp_get_minimum_free_heap_size(){return 0;} void esp_restart(){} void esp_chip_info(esp_chip_info_t*){}
esp_err_t esp_flash_get_size(void*, uint32_t*){return 0;} uint32_t heap_caps_get_total_size(int){return 0;} void rtc_clk_cpu_freq_get_config(rtc_cpu_freq_config_t*){}
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*){return 0;} esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t*){return 0;}
esp_err_t esp_ota_write(esp_ota_handle_t, const void*, size_t){return 0;} esp_err_t esp_ota_end(esp_ota_handle_t){return 0;} esp_err_t esp_ota_set_boot_partition(const esp_partition_t*){return 0;}
FRESULT f_getfree(const char*, DWORD*, FATFS**){return 0;}
size_t logging_get_logs(char*, size_t){return 0;}
#include "system/settings.h"
namespace openmc { namespace system { struct Settings settings; } namespace wifi { bool ready; char ip_address[16]; uint8_t ssid[32]; }
 namespace storage { namespace config { bool LoadConfig(){ return true; } } } }
extern int g_log_quiet;
static int fails=0; 
#define CHECK(c) do{ if(!(c)){ printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } }while(0)

static const char* P(const char* uri, char* dest, size_t n=255){ return get_path_from_uri(dest, "/sdcard", uri, n); }
int main(){
  g_log_quiet=1; char d[256]; const char* r;
  printf("[get_path_from_uri]\n");
  r=P("/",d); CHECK(r && !strcmp(r,"/") && !strcmp(d,"/sdcard/"));
  r=P("/Final%20Fantasy%20VII.mc",d); CHECK(r && !strcmp(d,"/sdcard/Final Fantasy VII.mc"));
  r=P("/a+b.mc",d); CHECK(r && !strcmp(d,"/sdcard/a+b.mc"));                  // '+' is literal in paths
  r=P("/Game%20%231.mc",d); CHECK(r && !strcmp(d,"/sdcard/Game #1.mc"));      // encoded '#': used to truncate the name
  r=P("/x.mc?foo=bar",d); CHECK(r && !strcmp(d,"/sdcard/x.mc"));              // query string removed
  CHECK(P("/../etc/passwd",d)==NULL); CHECK(P("/%2e%2e/x",d)==NULL); CHECK(P("/a/../../b",d)==NULL); CHECK(P("/a/..",d)==NULL);
  CHECK(P("/..%5c..%5cx",d)==NULL);                                            // backslash
  r=P("/..hidden/file..mc",d); CHECK(r!=NULL);                                 // names merely containing ".." are fine
  std::string longu="/"+std::string(300,'a'); CHECK(P(longu.c_str(),d)==NULL);
  std::string hugeu="/"+std::string(600,'a'); CHECK(P(hugeu.c_str(),d)==NULL);

  printf("[escaping]\n");
  httpd_req_t req{}; 
  g_out.clear(); send_html_escaped(&req,"A&B <x> \"q\" it's"); CHECK(g_out=="A&amp;B &lt;x&gt; &quot;q&quot; it&#39;s");
  g_out.clear(); send_html_escaped(&req,""); CHECK(g_out.empty() && g_zero_chunks==0);   // empty string must not end the response
  g_out.clear(); send_url_encoded(&req,"My Game #1 (v2)+x.mc",false); CHECK(g_out=="My%20Game%20%231%20%28v2%29%2Bx.mc");
  g_out.clear(); send_url_encoded(&req,"/dir one/",true); CHECK(g_out=="/dir%20one/");
  g_out.clear(); send_url_encoded(&req,"'); alert(1);//",false); CHECK(g_out.find('\'')==std::string::npos && g_out.find('(')==std::string::npos);
  std::string big(500,'&'); g_out.clear(); send_html_escaped(&req,big.c_str()); CHECK(g_out.size()==500*5);   // spans several internal flushes
  std::string big2(500,'/'); g_out.clear(); send_url_encoded(&req,big2.c_str(),false); CHECK(g_out.size()==1500);
  CHECK(g_zero_chunks==0);

  printf("[has_file_ext]\n");
  CHECK(has_file_ext("a.MC",".mc")); CHECK(!has_file_ext("c",".mcd")); CHECK(!has_file_ext("",".mc")); CHECK(has_file_ext("x.mcd",".mcd"));
  printf("\n%s (%d failures)\n", fails?"FAILED":"ALL TESTS PASSED", fails); return fails?1:0; }
