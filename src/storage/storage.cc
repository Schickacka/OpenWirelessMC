#include "storage/storage.h"
#include "storage/ps1_empty_card.h"
#include "storage/parent_db.h"
#include "storage/sdcard.h"

#include "playstation/sio.h"
#include "playstation/sio_memory_card.h"
#include "playstation/spi.h"
#include "pins.h"
#include "system/timer.h"
#include "logging.h"

#include <sdkconfig.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#define FLUSH_QUIET_TICKS 15
#define FLUSH_MAX_DIRTY_MS 1000
#define FLUSH_RETRY_DELAY_MS 3000

namespace openmc
{
  namespace storage
  {
    const char kCardsRoot[] = "MemoryCards/PS1";
    const char kBootFolder[] = "BOOT";
    const char kBootCardBase[] = "BootCard";
    const char kGame2FolderDir[] = ".sd2psx";
    const char kGame2FolderFile[] = ".sd2psx/Game2Folder.ini";
    const char kUserCardsFolder[] = "UserCards";

    char loaded_file_path[CONFIG_FATFS_MAX_LFN + 1];
    char base_path[16];
    bool ready = false;

    static uint16_t saved_version[kCardSectorCount];
    SemaphoreHandle_t storage_mutex = NULL;

    static char pending_game_id[64] = {0};

    static void RememberPendingGameID(const char *game_id_str)
    {
      if (game_id_str && game_id_str[0] != '\0')
      {
        strlcpy(pending_game_id, game_id_str, sizeof(pending_game_id));
        ESP_LOGI(kLogPrefix, "RememberPendingGameID: saved '%s' for when SD is ready\n", pending_game_id);
      }
    }

    static void ApplyPendingGameID()
    {
      if (pending_game_id[0] != '\0')
      {
        char gid[64];
        strlcpy(gid, pending_game_id, sizeof(gid));
        pending_game_id[0] = '\0';
        ESP_LOGI(kLogPrefix, "ApplyPendingGameID: applying saved GameID '%s'\n", gid);
        SwitchToGameID(gid);
      }
    }

    static void MarkAllSectorsSaved()
    {
      for (size_t i = 0; i < kCardSectorCount; i++)
      {
        saved_version[i] = sio::memory_card::sector_version[i];
      }
    }

    static TickType_t MsToTicks(uint32_t ms)
    {
      TickType_t ticks = pdMS_TO_TICKS(ms);
      return ticks > 0 ? ticks : 1;
    }

    static bool MkdirP(const char *relative_path)
    {
      if (!relative_path || relative_path[0] == '\0')
      {
        return false;
      }

      char full_path[kCardPathMax];
      snprintf(full_path, sizeof(full_path), "%s/%s", base_path, relative_path);

      char temp[kCardPathMax];
      size_t len = strlen(full_path);
      if (len >= sizeof(temp))
      {
        return false;
      }

      strlcpy(temp, full_path, sizeof(temp));
      for (size_t i = 1; i < len; i++)
      {
        if (temp[i] == '/')
        {
          temp[i] = '\0';
          struct stat st;
          if (stat(temp, &st) != 0)
          {
            if (mkdir(temp, 0755) != 0 && errno != EEXIST)
            {
              ESP_LOGE(kLogPrefix, "MkdirP failed on '%s' (errno %d)\n", temp, errno);
              return false;
            }
          }
          temp[i] = '/';
        }
      }

      struct stat st;
      if (stat(temp, &st) != 0)
      {
        if (mkdir(temp, 0755) != 0 && errno != EEXIST)
        {
          ESP_LOGE(kLogPrefix, "MkdirP failed on leaf '%s' (errno %d)\n", temp, errno);
          return false;
        }
      }

      return true;
    }

    bool EnsureDirectoryPublic(const char *rel_path)
    {
      return MkdirP(rel_path);
    }

    bool IsValidFolderName(const char *name)
    {
      if (!name || name[0] == '\0')
      {
        return false;
      }
      size_t len = strlen(name);
      if (len > kMaxFolderNameLen)
      {
        return false;
      }
      if (name[0] == '.' || name[len - 1] == '.')
      {
        return false;
      }
      for (size_t i = 0; i < len; i++)
      {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '-')
        {
          return false;
        }
      }

      static const char *const kReservedNames[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
      };
      char base[16];
      size_t base_len = strcspn(name, ".");
      if (base_len < sizeof(base))
      {
        memcpy(base, name, base_len);
        base[base_len] = '\0';
        for (size_t i = 0; i < sizeof(kReservedNames) / sizeof(kReservedNames[0]); i++)
        {
          if (strcasecmp(base, kReservedNames[i]) == 0)
          {
            return false;
          }
        }
      }

      return true;
    }

    static bool EnsureCardsRoot()
    {
      char path[64];
      snprintf(path, sizeof(path), "MemoryCards");
      if (!MkdirP(path))
      {
        return false;
      }
      snprintf(path, sizeof(path), "%s", kCardsRoot);
      return MkdirP(path);
    }

    static bool EnsureGameFolder(const char *folder)
    {
      if (!IsValidFolderName(folder))
      {
        return false;
      }
      char path[kCardPathMax];
      snprintf(path, sizeof(path), "%s/%s", kCardsRoot, folder);
      return MkdirP(path);
    }

    static bool ParseIniValue(const char *path, const char *section, const char *key, char *out_val, size_t out_len)
    {
      if (!path || !section || !key || !out_val || out_len == 0)
      {
        return false;
      }

      FILE *f = fopen(path, "r");
      if (!f)
      {
        return false;
      }

      char line[256];
      bool in_target_section = false;

      while (fgets(line, sizeof(line), f))
      {
        char *p = line;
        while (*p == ' ' || *p == '\t')
        {
          p++;
        }
        if (*p == '#' || *p == ';' || *p == '\r' || *p == '\n' || *p == '\0')
        {
          continue;
        }

        if (*p == '[')
        {
          char *end_sec = strchr(p, ']');
          if (end_sec)
          {
            *end_sec = '\0';
            in_target_section = (strcasecmp(p + 1, section) == 0);
          }
          continue;
        }

        if (!in_target_section)
        {
          continue;
        }

        char *eq = strchr(p, '=');
        if (!eq)
        {
          continue;
        }

        char *k_end = eq - 1;
        while (k_end >= p && (*k_end == ' ' || *k_end == '\t'))
        {
          *k_end = '\0';
          k_end--;
        }
        *eq = '\0';

        if (strcasecmp(p, key) == 0)
        {
          char *val = eq + 1;
          while (*val == ' ' || *val == '\t')
          {
            val++;
          }
          size_t v_len = strcspn(val, "\r\n#;");
          while (v_len > 0 && (val[v_len - 1] == ' ' || val[v_len - 1] == '\t'))
          {
            v_len--;
          }
          if (v_len > 0 && v_len < out_len)
          {
            memcpy(out_val, val, v_len);
            out_val[v_len] = '\0';
            fclose(f);
            return true;
          }
        }
      }

      fclose(f);
      return false;
    }

    bool ReadCardFolderOverride(const char *parent_id, char *out, size_t out_len)
    {
      if (!parent_id || !out || out_len == 0)
      {
        return false;
      }
      char ini_path[kCardPathMax];
      snprintf(ini_path, sizeof(ini_path), "%s/%s", base_path, kGame2FolderFile);
      return ParseIniValue(ini_path, "PS1", parent_id, out, out_len);
    }

    unsigned ReadFolderMaxChannels(const char *folder, const char *base_name)
    {
      if (!folder || !base_name)
      {
        return kDefaultMaxChannels;
      }
      char ini_path[kCardPathMax];
      snprintf(ini_path, sizeof(ini_path), "%s/%s/%s/%s.ini", base_path, kCardsRoot, folder, base_name);

      char val[16];
      if (ParseIniValue(ini_path, "Settings", "MaxChannels", val, sizeof(val)))
      {
        char *endptr = NULL;
        long parsed = strtol(val, &endptr, 10);
        if (endptr != val && parsed >= 1 && parsed <= (long)kCardChannelMax)
        {
          return (unsigned)parsed;
        }
      }
      return kDefaultMaxChannels;
    }

    bool ReadFolderChannelName(const char *folder, const char *base_name, unsigned channel, char *out, size_t out_len)
    {
      if (!folder || !base_name || !out || out_len == 0 || channel == 0)
      {
        return false;
      }
      char ini_path[kCardPathMax];
      snprintf(ini_path, sizeof(ini_path), "%s/%s/%s/%s.ini", base_path, kCardsRoot, folder, base_name);

      char key[16];
      snprintf(key, sizeof(key), "%u", channel);
      return ParseIniValue(ini_path, "ChannelName", key, out, out_len);
    }

    bool WriteFolderChannelName(const char *folder, const char *base_name, unsigned channel, const char *name)
    {
      if (!folder || !base_name || channel == 0 || !name)
      {
        return false;
      }

      char clean_name[128];
      size_t cn = 0;
      for (size_t i = 0; name[i] != '\0' && cn < sizeof(clean_name) - 1; i++)
      {
        if (name[i] != '\r' && name[i] != '\n')
        {
          clean_name[cn++] = name[i];
        }
      }
      clean_name[cn] = '\0';

      char ini_path[kCardPathMax];
      snprintf(ini_path, sizeof(ini_path), "%s/%s/%s/%s.ini", base_path, kCardsRoot, folder, base_name);

      struct stat st;
      if (stat(ini_path, &st) == 0 && st.st_size > 4096)
      {
        return false;
      }

      char temp_path[kCardPathMax + 8];
      snprintf(temp_path, sizeof(temp_path), "%s.tmp", ini_path);

      FILE *in = fopen(ini_path, "r");
      FILE *out = fopen(temp_path, "w");
      if (!out)
      {
        if (in) fclose(in);
        return false;
      }

      char line[256];
      bool in_channel_name = false;
      bool channel_name_section_seen = false;
      bool channel_written = false;
      char target_key[16];
      snprintf(target_key, sizeof(target_key), "%u", channel);

      if (in)
      {
        while (fgets(line, sizeof(line), in))
        {
          char *p = line;
          while (*p == ' ' || *p == '\t') p++;
          if (*p == '[')
          {
            if (in_channel_name && !channel_written)
            {
              fprintf(out, "%u=%s\n", channel, clean_name);
              channel_written = true;
            }
            char *end_sec = strchr(p, ']');
            if (end_sec)
            {
              *end_sec = '\0';
              if (strcasecmp(p + 1, "ChannelName") == 0)
              {
                if (channel_name_section_seen)
                {
                  in_channel_name = true;
                  continue;
                }
                channel_name_section_seen = true;
                in_channel_name = true;
              }
              else
              {
                in_channel_name = false;
              }
              *end_sec = ']';
            }
            fputs(line, out);
          }
          else if (in_channel_name)
          {
            char *eq = strchr(p, '=');
            if (eq)
            {
              *eq = '\0';
              char *k = p;
              while (*k == ' ' || *k == '\t') k++;
              if (strcasecmp(k, target_key) == 0)
              {
                if (!channel_written)
                {
                  fprintf(out, "%u=%s\n", channel, clean_name);
                  channel_written = true;
                }
                continue;
              }
              *eq = '=';
            }
            fputs(line, out);
          }
          else
          {
            fputs(line, out);
          }
        }
        fclose(in);
      }

      if (!channel_written)
      {
        if (!channel_name_section_seen)
        {
          fprintf(out, "\n[ChannelName]\n");
        }
        fprintf(out, "%u=%s\n", channel, clean_name);
      }

      fflush(out);
      fsync(fileno(out));
      fclose(out);

      char bak_path[kCardPathMax + 8];
      snprintf(bak_path, sizeof(bak_path), "%s.bak", ini_path);
      unlink(bak_path);
      bool dest_existed = (stat(ini_path, &st) == 0);
      if (dest_existed)
      {
        rename(ini_path, bak_path);
      }
      if (rename(temp_path, ini_path) != 0)
      {
        unlink(temp_path);
        if (dest_existed)
        {
          rename(bak_path, ini_path);
        }
        return false;
      }
      if (dest_existed)
      {
        unlink(bak_path);
      }
      return true;
    }

    bool RemoveFolderChannelName(const char *folder, const char *base_name, unsigned channel)
    {
      if (!folder || !base_name || channel == 0)
      {
        return false;
      }

      char ini_path[kCardPathMax];
      snprintf(ini_path, sizeof(ini_path), "%s/%s/%s/%s.ini", base_path, kCardsRoot, folder, base_name);

      struct stat st;
      if (stat(ini_path, &st) != 0)
      {
        return true;
      }
      if (st.st_size > 4096)
      {
        return false;
      }

      char temp_path[kCardPathMax + 8];
      snprintf(temp_path, sizeof(temp_path), "%s.tmp", ini_path);

      FILE *in = fopen(ini_path, "r");
      if (!in)
      {
        return true;
      }

      FILE *out = fopen(temp_path, "w");
      if (!out)
      {
        fclose(in);
        return false;
      }

      char line[256];
      bool in_channel_name = false;
      char target_key[16];
      snprintf(target_key, sizeof(target_key), "%u", channel);

      while (fgets(line, sizeof(line), in))
      {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[')
        {
          char *end_sec = strchr(p, ']');
          if (end_sec)
          {
            *end_sec = '\0';
            in_channel_name = (strcasecmp(p + 1, "ChannelName") == 0);
            *end_sec = ']';
          }
        }
        else if (in_channel_name)
        {
          char *eq = strchr(p, '=');
          if (eq)
          {
            *eq = '\0';
            char *k = p;
            while (*k == ' ' || *k == '\t') k++;
            if (strcasecmp(k, target_key) == 0)
            {
              continue;
            }
            *eq = '=';
          }
        }
        fputs(line, out);
      }

      fclose(in);
      fflush(out);
      fsync(fileno(out));
      fclose(out);

      char bak_path[kCardPathMax + 8];
      snprintf(bak_path, sizeof(bak_path), "%s.bak", ini_path);
      unlink(bak_path);
      if (rename(ini_path, bak_path) == 0)
      {
        if (rename(temp_path, ini_path) != 0)
        {
          unlink(temp_path);
          rename(bak_path, ini_path);
          return false;
        }
        unlink(bak_path);
      }
      else
      {
        unlink(temp_path);
        return false;
      }
      return true;
    }

    static bool ResolveCardFolder(const char *normalized_id, char *out_folder, size_t out_len,
                                  bool *out_has_parent, bool *out_has_override)
    {
      if (!normalized_id || !out_folder || out_len == 0)
      {
        return false;
      }

      const char *parent = FindParentGameID(normalized_id);
      const char *effective_parent = parent ? parent : normalized_id;
      if (out_has_parent)
      {
        *out_has_parent = (parent != NULL);
      }

      char override_folder[64];
      if (ReadCardFolderOverride(effective_parent, override_folder, sizeof(override_folder)))
      {
        if (out_has_override)
        {
          *out_has_override = true;
        }
        strlcpy(out_folder, override_folder, out_len);
      }
      else
      {
        if (out_has_override)
        {
          *out_has_override = false;
        }
        strlcpy(out_folder, effective_parent, out_len);
      }

      return IsValidFolderName(out_folder);
    }

    bool ResolveGameFolder(const char *normalized_id, char *out_folder, size_t out_len,
                           bool *out_has_parent, bool *out_has_override)
    {
      return ResolveCardFolder(normalized_id, out_folder, out_len, out_has_parent, out_has_override);
    }

    static bool BuildGameCardPath(const char *folder, unsigned channel, const char *ext, char *out, size_t out_len)
    {
      if (!folder || !ext || !out || out_len == 0)
      {
        return false;
      }
      int written = snprintf(out, out_len, "%s/%s/%s/%s-%u%s",
                             base_path, kCardsRoot, folder, folder, channel, ext);
      return (written > 0 && (size_t)written < out_len);
    }

    static bool BuildBootCardPath(unsigned channel, char *out, size_t out_len)
    {
      if (!out || out_len == 0)
      {
        return false;
      }
      int written = snprintf(out, out_len, "%s/%s/%s/%s-%u.mcd",
                             base_path, kCardsRoot, kBootFolder, kBootCardBase, channel);
      return (written > 0 && (size_t)written < out_len);
    }

    static bool FindGameCard(const char *id, const char *folder, unsigned channel, char *out_path, size_t out_len)
    {
      (void)id;
      if (!BuildGameCardPath(folder, channel, ".mcd", out_path, out_len))
      {
        return false;
      }
      struct stat st;
      return (stat(out_path, &st) == 0 && st.st_size == (off_t)kCardSizeBytes);
    }

    static bool IsValidCardImage(const char *path)
    {
      struct stat st;
      if (stat(path, &st) != 0 || st.st_size != (off_t)kCardSizeBytes)
      {
        return false;
      }

      FILE *f = fopen(path, "rb");
      if (!f)
      {
        return false;
      }

      uint8_t header[kCardSectorSize];
      size_t got = fread(header, 1, sizeof(header), f);
      fclose(f);
      if (got != sizeof(header))
      {
        return false;
      }

      if (header[0] != 'M' || header[1] != 'C')
      {
        return false;
      }

      uint8_t checksum = 0;
      for (size_t i = 0; i < sizeof(header) - 1; i++)
      {
        checksum ^= header[i];
      }
      return checksum == header[sizeof(header) - 1];
    }

    static bool LoadCardImpl(const char *filepath, void *destination, uint32_t settle_ms)
    {
      StorageLock lock;

      char path[CONFIG_FATFS_MAX_LFN + 1];
      strlcpy(path, filepath, sizeof(path));

      uint8_t *dest = destination ? (uint8_t *)destination : sio::memory_card::memory_card_ram;

      ESP_LOGI(kLogPrefix, "LoadCardFromFile(%s)\n", path);

      struct stat st;
      if (stat(path, &st) != 0)
      {
        ESP_LOGE(kLogPrefix, "Error opening file!\n");
        return false;
      }
      if (st.st_size != (off_t)kCardSizeBytes)
      {
        ESP_LOGE(kLogPrefix, "Invalid file size (%ld bytes, expected %d)!\n", (long)st.st_size, (int)kCardSizeBytes);
        return false;
      }

      FILE *f = fopen(path, "rb");
      if (!f)
      {
        ESP_LOGE(kLogPrefix, "Error opening file!\n");
        return false;
      }

      if (!FlushPending())
      {
        ESP_LOGE(kLogPrefix, "Could not save the current card; keeping it mounted.\n");
        fclose(f);
        return false;
      }

      sio::SetMemoryCardEmulation(false);
      if (settle_ms > 0)
      {
        vTaskDelay(MsToTicks(settle_ms));
      }

      size_t got = fread(dest, 1, kCardSizeBytes, f);
      fclose(f);

      if (got != kCardSizeBytes)
      {
        ESP_LOGE(kLogPrefix, "Short read (%u bytes)! Card unloaded.\n", (unsigned)got);
        loaded_file_path[0] = '\0';
        sio::memory_card::committed_to_storage = true;
        return false;
      }

      strlcpy(loaded_file_path, path, sizeof(loaded_file_path));
      MarkAllSectorsSaved();

      sio::memory_card::flag = sio::memory_card::Flags::kDirectoryUnread;
      sio::memory_card::GoIdle();
      sio::memory_card::last_write_tick = sio::event_ticks;
      sio::memory_card::committed_to_storage = true;

      sio::SetMemoryCardEmulation(true);

      return true;
    }

    bool LoadCardFromFile(char *filepath, void *destination)
    {
      return LoadCardImpl(filepath, destination, 250);
    }

    bool CreateBlankFormattedCard(const char *filepath)
    {
      ESP_LOGI(kLogPrefix, "Creating new blank formatted memory card: %s\n", filepath);
      FILE *f = fopen(filepath, "wb");
      if (!f)
      {
        ESP_LOGE(kLogPrefix, "Failed to create new card file: %s\n", filepath);
        return false;
      }

      bool ok = true;
      if (fwrite(ps1_empty_card, 1, sizeof(ps1_empty_card), f) != sizeof(ps1_empty_card))
      {
        ok = false;
      }

      uint8_t buffer[kCardSectorSize];
      memset(buffer, 0xFF, sizeof(buffer));
      for (int i = (int)(sizeof(ps1_empty_card) / kCardSectorSize); ok && i < (int)kCardSectorCount; i++)
      {
        if (fwrite(buffer, 1, sizeof(buffer), f) != sizeof(buffer))
        {
          ok = false;
        }
      }

      if (ok && fflush(f) != 0)
      {
        ok = false;
      }
      if (ok)
      {
        fsync(fileno(f));
      }
      if (fclose(f) != 0)
      {
        ok = false;
      }

      if (!ok)
      {
        ESP_LOGE(kLogPrefix, "Failed to write new card file: %s\n", filepath);
        unlink(filepath);
        return false;
      }

      ESP_LOGI(kLogPrefix, "New formatted card created successfully!\n");
      return true;
    }

    int WriteFile()
    {
      StorageLock lock;

      if (loaded_file_path[0] == '\0')
      {
        ESP_LOGE(kLogPrefix, "No file loaded, abort write\n");
        return -4;
      }

      uint32_t write_start_time = (uint32_t)system::timer::timestamp;

      FILE *f = fopen(loaded_file_path, "r+b");
      if (!f)
      {
        f = fopen(loaded_file_path, "wb");
        if (!f)
        {
          ESP_LOGE(kLogPrefix, "Error opening file %s (errno %d)\n", loaded_file_path, errno);
          return -1;
        }
        for (size_t i = 0; i < kCardSectorCount; i++)
        {
          saved_version[i] = (uint16_t)(sio::memory_card::sector_version[i] - 1);
        }
      }

      int result = 0;
      int sectors_written = 0;
      uint8_t sector_buffer[kCardSectorSize];

      for (size_t s = 0; s < kCardSectorCount; s++)
      {
        uint16_t version = sio::memory_card::sector_version[s];
        if (version == saved_version[s])
        {
          continue;
        }

        for (int attempt = 0; attempt < 4; attempt++)
        {
          version = sio::memory_card::sector_version[s];
          memcpy(sector_buffer, &sio::memory_card::memory_card_ram[s * kCardSectorSize], kCardSectorSize);
          if (version == sio::memory_card::sector_version[s])
          {
            break;
          }
        }

        if (fseek(f, (long)s * kCardSectorSize, SEEK_SET) != 0 ||
            fwrite(sector_buffer, 1, kCardSectorSize, f) != kCardSectorSize)
        {
          ESP_LOGE(kLogPrefix, "Error writing sector %d (errno %d)\n", (int)s, errno);
          result = -2;
          break;
        }

        saved_version[s] = version;
        sectors_written++;
      }

      if (result == 0 && fflush(f) != 0)
      {
        result = -3;
      }
      if (result == 0)
      {
        fsync(fileno(f));
      }
      if (fclose(f) != 0 && result == 0)
      {
        result = -3;
      }

      if (result == 0)
      {
        ESP_LOGI(kLogPrefix, "Saved %d sector(s) to %s in %ims\n", sectors_written, loaded_file_path,
                 (int)((uint32_t)system::timer::timestamp - write_start_time));
      }
      else
      {
        ESP_LOGE(kLogPrefix, "WriteFile failed (%d)\n", result);
      }

      return result;
    }

    bool FlushPending()
    {
      StorageLock lock;

      if (sio::memory_card::committed_to_storage)
      {
        return true;
      }

      if (!ready || loaded_file_path[0] == '\0')
      {
        sio::memory_card::committed_to_storage = true;
        return true;
      }

      sio::memory_card::committed_to_storage = true;
      if (WriteFile() == 0)
      {
        return true;
      }

      sio::memory_card::committed_to_storage = false;
      return false;
    }

    bool MountBootCard(uint32_t settle_ms)
    {
      char boot_path[kCardPathMax];
      BuildBootCardPath(1, boot_path, sizeof(boot_path));

      struct stat st;
      // Um BootCard existente mas invalido nunca e sobrescrito: vai para
      // BootCard-1.mcd.invalid e um novo cartao em branco e criado no lugar.
      if (stat(boot_path, &st) == 0 && !IsValidCardImage(boot_path))
      {
        char invalid_path[kCardPathMax + 16];
        snprintf(invalid_path, sizeof(invalid_path), "%s.invalid", boot_path);
        unlink(invalid_path);
        if (rename(boot_path, invalid_path) != 0)
        {
          ESP_LOGE(kLogPrefix, "MountBootCard: %s is invalid and could not be moved aside (errno %d)\n",
                   boot_path, errno);
          return false;
        }
        ESP_LOGW(kLogPrefix, "MountBootCard: %s is not a valid card image; moved to %s\n",
                 boot_path, invalid_path);
      }

      if (stat(boot_path, &st) == 0 && IsValidCardImage(boot_path))
      {
        return LoadCardImpl(boot_path, sio::memory_card::memory_card_ram, settle_ms);
      }

      char legacy_boot[kCardPathMax];
      snprintf(legacy_boot, sizeof(legacy_boot), "%s/%s/%s/%s.mcd",
               base_path, kCardsRoot, kBootFolder, kBootCardBase);
      if (stat(legacy_boot, &st) == 0 && st.st_size == (off_t)kCardSizeBytes)
      {
        if (IsValidCardImage(legacy_boot))
        {
          return LoadCardImpl(legacy_boot, sio::memory_card::memory_card_ram, settle_ms);
        }
      }

      char boot_dir[kCardPathMax];
      snprintf(boot_dir, sizeof(boot_dir), "%s/%s", kCardsRoot, kBootFolder);
      if (!MkdirP(boot_dir))
      {
        ESP_LOGE(kLogPrefix, "MountBootCard: failed to create BOOT directory\n");
        return false;
      }

      if (!CreateBlankFormattedCard(boot_path))
      {
        ESP_LOGE(kLogPrefix, "MountBootCard: failed to create BootCard-1.mcd\n");
        return false;
      }

      return LoadCardImpl(boot_path, sio::memory_card::memory_card_ram, settle_ms);
    }

    bool MountCardChannel(const char *folder, unsigned channel)
    {
      if (!ready || !folder || !IsValidFolderName(folder))
      {
        return false;
      }

      unsigned max_channels = ReadFolderMaxChannels(folder, folder);
      if (channel < kCardChannelMin || channel > max_channels || channel > kCardChannelMax)
      {
        ESP_LOGW(kLogPrefix, "MountCardChannel: channel %u out of bounds (1..%u)\n", channel, max_channels);
        return false;
      }

      StorageLock lock;

      char target_path[kCardPathMax];
      if (!BuildGameCardPath(folder, channel, ".mcd", target_path, sizeof(target_path)))
      {
        return false;
      }

      struct stat st;
      if (stat(target_path, &st) != 0)
      {
        if (!EnsureGameFolder(folder))
        {
          return false;
        }
        if (!CreateBlankFormattedCard(target_path))
        {
          return false;
        }
      }

      return LoadCardFromFile(target_path, sio::memory_card::memory_card_ram);
    }

    LegacyProbeResult ProbeLegacyLayout(void)
    {
      LegacyProbeResult r;
      struct stat st;
      char path[kCardPathMax];

      snprintf(path, sizeof(path), "%s/games", base_path);
      r.games_dir = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));

      snprintf(path, sizeof(path), "%s/default.mc", base_path);
      r.default_mc = (stat(path, &st) == 0 && S_ISREG(st.st_mode));

      snprintf(path, sizeof(path), "%s/Game2Folder.ini", base_path);
      r.root_game2folder = (stat(path, &st) == 0 && S_ISREG(st.st_mode));

      snprintf(path, sizeof(path), "%s/multidisc.txt", base_path);
      r.multidisc_txt = (stat(path, &st) == 0 && S_ISREG(st.st_mode));

      r.detected = (r.games_dir || r.default_mc || r.root_game2folder || r.multidisc_txt);

      return r;
    }

    bool NormalizeGameID(const char *raw_id, char *out, size_t out_len)
    {
      if (!raw_id || !out || out_len < 16)
      {
        return false;
      }

      const char *p = raw_id;
      while (*p)
      {
        if (isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[1]) &&
            isalpha((unsigned char)p[2]) && isalpha((unsigned char)p[3]))
        {
          char prefix[5];
          prefix[0] = (char)toupper((unsigned char)p[0]);
          prefix[1] = (char)toupper((unsigned char)p[1]);
          prefix[2] = (char)toupper((unsigned char)p[2]);
          prefix[3] = (char)toupper((unsigned char)p[3]);
          prefix[4] = '\0';

          const char *digits = p + 4;
          while (*digits == '_' || *digits == '-' || *digits == ' ')
          {
            digits++;
          }

          char num[16];
          size_t num_len = 0;
          while (*digits && num_len < sizeof(num) - 1)
          {
            if (isdigit((unsigned char)*digits))
            {
              num[num_len++] = *digits;
            }
            else if (*digits != '.')
            {
              break;
            }
            digits++;
          }
          num[num_len] = '\0';

          if (num_len >= 3)
          {
            snprintf(out, out_len, "%s-%s", prefix, num);
            return true;
          }
        }
        p++;
      }
      return false;
    }

    bool CreateGameCardForID(const char *raw_id, unsigned channel,
                             char *out_path, size_t out_path_len,
                             char *out_folder, size_t out_folder_len,
                             bool *out_exists)
    {
      if (!raw_id || !out_path || !out_folder || !out_exists)
      {
        return false;
      }

      *out_exists = false;
      char clean_id[64];
      if (!NormalizeGameID(raw_id, clean_id, sizeof(clean_id)))
      {
        return false;
      }

      char folder[64];
      if (!ResolveCardFolder(clean_id, folder, sizeof(folder), NULL, NULL))
      {
        return false;
      }
      strlcpy(out_folder, folder, out_folder_len);

      unsigned max_channels = ReadFolderMaxChannels(folder, folder);
      if (channel < kCardChannelMin || channel > max_channels || channel > kCardChannelMax)
      {
        return false;
      }

      if (!EnsureGameFolder(folder))
      {
        return false;
      }

      if (!BuildGameCardPath(folder, channel, ".mcd", out_path, out_path_len))
      {
        return false;
      }

      struct stat st;
      if (stat(out_path, &st) == 0 && st.st_size == (off_t)kCardSizeBytes)
      {
        *out_exists = true;
        return true;
      }

      return CreateBlankFormattedCard(out_path);
    }

    bool SwitchToGameID(const char *game_id_str)
    {
      if (!game_id_str || game_id_str[0] == '\0')
      {
        return false;
      }

      if (!ready)
      {
        RememberPendingGameID(game_id_str);
        return false;
      }

      StorageLock lock;

      char clean_id[64];
      if (!NormalizeGameID(game_id_str, clean_id, sizeof(clean_id)))
      {
        ESP_LOGW(kLogPrefix, "SwitchToGameID: Non-standard GameID '%s', ignored.\n", game_id_str);
        return false;
      }

      char folder[64];
      if (!ResolveCardFolder(clean_id, folder, sizeof(folder), NULL, NULL))
      {
        ESP_LOGE(kLogPrefix, "SwitchToGameID: Failed to resolve folder for '%s'\n", clean_id);
        return false;
      }

      char target_path[kCardPathMax];
      if (!FindGameCard(clean_id, folder, 1, target_path, sizeof(target_path)))
      {
        if (!EnsureGameFolder(folder))
        {
          return false;
        }
        if (!BuildGameCardPath(folder, 1, ".mcd", target_path, sizeof(target_path)))
        {
          return false;
        }
        if (!CreateBlankFormattedCard(target_path))
        {
          ESP_LOGE(kLogPrefix, "SwitchToGameID: could not create '%s'\n", target_path);
          return false;
        }
      }

      if (strcasecmp(loaded_file_path, target_path) == 0)
      {
        sio::SetMemoryCardEmulation(false);
        vTaskDelay(pdMS_TO_TICKS(250));
        sio::memory_card::flag = sio::memory_card::Flags::kDirectoryUnread;
        sio::memory_card::GoIdle();
        sio::SetMemoryCardEmulation(true);
        return true;
      }

      return LoadCardFromFile(target_path, sio::memory_card::memory_card_ram);
    }

    bool PlaceUserCard(const char *from_folder, unsigned from_channel,
                       const char *target_folder,
                       char *out_path, size_t out_path_len, unsigned *out_channel)
    {
      if (!ready || !from_folder || !target_folder || !out_path || !out_channel)
      {
        return false;
      }
      if (!IsValidFolderName(from_folder) || !IsValidFolderName(target_folder))
      {
        return false;
      }

      StorageLock lock;

      if (!FlushPending())
      {
        ESP_LOGE(kLogPrefix, "PlaceUserCard: FlushPending failed\n");
        return false;
      }

      char src_path[kCardPathMax];
      if (!BuildGameCardPath(from_folder, from_channel, ".mcd", src_path, sizeof(src_path)))
      {
        return false;
      }

      struct stat st;
      if (stat(src_path, &st) != 0 || st.st_size != (off_t)kCardSizeBytes)
      {
        return false;
      }

      if (!EnsureGameFolder(target_folder))
      {
        return false;
      }

      unsigned max_channels = ReadFolderMaxChannels(target_folder, target_folder);
      unsigned target_ch = 0;
      char dst_path[kCardPathMax];

      for (unsigned ch = kCardChannelMin; ch <= max_channels; ch++)
      {
        if (BuildGameCardPath(target_folder, ch, ".mcd", dst_path, sizeof(dst_path)))
        {
          if (stat(dst_path, &st) != 0)
          {
            target_ch = ch;
            break;
          }
        }
      }

      if (target_ch == 0)
      {
        ESP_LOGE(kLogPrefix, "PlaceUserCard: no free channel in '%s' (max %u)\n", target_folder, max_channels);
        return false;
      }

      char card_name[128];
      bool has_name = ReadFolderChannelName(from_folder, from_folder, from_channel, card_name, sizeof(card_name));

      bool was_mounted = (strcasecmp(loaded_file_path, src_path) == 0);
      if (was_mounted)
      {
        FlushPending();
        sio::SetMemoryCardEmulation(false);
      }

      if (rename(src_path, dst_path) != 0)
      {
        ESP_LOGE(kLogPrefix, "PlaceUserCard: rename failed from '%s' to '%s' (errno %d)\n",
                 src_path, dst_path, errno);
        if (was_mounted)
        {
          sio::SetMemoryCardEmulation(true);
        }
        return false;
      }

      if (has_name)
      {
        WriteFolderChannelName(target_folder, target_folder, target_ch, card_name);
      }
      RemoveFolderChannelName(from_folder, from_folder, from_channel);

      if (was_mounted)
      {
        LoadCardImpl(dst_path, sio::memory_card::memory_card_ram, 250);
      }

      strlcpy(out_path, dst_path, out_path_len);
      *out_channel = target_ch;
      return true;
    }

    void Task_Write(void *params)
    {
      uint8_t write_fail_count = 0;
      uint32_t dirty_since_ms = 0;
      uint32_t last_hb_ms = 0;
      bool led_on = false;

      ESP_LOGI(kLogPrefix, "Entering write task loop\n");
      while (1)
      {
        bool dirty = (sio::memory_card::committed_to_storage == false);

        if (dirty != led_on)
        {
          led_on = dirty;
          gpio_set_level(kPin_LED, dirty ? 1 : 0);
        }

        if (dirty)
        {
          uint32_t now_ms = (uint32_t)system::timer::timestamp;
          if (dirty_since_ms == 0)
          {
            dirty_since_ms = (now_ms != 0) ? now_ms : 1;
          }

          bool quiet = (sio::event_ticks - sio::memory_card::last_write_tick) >= FLUSH_QUIET_TICKS;
          bool too_long = (now_ms - dirty_since_ms) >= FLUSH_MAX_DIRTY_MS;

          if (!ready || loaded_file_path[0] == '\0')
          {
            sio::memory_card::committed_to_storage = true;
            dirty_since_ms = 0;
          }
          else if (quiet || too_long)
          {
            if (FlushPending())
            {
              write_fail_count = 0;
              dirty_since_ms = 0;
            }
            else
            {
              if (write_fail_count < 255)
              {
                write_fail_count++;
              }
              ESP_LOGW(kLogPrefix, "Write failed (attempt %u), retrying in %d ms\n", write_fail_count, FLUSH_RETRY_DELAY_MS);
              vTaskDelay(MsToTicks(FLUSH_RETRY_DELAY_MS));
            }
          }
        }

        if (sio::memory_card::game_id_updated)
        {
          __sync_synchronize();
          sio::memory_card::game_id_updated = false;

          char gid[64];
          strlcpy(gid, (const char *)sio::memory_card::game_id, sizeof(gid));
          ESP_LOGI(kLogPrefix, "PicoStation GameID detected: %s\n", gid);
          SwitchToGameID(gid);
        }

        while (sio::memory_card::trace_read_idx != sio::memory_card::trace_write_idx)
        {
          uint32_t idx = sio::memory_card::trace_read_idx & 31;
          sio::memory_card::McTraceEntry e = sio::memory_card::trace_ring[idx];
          sio::memory_card::trace_read_idx++;
          if (e.cmd != 0x20)
          {
            ESP_LOGI(kLogPrefix, "[MC_TRACE] tick=%u addr=0x%02X cmd=0x%02X flag=0x%02X ticks=%u sec=0x%03X chk=%02X/%02X st=0x%02X abt=%u pads=%u b0abt=%u glt=%u\n",
                     e.event_tick, e.addr, e.cmd, e.flag_sent, e.ticks, e.sector, e.chk_in, e.chk_out, e.status, e.abort_reason,
                     (uint32_t)sio::memory_card::pad_poll_count, (uint32_t)sio::memory_card::abort_byte0_count,
                     (uint32_t)spi::sel_glitch_count);
          }
        }

        uint32_t hb_now_ms = (uint32_t)system::timer::timestamp;
        if (hb_now_ms - last_hb_ms >= 5000)
        {
          last_hb_ms = hb_now_ms;
          ESP_LOGI(kLogPrefix, "[MC_HB] ev_ticks=%u pads=%u b0abt=%u glt=%u flag=0x%02X\n",
                   (uint32_t)sio::event_ticks, (uint32_t)sio::memory_card::pad_poll_count,
                   (uint32_t)sio::memory_card::abort_byte0_count, (uint32_t)spi::sel_glitch_count,
                   sio::memory_card::flag);
        }

        vTaskDelay(MsToTicks(10));
      }

      vTaskDelete(NULL);
    }

    bool UnloadCard()
    {
      StorageLock lock;

      ESP_LOGI(kLogPrefix, "UnloadCard: disabling MC emulation and clearing slot.\n");
      if (!FlushPending())
      {
        ESP_LOGE(kLogPrefix, "UnloadCard: could not save pending changes, card kept mounted.\n");
        return false;
      }
      sio::SetMemoryCardEmulation(false);
      loaded_file_path[0] = '\0';
      return true;
    }

    bool SafelyUnmountSD()
    {
      StorageLock lock;

      ESP_LOGI(kLogPrefix, "SafelyUnmountSD: committing saves and unmounting FATFS...\n");
      if (!FlushPending())
      {
        ESP_LOGE(kLogPrefix, "SafelyUnmountSD: could not save pending changes, SD kept mounted.\n");
        return false;
      }
      sio::SetMemoryCardEmulation(false);

      if (ready)
      {
        if (sdcard::unmount_sdcard() != ESP_OK)
        {
          ESP_LOGE(kLogPrefix, "SafelyUnmountSD: unmount failed, card kept mounted.\n");
          sio::SetMemoryCardEmulation(true);
          return false;
        }
        ready = false;
      }

      loaded_file_path[0] = '\0';
      ESP_LOGI(kLogPrefix, "SafelyUnmountSD complete: SD is safe to remove.\n");
      return true;
    }

    bool RemountSD()
    {
      StorageLock lock;

      ESP_LOGI(kLogPrefix, "RemountSD: attempting to remount MicroSD card...\n");
      if (ready)
      {
        return true;
      }

      if (sdcard::Init() == ESP_OK)
      {
        strcpy(base_path, "/sdcard");
        ready = true;

        if (!MountBootCard(250))
        {
          ESP_LOGW(kLogPrefix, "RemountSD: SD mounted, but no BootCard could be loaded.\n");
        }
        ApplyPendingGameID();
        ESP_LOGI(kLogPrefix, "RemountSD: successfully remounted MicroSD card!\n");
        return true;
      }
      else
      {
        ESP_LOGE(kLogPrefix, "RemountSD: failed to remount MicroSD card.\n");
        return false;
      }
    }

    void Init()
    {
      if (storage_mutex == NULL)
      {
        storage_mutex = xSemaphoreCreateRecursiveMutex();
      }

      loaded_file_path[0] = '\0';

      if (sdcard::Init() == ESP_OK)
      {
        strcpy(base_path, "/sdcard");
        ready = true;

        LegacyProbeResult probe = ProbeLegacyLayout();
        if (probe.Any())
        {
          ESP_LOGW(kLogPrefix, "legacy layout detected (games=%d, default=%d, g2f=%d, multidisc=%d); run tools/migrate_sd.py on a PC\n",
                   probe.games_dir, probe.default_mc, probe.root_game2folder, probe.multidisc_txt);
        }

        EnsureCardsRoot();

        if (!MountBootCard(0))
        {
          ESP_LOGE(kLogPrefix, "Could not mount BootCard: memory card emulation disabled.\n");
          loaded_file_path[0] = '\0';
          sio::SetMemoryCardEmulation(false);
        }

        ApplyPendingGameID();
      }
      else
      {
        ESP_LOGE(kLogPrefix, "SD card not available: memory card emulation disabled until it is mounted.\n");
        sio::SetMemoryCardEmulation(false);
      }
    }
  } // namespace storage
} // namespace openmc
