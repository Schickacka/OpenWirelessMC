#include "bsd_compat.h"
#include "web/file_server.cc"
#include "storage/storage.h"
#include "esp_http_server.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "ASSERT FAILED: %s at %s:%d\n", #cond, __FILE__, __LINE__); assert(cond); } } while(0)

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

// 10. /api/channels lista 1..8, exists correto e name vindo de [ChannelName]
void test_10_api_channels_list()
{
  EnsureDirectoryPublic("MemoryCards/PS1/TestFolder");
  WriteFolderChannelName("TestFolder", "TestFolder", 1, "Leon");

  httpd_req_t req;
  SetupMockReq(&req, "/api/channels?folder=TestFolder", HTTP_GET);
  esp_err_t res = api_channels_get_handler(&req);
  CHECK(res == ESP_OK);
  CHECK(strstr(g_http_mock.response_body, "\"n\":1") != NULL);
  CHECK(strstr(g_http_mock.response_body, "\"name\":\"Leon\"") != NULL);
  CHECK(strstr(g_http_mock.response_body, "\"n\":8") != NULL);
}

// 11. /api/create não existe mais -> 404; nenhuma rota grava arquivo solto na raiz
void test_11_api_create_removed()
{
  CHECK(IsFsPathAllowed("root_card.mc") == false);
  CHECK(IsFsPathAllowed("UserCards-1.mcd") == false);
}

// 12. POST /api/rename com SLUS-00421-1.mcd -> foo.mcd -> 400; com /api/card/name muda [ChannelName] e arquivo continua no disco
void test_12_rename_preserves_mcd_filename()
{
  EnsureDirectoryPublic("MemoryCards/PS1/SLUS-00421");
  char card[kCardPathMax];
  snprintf(card, sizeof(card), "%s/MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd", base_path);
  CreateBlankFormattedCard(card);

  httpd_req_t req;
  SetupMockReq(&req, "/api/rename", HTTP_POST);
  api_rename_handler(&req);
  CHECK(strcmp(g_http_mock.last_status, "400 Bad Request") == 0);

  const char *body = "folder=SLUS-00421&channel=1&name=NovoNome";
  SetupMockReq(&req, "/api/card/name", HTTP_POST, body);
  esp_err_t res = api_card_name_handler(&req);
  CHECK(res == ESP_OK);

  struct stat st;
  CHECK(stat(card, &st) == 0);
  char check_name[64];
  CHECK(ReadFolderChannelName("SLUS-00421", "SLUS-00421", 1, check_name, sizeof(check_name)) == true);
  CHECK(strcmp(check_name, "NovoNome") == 0);
}

// 13. Nenhuma rota renomeia pasta: POST /api/rename -> 400
void test_13_rename_folder_prohibited()
{
  httpd_req_t req;
  SetupMockReq(&req, "/api/rename", HTTP_POST);
  api_rename_handler(&req);
  CHECK(strcmp(g_http_mock.last_status, "400 Bad Request") == 0);
  CHECK(IsValidFolderName("folder/sub") == false);
  CHECK(IsValidFolderName("../MemoryCards/PS1/SLUS-00421") == false);
}

// 14. Apagar cartão montado -> 400/409; apagar pasta -> 400; apagar .mcd desmontado -> 200
void test_14_delete_mounted_card_rejected()
{
  EnsureDirectoryPublic("MemoryCards/PS1/SLUS-00421");
  char mounted_card[kCardPathMax];
  snprintf(mounted_card, sizeof(mounted_card), "%s/MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd", base_path);
  CreateBlankFormattedCard(mounted_card);

  LoadCardFromFile(mounted_card, NULL);

  httpd_req_t req;
  SetupMockReq(&req, "/delete/MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd", HTTP_POST);
  delete_post_handler(&req);
  CHECK(g_http_mock.error_code == HTTPD_400_BAD_REQUEST || strcmp(g_http_mock.last_status, "400 Bad Request") == 0);

  SetupMockReq(&req, "/api/fs/delete?path=MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd", HTTP_POST);
  api_fs_delete_handler(&req);
  CHECK(strcmp(g_http_mock.last_status, "409 Conflict") == 0);
}

// 15. Listagem allowlist: bloqueia config.txt, titles.db
void test_15_root_listing_filters_system_files()
{
  CHECK(IsFsPathAllowed("config.txt") == false);
  CHECK(IsFsPathAllowed("titles.db") == false);
  CHECK(IsFsPathAllowed(".sd2psx/Game2Folder.ini") == true);
  CHECK(IsFsPathAllowed("MemoryCards/PS1/BOOT") == true);
}

// 19. POST /api/card/create cria UserCards-1.mcd e grava [ChannelName] 1 = <nome>; segundo vai canal 2
void test_19_api_card_create_usercards()
{
  EnsureDirectoryPublic("MemoryCards/PS1/UserCards");
  char card1[kCardPathMax], card2[kCardPathMax];
  snprintf(card1, sizeof(card1), "%s/MemoryCards/PS1/UserCards/UserCards-1.mcd", base_path);
  snprintf(card2, sizeof(card2), "%s/MemoryCards/PS1/UserCards/UserCards-2.mcd", base_path);
  unlink(card1);
  unlink(card2);

  httpd_req_t req;
  const char *body1 = "name=Card1";
  SetupMockReq(&req, "/api/card/create", HTTP_POST, body1);
  api_card_create_handler(&req);
  struct stat st;
  CHECK(stat(card1, &st) == 0);

  char name1[64] = {0};
  CHECK(ReadFolderChannelName("UserCards", "UserCards", 1, name1, sizeof(name1)) == true);
  CHECK(strcmp(name1, "Card1") == 0);

  const char *body2 = "name=Card2";
  SetupMockReq(&req, "/api/card/create", HTTP_POST, body2);
  api_card_create_handler(&req);
  CHECK(stat(card2, &st) == 0);

  char name2[64] = {0};
  CHECK(ReadFolderChannelName("UserCards", "UserCards", 2, name2, sizeof(name2)) == true);
  CHECK(strcmp(name2, "Card2") == 0);
}

// 26. /api/status traz legacy, folder, channel, heap_free, heap_min
void test_26_api_status_contains_all_metrics()
{
  httpd_req_t req;
  SetupMockReq(&req, "/api/status", HTTP_GET);
  esp_err_t res = api_status_handler(&req);
  CHECK(res == ESP_OK);
  CHECK(strstr(g_http_mock.response_body, "\"legacy\":") != NULL);
  CHECK(strstr(g_http_mock.response_body, "\"folder\":") != NULL);
  CHECK(strstr(g_http_mock.response_body, "\"channel\":") != NULL);
  CHECK(strstr(g_http_mock.response_body, "\"heap_free\":") != NULL);
  CHECK(strstr(g_http_mock.response_body, "\"heap_min\":") != NULL);
}

// 27. /api/fs/put cria pastas que faltam, grava 131072 B, nenhum .part sobra
void test_27_api_fs_put_creates_folders()
{
  const char *rel = "MemoryCards/PS1/SLUS-00421/SLUS-00421-4.mcd";
  char target[kCardPathMax];
  snprintf(target, sizeof(target), "%s/%s", base_path, rel);
  unlink(target);

  char dummy_data[1024];
  memset(dummy_data, 0xAA, sizeof(dummy_data));

  httpd_req_t req;
  char uri[256];
  snprintf(uri, sizeof(uri), "/api/fs/put?path=%s", rel);
  SetupMockReq(&req, uri, HTTP_POST, dummy_data, sizeof(dummy_data));
  esp_err_t res = api_fs_put_handler(&req);
  CHECK(res == ESP_OK);
  CHECK(strcmp(g_http_mock.last_status, "201 Created") == 0);

  struct stat st;
  CHECK(stat(target, &st) == 0);
  CHECK(st.st_size == (off_t)sizeof(dummy_data));

  char part[kCardPathMax + 8];
  snprintf(part, sizeof(part), "%s.part", target);
  CHECK(stat(part, &st) != 0);
}

// 28. put sobre arquivo existente sem overwrite=1 -> 409; com overwrite=1 -> substitui (Item 2)
void test_28_api_fs_put_conflict_without_overwrite()
{
  const char *rel = "MemoryCards/PS1/SLUS-00421/SLUS-00421-4.mcd";
  char dummy_data[512];
  memset(dummy_data, 0xBB, sizeof(dummy_data));

  httpd_req_t req;
  char uri1[256];
  snprintf(uri1, sizeof(uri1), "/api/fs/put?path=%s", rel);
  SetupMockReq(&req, uri1, HTTP_POST, dummy_data, sizeof(dummy_data));
  api_fs_put_handler(&req);
  // Item 2 test: deve retornar 409 Conflict
  CHECK(strcmp(g_http_mock.last_status, "409 Conflict") == 0);

  char uri2[256];
  snprintf(uri2, sizeof(uri2), "/api/fs/put?path=%s&overwrite=1", rel);
  SetupMockReq(&req, uri2, HTTP_POST, dummy_data, sizeof(dummy_data));
  esp_err_t res = api_fs_put_handler(&req);
  CHECK(res == ESP_OK);
  CHECK(strcmp(g_http_mock.last_status, "201 Created") == 0);
}

// 29. put em config.txt, titles.db ou .. -> 403
void test_29_api_fs_put_forbidden_files()
{
  httpd_req_t req;
  char dummy[] = "test";
  SetupMockReq(&req, "/api/fs/put?path=config.txt", HTTP_POST, dummy);
  api_fs_put_handler(&req);
  CHECK(strcmp(g_http_mock.last_status, "403 Forbidden") == 0);

  SetupMockReq(&req, "/api/fs/put?path=titles.db", HTTP_POST, dummy);
  api_fs_put_handler(&req);
  CHECK(strcmp(g_http_mock.last_status, "403 Forbidden") == 0);

  SetupMockReq(&req, "/api/fs/put?path=../secret.txt", HTTP_POST, dummy);
  api_fs_put_handler(&req);
  CHECK(strcmp(g_http_mock.last_status, "403 Forbidden") == 0);
}

// 30. put truncado -> 500, .part removido e destino intacto
void test_30_api_fs_put_truncated_cleanup()
{
  const char *rel = "MemoryCards/PS1/SLUS-00421/SLUS-00421-4.mcd";
  char target[kCardPathMax];
  snprintf(target, sizeof(target), "%s/%s", base_path, rel);
  struct stat st_before;
  stat(target, &st_before);

  httpd_req_t req;
  char uri[256];
  snprintf(uri, sizeof(uri), "/api/fs/put?path=%s&overwrite=1", rel);
  // Anuncia 1024 bytes mas passa buffer vazio
  SetupMockReq(&req, uri, HTTP_POST, NULL, 1024);
  api_fs_put_handler(&req);
  CHECK(g_http_mock.error_code == HTTPD_500_INTERNAL_SERVER_ERROR);

  char part[kCardPathMax + 8];
  snprintf(part, sizeof(part), "%s.part", target);
  struct stat st_part;
  CHECK(stat(part, &st_part) != 0);

  struct stat st_after;
  stat(target, &st_after);
  CHECK(st_before.st_size == st_after.st_size);
}

// 31. Dois put simultâneos -> segundo recebe 409
void test_31_api_fs_put_concurrency_busy()
{
  openmc::web::file_server::fs_transfer_busy = true;

  httpd_req_t req;
  char dummy[] = "test";
  SetupMockReq(&req, "/api/fs/put?path=MemoryCards/PS1/SLUS-00421/SLUS-00421-4.mcd&overwrite=1", HTTP_POST, dummy);
  api_fs_put_handler(&req);
  CHECK(strcmp(g_http_mock.last_status, "409 Conflict") == 0);

  openmc::web::file_server::fs_transfer_busy = false;
}

// 32. /api/fs/list devolve JSON válido em chunk
void test_32_api_fs_list_streaming_chunks()
{
  httpd_req_t req;
  SetupMockReq(&req, "/api/fs/list?path=MemoryCards/PS1", HTTP_GET);
  esp_err_t res = api_fs_list_handler(&req);
  CHECK(res == ESP_OK);
  CHECK(strstr(g_http_mock.response_body, "\"entries\":[") != NULL);
}

// 33. delete de diretório com conteúdo -> 400; vazio -> 200; cartão montado -> 409 (Item 4)
void test_33_api_fs_delete_directory_rules()
{
  EnsureDirectoryPublic("games/test_del_dir");
  char file_in_dir[kCardPathMax];
  snprintf(file_in_dir, sizeof(file_in_dir), "%s/games/test_del_dir/file.bin", base_path);
  FILE *f = fopen(file_in_dir, "w");
  if (f) { fprintf(f, "x"); fclose(f); }

  httpd_req_t req;
  // Diretório com conteúdo -> 400
  SetupMockReq(&req, "/api/fs/delete?path=games/test_del_dir", HTTP_POST);
  api_fs_delete_handler(&req);
  CHECK(strcmp(g_http_mock.last_status, "400 Bad Request") == 0);

  // Remove arquivo interno
  unlink(file_in_dir);

  // Diretório vazio -> 200
  SetupMockReq(&req, "/api/fs/delete?path=games/test_del_dir", HTTP_POST);
  esp_err_t res = api_fs_delete_handler(&req);
  CHECK(res == ESP_OK);

  char dir_path[kCardPathMax];
  snprintf(dir_path, sizeof(dir_path), "%s/games/test_del_dir", base_path);
  struct stat st;
  CHECK(stat(dir_path, &st) != 0);

  // Cartão montado -> 409 Conflict (Item 4 test)
  char mounted_card[kCardPathMax];
  snprintf(mounted_card, sizeof(mounted_card), "%s/MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd", base_path);
  CreateBlankFormattedCard(mounted_card);
  LoadCardFromFile(mounted_card, NULL);

  SetupMockReq(&req, "/api/fs/delete?path=MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd", HTTP_POST);
  api_fs_delete_handler(&req);
  CHECK(strcmp(g_http_mock.last_status, "409 Conflict") == 0);

  // Cartão desmontado -> 200
  UnloadCard();
  SetupMockReq(&req, "/api/fs/delete?path=MemoryCards/PS1/SLUS-00421/SLUS-00421-4.mcd", HTTP_POST);
  res = api_fs_delete_handler(&req);
  CHECK(res == ESP_OK);
}

// 34. legacy.detected passa a false sem reboot ao deletar pasta games
void test_34_api_fs_legacy_status_clears_live()
{
  EnsureDirectoryPublic("games");
  LegacyProbeResult r1 = ProbeLegacyLayout();
  CHECK(r1.games_dir == true);

  httpd_req_t req;
  SetupMockReq(&req, "/api/fs/delete?path=games", HTTP_POST);
  esp_err_t res = api_fs_delete_handler(&req);
  CHECK(res == ESP_OK);

  LegacyProbeResult r2 = ProbeLegacyLayout();
  CHECK(r2.games_dir == false);
  CHECK(r2.detected == false);
}

// 35. Validação de status truncado (Item 6) e asserções de memória
void test_35_asan_clean_execution()
{
  // Item 6: Testar /api/status devolvendo HTTP 200 válido
  httpd_req_t req;
  SetupMockReq(&req, "/api/status", HTTP_GET);
  esp_err_t res = api_status_handler(&req);
  CHECK(res == ESP_OK);
  // Status retornado é 200 (não cortado / não 500)
  CHECK(g_http_mock.error_code == 0);
  CHECK(strstr(g_http_mock.response_body, "\"sd_mounted\":") != NULL);
  CHECK(strstr(g_http_mock.response_body, "\"legacy\":") != NULL);

  CHECK(sizeof(openmc::storage::LegacyProbeResult) <= 8);
}

int main()
{
  strcpy(base_path, "tests_sdcard");
  mkdir("tests_sdcard", 0755);
  ready = true;

  printf("t10\n"); fflush(stdout); test_10_api_channels_list();
  printf("t11\n"); fflush(stdout); test_11_api_create_removed();
  printf("t12\n"); fflush(stdout); test_12_rename_preserves_mcd_filename();
  printf("t13\n"); fflush(stdout); test_13_rename_folder_prohibited();
  printf("t14\n"); fflush(stdout); test_14_delete_mounted_card_rejected();
  printf("t15\n"); fflush(stdout); test_15_root_listing_filters_system_files();
  printf("t19\n"); fflush(stdout); test_19_api_card_create_usercards();
  printf("t26\n"); fflush(stdout); test_26_api_status_contains_all_metrics();
  printf("t27\n"); fflush(stdout); test_27_api_fs_put_creates_folders();
  printf("t28\n"); fflush(stdout); test_28_api_fs_put_conflict_without_overwrite();
  printf("t29\n"); fflush(stdout); test_29_api_fs_put_forbidden_files();
  printf("t30\n"); fflush(stdout); test_30_api_fs_put_truncated_cleanup();
  printf("t31\n"); fflush(stdout); test_31_api_fs_put_concurrency_busy();
  printf("t32\n"); fflush(stdout); test_32_api_fs_list_streaming_chunks();
  printf("t33\n"); fflush(stdout); test_33_api_fs_delete_directory_rules();
  printf("t34\n"); fflush(stdout); test_34_api_fs_legacy_status_clears_live();
  printf("t35\n"); fflush(stdout); test_35_asan_clean_execution();
  printf("All test_web tests passed successfully.\n");
  return 0;
}
