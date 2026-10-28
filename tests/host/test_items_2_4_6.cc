#include "bsd_compat.h"
#include "web/file_server.h"
#include "storage/storage.h"
#include "esp_http_server.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

using namespace openmc::storage;
using namespace openmc::web::file_server;

struct MockFsData {
  char base_path[512];
  char scratch[8192];
};
static MockFsData g_default_fs_data;

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
extern HttpMockState g_http_mock;

static void SetupMockReq(httpd_req_t *req, const char *uri, int method, const char *body = NULL, size_t body_len = 0)
{
  g_http_mock.Reset();
  req->uri = uri;
  req->method = method;
  req->content_len = body_len ? body_len : (body ? strlen(body) : 0);
  strcpy(g_default_fs_data.base_path, base_path);
  req->user_ctx = &g_default_fs_data;
  g_http_mock.request_body = body;
  g_http_mock.request_body_len = req->content_len;
}

int main()
{
  strcpy(base_path, "tests_sdcard");
  mkdir("tests_sdcard", 0755);
  ready = true;

  printf("=== EVIDENCIA ITENS 2, 4, 6 ===\n");

  // ITEM 2: POST /api/fs/put com overwrite=0 sobre arquivo existente -> 409 Conflict
  {
    const char *rel = "MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd";
    char dummy[64] = "test_payload";
    httpd_req_t req;
    SetupMockReq(&req, "/api/fs/put?path=MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd", HTTP_POST, dummy);
    api_fs_put_handler(&req);
    printf("Item 2 (POST /api/fs/put existente sem overwrite): HTTP Status = %s | Body = %s\n",
           g_http_mock.last_status, g_http_mock.response_body);
  }

  // ITEM 4: POST /api/fs/put sobre cartão atualmente montado -> 409 Conflict
  {
    char card_path[kCardPathMax];
    snprintf(card_path, sizeof(card_path), "%s/MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd", base_path);
    CreateBlankFormattedCard(card_path);
    LoadCardFromFile(card_path, NULL);

    char dummy[64] = "test_payload";
    httpd_req_t req;
    SetupMockReq(&req, "/api/fs/put?path=MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd&overwrite=1", HTTP_POST, dummy);
    api_fs_put_handler(&req);
    printf("Item 4 (POST /api/fs/put cartao montado): HTTP Status = %s | Body = %s\n",
           g_http_mock.last_status, g_http_mock.response_body);
    UnloadCard();
  }

  // ITEM 6: GET /api/status -> 200 OK (payload valido)
  {
    httpd_req_t req;
    SetupMockReq(&req, "/api/status", HTTP_GET);
    api_status_handler(&req);
    const char *st = g_http_mock.last_status[0] ? g_http_mock.last_status : "200 OK";
    printf("Item 6 (GET /api/status): HTTP Status = %s | Body = %s\n",
           st, g_http_mock.response_body);
  }

  return 0;
}
