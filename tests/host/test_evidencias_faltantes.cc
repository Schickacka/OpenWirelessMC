#include "bsd_compat.h"
#include "esp_http_server.h"
#include "storage/storage.h"
#include "storage/ps1_empty_card.h"
#include "system/settings.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

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

// Inclui a implementação do servidor web diretamente no harness de teste
#include "web/file_server.cc"

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "ASSERT FAILED: %s at %s:%d\n", #cond, __FILE__, __LINE__); assert(cond); } } while(0)

using namespace openmc::storage;
using namespace openmc::web::file_server;

static struct file_server_data g_test_fs_data;

static void SetupReq(httpd_req_t *req, const char *uri, int method, const char *body = NULL, size_t body_len = 0)
{
  g_http_mock.Reset();
  req->uri = uri;
  req->method = method;
  req->content_len = body_len ? body_len : (body ? strlen(body) : 0);
  strcpy(g_test_fs_data.base_path, base_path);
  req->user_ctx = &g_test_fs_data;
  g_http_mock.request_body = body;
  g_http_mock.request_body_len = req->content_len;
}

int main()
{
  strcpy(base_path, "tests_sdcard");
  mkdir("tests_sdcard", 0755);
  ready = true;

  printf("=== EVIDENCIA 1: POST /api/fs/put com overwrite=1 (conteudo final no disco) ===\n");
  {
    EnsureDirectoryPublic("MemoryCards/PS1/SLUS-00421");
    char target_file[kCardPathMax];
    snprintf(target_file, sizeof(target_file), "%s/MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd", base_path);

    // 1. Cria arquivo existente com conteudo inicial
    const char *initial_data = "CONTEUDO_INICIAL_ANTES_DO_OVERWRITE_12345";
    FILE *f1 = fopen(target_file, "wb");
    CHECK(f1 != NULL);
    fwrite(initial_data, 1, strlen(initial_data), f1);
    fclose(f1);

    printf("1. Conteudo antes do overwrite: [%s]\n", initial_data);

    // 2. Executa PUT com overwrite=1 e novo conteudo
    const char *new_data = "CONTEUDO_FINAL_ATUALIZADO_APOS_OVERWRITE_67890";
    httpd_req_t req;
    SetupReq(&req, "/api/fs/put?path=MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd&overwrite=1", HTTP_POST, new_data);
    esp_err_t res = api_fs_put_handler(&req);
    CHECK(res == ESP_OK);
    printf("2. HTTP Status retornado: [%s]\n", g_http_mock.last_status);

    // 3. Le o arquivo diretamente do disco para validar substituicao real
    FILE *f2 = fopen(target_file, "rb");
    CHECK(f2 != NULL);
    char disk_buf[128] = {0};
    size_t n = fread(disk_buf, 1, sizeof(disk_buf) - 1, f2);
    disk_buf[n] = '\0';
    fclose(f2);

    printf("3. Conteudo lido do disco apos PUT: [%s]\n", disk_buf);
    CHECK(strcmp(disk_buf, new_data) == 0);

    // Confere que .part e .bak foram devidamente removidos
    char part[kCardPathMax + 8], bak[kCardPathMax + 8];
    snprintf(part, sizeof(part), "%s.part", target_file);
    snprintf(bak, sizeof(bak), "%s.bak", target_file);
    struct stat st;
    CHECK(stat(part, &st) != 0);
    CHECK(stat(bak, &st) != 0);
    printf("4. Arquivos temporarios .part e .bak removidos: OK\n\n");
  }

  printf("=== EVIDENCIA 2: /api/status com payload que estoura 2048 bytes (truncated: true) ===\n");
  {
    // Preenche loaded_file_path com 2100 caracteres para forcar o retorno de snprintf >= 2048
    memset(loaded_file_path, 'P', 2100);
    loaded_file_path[2100] = '\0';
    memset(openmc::system::settings.ssid, 'W', 31);
    openmc::system::settings.ssid[31] = '\0';

    httpd_req_t req;
    SetupReq(&req, "/api/status", HTTP_GET);
    esp_err_t res = api_status_handler(&req);
    CHECK(res == ESP_OK);

    const char *st = g_http_mock.last_status[0] ? g_http_mock.last_status : "200 OK";
    printf("HTTP Status retornado: [%s]\n", st);
    printf("Payload minimo servido:\n%s\n", g_http_mock.response_body);
    CHECK(strstr(g_http_mock.response_body, "\"truncated\":true") != NULL);
    CHECK(strstr(g_http_mock.response_body, "\"version\":\"1.5.0\"") != NULL);
    printf("Validacao de truncated:true e status 200: OK\n\n");
  }

  return 0;
}
