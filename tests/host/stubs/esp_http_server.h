#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct httpd_req { const char *uri; size_t content_len; void *user_ctx; int method; } httpd_req_t;
typedef void* httpd_handle_t;
typedef enum { HTTPD_400_BAD_REQUEST, HTTPD_403_FORBIDDEN, HTTPD_404_NOT_FOUND, HTTPD_500_INTERNAL_SERVER_ERROR } httpd_err_code_t;
typedef struct { int server_port; int max_uri_handlers; bool (*uri_match_fn)(const char*, const char*, size_t); size_t stack_size; } httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() {80,8,0,8192}
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t*); void *user_ctx; } httpd_uri_t;
#define HTTP_GET 1
#define HTTP_POST 3
#define HTTPD_SOCK_ERR_TIMEOUT -3
esp_err_t httpd_resp_send_chunk(httpd_req_t*, const char*, ssize_t);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t*, const char*);
esp_err_t httpd_resp_send(httpd_req_t*, const char*, ssize_t);
esp_err_t httpd_resp_sendstr(httpd_req_t*, const char*);
esp_err_t httpd_resp_send_err(httpd_req_t*, httpd_err_code_t, const char*);
esp_err_t httpd_resp_set_type(httpd_req_t*, const char*);
esp_err_t httpd_resp_set_status(httpd_req_t*, const char*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*);
int httpd_req_recv(httpd_req_t*, char*, size_t);
esp_err_t httpd_start(httpd_handle_t*, const httpd_config_t*);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*);
bool httpd_uri_match_wildcard(const char*, const char*, size_t);
