#include "storage/storage.h"
#include "storage/game_lookup.h"
#include "storage/config.h"
#include "system/settings.h"
#include "nvs.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "ASSERT FAILED: %s at %s:%d\n", #cond, __FILE__, __LINE__); assert(cond); } } while(0)

using namespace openmc::storage;

void test_1_re2_leon_path()
{
  char folder[64];
  bool has_p = false, has_ov = false;
  CHECK(ResolveGameFolder("SLUS-00421", folder, sizeof(folder), &has_p, &has_ov));
  CHECK(strcmp(folder, "SLUS-00421") == 0);
  CHECK(has_p == false);
}

void test_2_re2_claire_unified()
{
  char folder[64];
  bool has_p = false, has_ov = false;
  CHECK(ResolveGameFolder("SLUS-00592", folder, sizeof(folder), &has_p, &has_ov));
  CHECK(strcmp(folder, "SLUS-00421") == 0);
  CHECK(has_p == true);
}

void test_3_chrono_cross_disc2()
{
  char folder[64];
  bool has_p = false, has_ov = false;
  CHECK(ResolveGameFolder("SLUS-01080", folder, sizeof(folder), &has_p, &has_ov));
  CHECK(strcmp(folder, "SLUS-01041") == 0);
  CHECK(has_p == true);
}

void test_4_scus_94164_legacy_preserved()
{
  char folder[64];
  bool has_p = false, has_ov = false;
  CHECK(ResolveGameFolder("SCUS-94164", folder, sizeof(folder), &has_p, &has_ov));
  CHECK(strcmp(folder, "SCUS-94163") == 0);
  CHECK(has_p == true);
}

void test_5_max_channels_ini_parsing()
{
  CHECK(ReadFolderMaxChannels("NonExistent", "NonExistent") == 8);

  EnsureDirectoryPublic("MemoryCards/PS1/TestFolder");
  char ini_path[kCardPathMax];
  snprintf(ini_path, sizeof(ini_path), "%s/MemoryCards/PS1/TestFolder/TestFolder.ini", base_path);
  FILE *f = fopen(ini_path, "w");
  if (f)
  {
    fprintf(f, "[Settings]\nMaxChannels=3\n");
    fclose(f);
    CHECK(ReadFolderMaxChannels("TestFolder", "TestFolder") == 3);

    f = fopen(ini_path, "w");
    fprintf(f, "[Settings]\nMaxChannels=0\n");
    fclose(f);
    CHECK(ReadFolderMaxChannels("TestFolder", "TestFolder") == 8);

    f = fopen(ini_path, "w");
    fprintf(f, "[Settings]\nMaxChannels=999\n");
    fclose(f);
    CHECK(ReadFolderMaxChannels("TestFolder", "TestFolder") == 8);
  }
}

void test_6_boot_card_mount()
{
  CHECK(MountBootCard(0) == true);
  CHECK(strstr(loaded_file_path, "MemoryCards/PS1/BOOT/BootCard-1.mcd") != NULL);
}

void test_7_game2folder_override()
{
  EnsureDirectoryPublic(".sd2psx");
  char g2f_path[kCardPathMax];
  snprintf(g2f_path, sizeof(g2f_path), "%s/%s", base_path, kGame2FolderFile);
  FILE *f = fopen(g2f_path, "w");
  if (f)
  {
    fprintf(f, "[PS1]\nSLUS-00421=FF7\nSLUS-00592=IGNORADO\n");
    fclose(f);

    char folder[64];
    bool has_p = false, has_ov = false;
    CHECK(ResolveGameFolder("SLUS-00421", folder, sizeof(folder), &has_p, &has_ov));
    CHECK(strcmp(folder, "FF7") == 0);
    CHECK(has_ov == true);

    CHECK(ResolveGameFolder("SLUS-00592", folder, sizeof(folder), &has_p, &has_ov));
    CHECK(strcmp(folder, "FF7") == 0);
    unlink(g2f_path);
  }
}

void test_8_mount_card_channel_2()
{
  CHECK(MountCardChannel("SLUS-00421", 2) == true);
  CHECK(strstr(loaded_file_path, "SLUS-00421-2.mcd") != NULL);
}

void test_9_channel_above_max()
{
  char prev[CONFIG_FATFS_MAX_LFN + 1];
  strlcpy(prev, loaded_file_path, sizeof(prev));
  CHECK(MountCardChannel("SLUS-00421", 999) == false);
  CHECK(strcmp(loaded_file_path, prev) == 0);
}

void test_16_lookup_re2_disc2()
{
  GameMatch matches[kMaxGameMatches];
  size_t count = 0;
  CHECK(LookupGameByName("Resident Evil 2 disco 2", matches, kMaxGameMatches, &count) == true);
  CHECK(count > 0);
  CHECK(strcmp(matches[0].id, "SLUS-00592") == 0);
  CHECK(strcmp(matches[0].folder, "SLUS-00421") == 0);
  CHECK(matches[0].confidence >= 60);
}

void test_17_lookup_chrono_cross()
{
  GameMatch matches[kMaxGameMatches];
  size_t count = 0;
  CHECK(LookupGameByName("chrono cross", matches, kMaxGameMatches, &count) == true);
  CHECK(count > 0);
  CHECK(strcmp(matches[0].id, "SLUS-01041") == 0);
  CHECK(strcmp(matches[0].folder, "SLUS-01041") == 0);
}

void test_18_lookup_unmatched()
{
  GameMatch matches[kMaxGameMatches];
  size_t count = 0;
  CHECK(LookupGameByName("qualquer coisa xyz", matches, kMaxGameMatches, &count) == false);
  CHECK(count == 0);
}

void test_20_place_user_card_channel1_and_2()
{
  char out_path[kCardPathMax];
  unsigned ch = 0;
  CHECK(MountCardChannel("UserCards", 1) == true);
  WriteFolderChannelName("UserCards", "UserCards", 1, "Save Teste");
  CHECK(PlaceUserCard("UserCards", 1, "SLUS-00421", out_path, sizeof(out_path), &ch) == true);
  CHECK(strstr(out_path, "MemoryCards/PS1/SLUS-00421/SLUS-00421-") != NULL);
  CHECK(ch >= 1);
}

void test_21_place_user_card_no_free_channel()
{
  unsigned ch = 0;
  char out_path[kCardPathMax];
  CHECK(PlaceUserCard("UserCards", 1, "Invalid/Folder", out_path, sizeof(out_path), &ch) == false);
}

void test_22_place_preserves_clean_filename()
{
  char out_path[kCardPathMax];
  unsigned ch = 0;
  CHECK(MountCardChannel("UserCards", 2) == true);
  WriteFolderChannelName("UserCards", "UserCards", 2, "save do primo");
  CHECK(PlaceUserCard("UserCards", 2, "SLUS-00421", out_path, sizeof(out_path), &ch) == true);
  CHECK(strstr(out_path, "save do primo") == NULL);
  CHECK(strstr(out_path, "SLUS-00421-") != NULL);
}

void test_23_reject_path_traversal()
{
  CHECK(IsValidFolderName("..") == false);
  CHECK(IsValidFolderName(".") == false);
  CHECK(IsValidFolderName(".hidden") == false);
  CHECK(IsValidFolderName("ending.") == false);
  CHECK(IsValidFolderName("CON") == false);
  CHECK(IsValidFolderName("AUX") == false);
  CHECK(IsValidFolderName("folder/sub") == false);
  CHECK(IsValidFolderName("folder\\sub") == false);
}

void test_24_probe_legacy_layout()
{
  EnsureDirectoryPublic("games");
  LegacyProbeResult r = ProbeLegacyLayout();
  CHECK(r.detected == true || r.games_dir == true);

  char gdir[kCardPathMax];
  snprintf(gdir, sizeof(gdir), "%s/games", base_path);
  rmdir(gdir);
}

void test_25_legacy_sd_leaves_old_mc_untouched()
{
  char legacy_file[kCardPathMax];
  snprintf(legacy_file, sizeof(legacy_file), "%s/SLUS-00421-1.mc", base_path);
  FILE *f = fopen(legacy_file, "w");
  if (f)
  {
    fprintf(f, "dummy old mc");
    fclose(f);
  }

  SwitchToGameID("cdrom:\\SLUS_004.21;1");
  CHECK(strstr(loaded_file_path, "MemoryCards/PS1/SLUS-00421/SLUS-00421-1.mcd") != NULL);

  struct stat st;
  CHECK(stat(legacy_file, &st) == 0);
  unlink(legacy_file);
}

// Item 1: o save pode estar em qualquer canal da pasta do jogo.
void test_26_game_id_finds_card_on_channel_2()
{
  const char *folder = "SLUS-99999";
  CHECK(EnsureDirectoryPublic("MemoryCards/PS1/SLUS-99999") == true);

  char card_path[kCardPathMax];
  snprintf(card_path, sizeof(card_path), "%s/MemoryCards/PS1/%s/%s-2.mcd", base_path, folder, folder);
  CHECK(CreateBlankFormattedCard(card_path) == true);

  SwitchToGameID("cdrom:\\SLUS_999.99;1");
  CHECK(strstr(loaded_file_path, "SLUS-99999-2.mcd") != NULL);

  // Nao pode ter criado um -1 em branco ao lado.
  char first_path[kCardPathMax];
  snprintf(first_path, sizeof(first_path), "%s/MemoryCards/PS1/%s/%s-1.mcd", base_path, folder, folder);
  struct stat st;
  CHECK(stat(first_path, &st) != 0);
}

// Item 3: mover cartao conserva o numero do canal quando o destino esta livre.
void test_27_place_preserves_channel_number()
{
  char out_path[kCardPathMax];
  unsigned ch = 0;

  CHECK(MountCardChannel("UserCards", 5) == true);
  CHECK(PlaceUserCard("UserCards", 5, "SLUS-88888", out_path, sizeof(out_path), &ch) == true);
  CHECK(ch == 5);
  CHECK(strstr(out_path, "SLUS-88888-5.mcd") != NULL);
}

// Item 6b: configuracao em NVS.
void test_28_config_nvs_roundtrip()
{
  nvs_test_reset();

  CHECK(openmc::storage::config::SaveConfig("client", "MinhaRede", "senha12345") == true);
  CHECK(openmc::storage::config::LoadConfig() == true);
  CHECK(openmc::storage::config::Source() == openmc::storage::config::kSourceNvs);
  CHECK(strcmp(openmc::storage::config::SourceName(), "nvs") == 0);
  CHECK(strcmp((const char *)openmc::system::settings.ssid, "MinhaRede") == 0);
  CHECK(strcmp((const char *)openmc::system::settings.password, "senha12345") == 0);
  CHECK(openmc::system::settings.wifi_mode == openmc::wifi::Mode::kClient);

  // Entradas invalidas sao recusadas sem tocar na NVS.
  CHECK(openmc::storage::config::SaveConfig("client", "", "senha12345") == false);
  CHECK(openmc::storage::config::SaveConfig("modoerrado", "Rede", "senha12345") == false);
  CHECK(openmc::storage::config::SaveConfig("client", "Rede", "curta") == false);

  nvs_test_reset();
}

// Item 6b: importacao unica do config.txt antigo.
void test_29_config_txt_import()
{
  nvs_test_reset();

  FILE *f = fopen("test_config.txt", "w");
  CHECK(f != NULL);
  fprintf(f, "mode=client\nssid=RedeAntiga\npassword=senha12345\n");
  fclose(f);

  CHECK(openmc::storage::config::LoadConfig() == true);
  CHECK(openmc::storage::config::Source() == openmc::storage::config::kSourceFile);
  CHECK(strcmp((const char *)openmc::system::settings.ssid, "RedeAntiga") == 0);

  // O arquivo saiu da raiz e foi para .sd2psx, sem ser apagado.
  struct stat st;
  CHECK(stat("test_config.txt", &st) != 0);
  CHECK(stat("tests_sdcard/.sd2psx/config.txt.imported", &st) == 0);

  // A importacao deixou a configuracao na NVS: o proximo boot vem de la.
  CHECK(openmc::storage::config::LoadConfig() == true);
  CHECK(openmc::storage::config::Source() == openmc::storage::config::kSourceNvs);

  nvs_test_reset();
}

int main()
{
  strcpy(base_path, "tests_sdcard");
  mkdir("tests_sdcard", 0755);
  ready = true;

  // Copy titles.db to base_path for tests 16-18
  const char *titles_src = "sdcard_assets/titles.db";
  FILE *fsrc = fopen(titles_src, "rb");
  if (!fsrc)
  {
    titles_src = "../../sdcard_assets/titles.db";
    fsrc = fopen(titles_src, "rb");
  }
  char titles_dst[] = "tests_sdcard/titles.db";
  if (fsrc)
  {
    FILE *fdst = fopen(titles_dst, "wb");
    if (fdst)
    {
      char buf[4096];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0)
      {
        fwrite(buf, 1, n, fdst);
      }
      fclose(fdst);
    }
    fclose(fsrc);
  }

  test_1_re2_leon_path();
  test_2_re2_claire_unified();
  test_3_chrono_cross_disc2();
  test_4_scus_94164_legacy_preserved();
  test_5_max_channels_ini_parsing();
  test_6_boot_card_mount();
  test_7_game2folder_override();
  test_8_mount_card_channel_2();
  test_9_channel_above_max();
  test_16_lookup_re2_disc2();
  test_17_lookup_chrono_cross();
  test_18_lookup_unmatched();
  test_20_place_user_card_channel1_and_2();
  test_21_place_user_card_no_free_channel();
  test_22_place_preserves_clean_filename();
  test_23_reject_path_traversal();
  test_24_probe_legacy_layout();
  test_25_legacy_sd_leaves_old_mc_untouched();
  test_26_game_id_finds_card_on_channel_2();
  test_27_place_preserves_channel_number();
  test_28_config_nvs_roundtrip();
  test_29_config_txt_import();
  printf("All test_main tests passed successfully.\n");
  return 0;
}
