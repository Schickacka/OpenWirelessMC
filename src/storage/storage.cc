#include "storage/storage.h"
#include "storage/ps1_empty_card.h"

#include "playstation/sio.h"
#include "playstation/sio_memory_card.h"
#include "playstation/spi.h"
#include "pins.h"
#include "storage/sdcard.h"
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
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#define CARD_SIZE_BYTES (128 * 1024)
#define CARD_SECTOR_COUNT 1024
#define CARD_SECTOR_SIZE 128

// A save is flushed to the SD card once the console has been quiet for this many SIO transactions
// (~60 per second on NTSC, ~50 on PAL), or after kMaxDirtyMs even if traffic never stops.
#define FLUSH_QUIET_TICKS 15
#define FLUSH_MAX_DIRTY_MS 1000
#define FLUSH_RETRY_DELAY_MS 3000

// If default.mc is not a valid PS1 card image (wrong size, missing "MC"
// signature or bad sector-0 checksum), move it aside and create a fresh one.
// The 1.3 release shipped an all-zero default.mc; without this the console
// showed "Card is not Formatted" even though the README promised a ready
// "Boot Card". See CORRECOES-v1.4.md (C3).
#ifndef MC_AUTO_REPAIR_DEFAULT_CARD
#define MC_AUTO_REPAIR_DEFAULT_CARD 1
#endif

namespace openmc
{
  namespace storage
  {
    char loaded_file_path[CONFIG_FATFS_MAX_LFN + 1];
    char base_path[16];
    bool ready = false;

    // Last sector_version[] value that is known to be on the SD card (storage side only).
    static uint16_t saved_version[CARD_SECTOR_COUNT];

    // The web server, the write task and the GameID switch all touch the card file and the RAM image.
    static SemaphoreHandle_t storage_mutex = NULL;

    class StorageLock
    {
    public:
      StorageLock()
      {
        if (storage_mutex)
        {
          xSemaphoreTakeRecursive(storage_mutex, portMAX_DELAY);
        }
      }
      ~StorageLock()
      {
        if (storage_mutex)
        {
          xSemaphoreGiveRecursive(storage_mutex);
        }
      }
    };

    static void MarkAllSectorsSaved()
    {
      for (int i = 0; i < CARD_SECTOR_COUNT; i++)
      {
        saved_version[i] = sio::memory_card::sector_version[i];
      }
    }

    static TickType_t MsToTicks(uint32_t ms)
    {
      TickType_t ticks = pdMS_TO_TICKS(ms);
      return ticks > 0 ? ticks : 1; // never 0: vTaskDelay(0) would busy-loop and starve the idle task
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

        // LED = "card image has changes that are not on the SD card yet"
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

          // Quiet = the console stopped writing. event_ticks counts SIO transactions (pad polls included).
          bool quiet = (sio::event_ticks - sio::memory_card::last_write_tick) >= FLUSH_QUIET_TICKS;
          bool too_long = (now_ms - dirty_since_ms) >= FLUSH_MAX_DIRTY_MS;

          if (!ready || loaded_file_path[0] == '\0')
          {
            // Nowhere to save to (SD ejected or card unloaded): do not retry forever
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
              // Keep the data (and the dirty flag): it is saved as soon as the SD card works again
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

      // Block 0 (8192 bytes = 64 frames): Sony's template (from sd2psX)
      if (fwrite(ps1_empty_card, 1, sizeof(ps1_empty_card), f) != sizeof(ps1_empty_card))
      {
        ok = false;
      }

      // Blocks 1 to 15 (960 frames) filled with 0xFF
      uint8_t buffer[CARD_SECTOR_SIZE];
      memset(buffer, 0xFF, sizeof(buffer));
      for (int i = (int)(sizeof(ps1_empty_card) / CARD_SECTOR_SIZE); ok && i < CARD_SECTOR_COUNT; i++)
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
        ESP_LOGE(kLogPrefix, "Failed to write new card file (SD full or error): %s\n", filepath);
        unlink(filepath); // never leave a truncated card behind
        return false;
      }

      ESP_LOGI(kLogPrefix, "New formatted card created successfully!\n");
      return true;
    }

    // Writes only the sectors the console changed since the last save, in place.
    // Compared with rewriting the whole 128 KB file (which truncated it first):
    //  - a power cut mid-write can only affect the sectors being written, not the whole card;
    //  - it takes a few milliseconds instead of hundreds;
    //  - a sector the console writes while we are flushing is never lost (version counters).
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
        // File was deleted/renamed behind our back: recreate it with the whole image
        f = fopen(loaded_file_path, "wb");
        if (!f)
        {
          ESP_LOGE(kLogPrefix, "Error opening file %s (errno %d)\n", loaded_file_path, errno);
          return -1;
        }
        for (int i = 0; i < CARD_SECTOR_COUNT; i++)
        {
          saved_version[i] = (uint16_t)(sio::memory_card::sector_version[i] - 1);
        }
      }

      int result = 0;
      int sectors_written = 0;
      uint8_t sector_buffer[CARD_SECTOR_SIZE];

      for (int s = 0; s < CARD_SECTOR_COUNT; s++)
      {
        uint16_t version = sio::memory_card::sector_version[s];
        if (version == saved_version[s])
        {
          continue;
        }

        // Snapshot the sector; retry if the SIO core commits it while we copy
        for (int attempt = 0; attempt < 4; attempt++)
        {
          version = sio::memory_card::sector_version[s];
          memcpy(sector_buffer, &sio::memory_card::memory_card_ram[s * CARD_SECTOR_SIZE], CARD_SECTOR_SIZE);
          if (version == sio::memory_card::sector_version[s])
          {
            break;
          }
        }

        if (fseek(f, (long)s * CARD_SECTOR_SIZE, SEEK_SET) != 0 ||
            fwrite(sector_buffer, 1, CARD_SECTOR_SIZE, f) != CARD_SECTOR_SIZE)
        {
          ESP_LOGE(kLogPrefix, "Error writing sector %d (errno %d)\n", s, errno);
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
        // Whatever was not confirmed on disk stays dirty (saved_version only advances on success),
        // but we do not know the state of the sector that failed.
        ESP_LOGE(kLogPrefix, "WriteFile failed (%d)\n", result);
      }

      return result;
    }

    // Saves pending changes. Returns true when there is nothing left to save.
    bool FlushPending()
    {
      StorageLock lock;

      if (sio::memory_card::committed_to_storage)
      {
        return true;
      }

      if (!ready || loaded_file_path[0] == '\0')
      {
        // Nowhere to save to
        sio::memory_card::committed_to_storage = true;
        return true;
      }

      // Clear the flag BEFORE writing. If the console writes again meanwhile, Commit() raises it again and
      // the next pass saves that sector. (Clearing it after the write used to lose such updates.)
      sio::memory_card::committed_to_storage = true;
      if (WriteFile() == 0)
      {
        return true;
      }

      sio::memory_card::committed_to_storage = false;
      return false;
    }

    // Loads a 128 KB card image into RAM and presents it to the console.
    // A bad file never disturbs the card that is currently mounted.
    static bool LoadCardImpl(const char *filepath, void *destination, uint32_t settle_ms)
    {
      StorageLock lock;

      // filepath may alias loaded_file_path
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
      if (st.st_size != CARD_SIZE_BYTES)
      {
        ESP_LOGE(kLogPrefix, "Invalid file size (%ld bytes, expected %d)!\n", (long)st.st_size, CARD_SIZE_BYTES);
        return false;
      }

      FILE *f = fopen(path, "rb");
      if (!f)
      {
        ESP_LOGE(kLogPrefix, "Error opening file!\n");
        return false;
      }

      // Never drop unsaved changes of the card being replaced
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

      size_t got = fread(dest, 1, CARD_SIZE_BYTES, f);
      fclose(f);

      if (got != CARD_SIZE_BYTES)
      {
        // RAM now holds a mix of two cards. Do not present it and make sure it can never be written back.
        ESP_LOGE(kLogPrefix, "Short read (%u bytes)! Card unloaded.\n", (unsigned)got);
        loaded_file_path[0] = '\0';
        sio::memory_card::committed_to_storage = true;
        return false; // emulation stays disabled
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

    // A PS1 card image is only usable if it has the right size, the "MC"
    // signature and a valid sector-0 checksum (XOR of bytes 0x00..0x7E stored
    // at 0x7F). The old all-zero default.mc failed this test.
    static bool IsValidCardImage(const char *path)
    {
      struct stat st;
      if (stat(path, &st) != 0 || st.st_size != CARD_SIZE_BYTES)
      {
        return false;
      }

      FILE *f = fopen(path, "rb");
      if (!f)
      {
        return false;
      }

      uint8_t header[CARD_SECTOR_SIZE];
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

    // Mounts <SD>/default.mc, creating a blank formatted card if it does not exist yet.
    static bool MountDefaultCard(uint32_t settle_ms)
    {
      char target[CONFIG_FATFS_MAX_LFN + 1];
      snprintf(target, sizeof(target), "%s/default.mc", base_path);

      struct stat st;
      if (stat(target, &st) == 0)
      {
#if MC_AUTO_REPAIR_DEFAULT_CARD
        if (!IsValidCardImage(target))
        {
          // Never overwrite silently: keep the bad file for inspection.
          char backup[CONFIG_FATFS_MAX_LFN + 16];
          snprintf(backup, sizeof(backup), "%s.invalid", target);
          unlink(backup);
          if (rename(target, backup) != 0)
          {
            ESP_LOGE(kLogPrefix, "default.mc is invalid and could not be moved aside (errno %d); refusing to overwrite it.\n", errno);
            return false;
          }
          ESP_LOGW(kLogPrefix, "default.mc was not a valid PS1 card image (size/signature/checksum); moved to %s and creating a fresh Boot Card.\n", backup);
        }
#endif
      }

      if (stat(target, &st) != 0)
      {
        ESP_LOGW(kLogPrefix, "default.mc not found, creating a blank formatted card.\n");
        if (!CreateBlankFormattedCard(target))
        {
          return false;
        }
      }

      if (!IsValidCardImage(target))
      {
        ESP_LOGE(kLogPrefix, "default.mc is still invalid after creation; leaving memory card emulation disabled.\n");
        return false;
      }

      return LoadCardImpl(target, sio::memory_card::memory_card_ram, settle_ms);
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
        strcpy(storage::base_path, "/sdcard");
        storage::ready = true;

        // SIO is not running yet at this point, so there is no need to wait for the console
        if (!MountDefaultCard(0))
        {
          // Better "no card" than a wrong or damaged one; default.mc is left untouched
          ESP_LOGE(kLogPrefix, "Could not mount default.mc: memory card emulation disabled.\n");
          loaded_file_path[0] = '\0';
          sio::SetMemoryCardEmulation(false);
        }
      }
      else
      {
        ESP_LOGE(kLogPrefix, "SD card not available: memory card emulation disabled until it is mounted.\n");
        sio::SetMemoryCardEmulation(false);
      }
    }

    static bool ResolveParentGameID(const char *child_id, char *out_parent, size_t out_len)
    {
      if (!child_id || child_id[0] == '\0' || !out_parent || out_len == 0)
      {
        return false;
      }

      // Check /sdcard/multidisc.txt if present
      char multidisc_path[CONFIG_FATFS_MAX_LFN + 1];
      snprintf(multidisc_path, sizeof(multidisc_path), "%s/multidisc.txt", base_path);

      FILE *f = fopen(multidisc_path, "r");
      if (f)
      {
        char line[64];
        const size_t child_len = strlen(child_id);
        while (fgets(line, sizeof(line), f))
        {
          if (line[0] == '#' || line[0] == '\r' || line[0] == '\n')
          {
            continue;
          }
          if (strncasecmp(line, child_id, child_len) == 0 && line[child_len] == '=')
          {
            const char *val = line + child_len + 1;
            size_t val_len = strcspn(val, "\r\n ");
            if (val_len > 0 && val_len < out_len)
            {
              memcpy(out_parent, val, val_len);
              out_parent[val_len] = '\0';
              fclose(f);
              return true;
            }
          }
        }
        fclose(f);
      }

      return false;
    }

    bool SwitchToGameID(const char *game_id_str)
    {
      if (!game_id_str || strlen(game_id_str) == 0 || !ready)
      {
        return false;
      }

      StorageLock lock;

      ESP_LOGI(kLogPrefix, "SwitchToGameID: Raw GameID received: '%s'\n", game_id_str);

      // Extract Game ID from paths like "cdrom:\SLUS_010.66;1", "SLUS_010.66", "SLUS-01066", etc.
      // Search for standard 4-letter PSX prefixes (SLUS, SCUS, SLES, SCES, SLPS, SLPM, SCPS, etc.)
      const char *p = game_id_str;
      char clean_id[64];
      clean_id[0] = '\0';

      while (*p)
      {
        // Check if current 4 characters are an alphabetic code
        if (isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[1]) &&
            isalpha((unsigned char)p[2]) && isalpha((unsigned char)p[3]))
        {
          char prefix[5];
          prefix[0] = toupper((unsigned char)p[0]);
          prefix[1] = toupper((unsigned char)p[1]);
          prefix[2] = toupper((unsigned char)p[2]);
          prefix[3] = toupper((unsigned char)p[3]);
          prefix[4] = '\0';

          const char *digits_ptr = p + 4;
          // Skip optional separators like '_', '-', or ' '
          while (*digits_ptr == '_' || *digits_ptr == '-' || *digits_ptr == ' ')
          {
            digits_ptr++;
          }

          // Extract digits (handling dots, e.g. "010.66" -> "01066")
          char num_buf[16];
          size_t num_len = 0;
          while (*digits_ptr && num_len < sizeof(num_buf) - 1)
          {
            if (isdigit((unsigned char)*digits_ptr))
            {
              num_buf[num_len++] = *digits_ptr;
            }
            else if (*digits_ptr != '.')
            {
              // Hit ';' or space or another separator indicating end of number
              break;
            }
            digits_ptr++;
          }
          num_buf[num_len] = '\0';

          if (num_len >= 3)
          {
            // Found a valid GameID! Format as PREFIX-NUMBER (e.g. SLUS-01066)
            snprintf(clean_id, sizeof(clean_id), "%s-%s", prefix, num_buf);
            break;
          }
        }
        p++;
      }

      if (clean_id[0] == '\0')
      {
        ESP_LOGW(kLogPrefix, "SwitchToGameID: Non-standard or unparseable GameID '%s', ignoring card switch request.\n", game_id_str);
        return false;
      }

      ESP_LOGI(kLogPrefix, "SwitchToGameID: Parsed GameID -> '%s'\n", clean_id);

      // Search candidates in order of priority:
      // 1. <GameID>-1.mc (PSxMemCard Gen2 standard channel)
      // 2. <GameID>.mc
      // 3. <GameID>-1.mcd
      // 4. <GameID>.mcd
      // 5. <GameID>-1.mcr
      // 6. <GameID>.mcr
      const char *suffixes[] = {
        "-1.mc",
        ".mc",
        "-1.mcd",
        ".mcd",
        "-1.mcr",
        ".mcr"
      };

      char found_path[CONFIG_FATFS_MAX_LFN + 1];
      bool file_exists = false;

      // 1. Check if a dedicated save file for this specific disc already exists on the SD
      for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
      {
        snprintf(found_path, sizeof(found_path), "%s/%s%s", base_path, clean_id, suffixes[i]);
        FILE *f = fopen(found_path, "rb");
        if (f)
        {
          fclose(f);
          file_exists = true;
          ESP_LOGI(kLogPrefix, "SwitchToGameID: Found existing save file '%s'!\n", found_path);
          break;
        }
      }

      // 2. If no dedicated save file exists, check multi-disc parent mapping (e.g. SCUS-94164 -> SCUS-94163)
      if (!file_exists)
      {
        char parent_id[64];
        if (ResolveParentGameID(clean_id, parent_id, sizeof(parent_id)))
        {
          ESP_LOGI(kLogPrefix, "SwitchToGameID: Multi-disc child ID '%s' resolving to parent '%s'...\n", clean_id, parent_id);
          for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
          {
            snprintf(found_path, sizeof(found_path), "%s/%s%s", base_path, parent_id, suffixes[i]);
            FILE *f = fopen(found_path, "rb");
            if (f)
            {
              fclose(f);
              file_exists = true;
              ESP_LOGI(kLogPrefix, "SwitchToGameID: Found existing parent save file '%s'!\n", found_path);
              strlcpy(clean_id, parent_id, sizeof(clean_id));
              break;
            }
          }
          if (!file_exists)
          {
            // If neither exists, use parent ID so all continuation discs share the newly created card
            strlcpy(clean_id, parent_id, sizeof(clean_id));
          }
        }
      }

      // If no candidate exists, create default <GameID>-1.mc
      if (!file_exists)
      {
        snprintf(found_path, sizeof(found_path), "%s/%s-1.mc", base_path, clean_id);
        ESP_LOGI(kLogPrefix, "SwitchToGameID: No existing save file found. Creating '%s'...\n", found_path);
        if (!CreateBlankFormattedCard(found_path))
        {
          ESP_LOGE(kLogPrefix, "SwitchToGameID: could not create '%s'; keeping the current card.\n", found_path);
          return false;
        }
      }

      if (strcasecmp(loaded_file_path, found_path) == 0)
      {
        ESP_LOGI(kLogPrefix, "GameID save %s is already loaded. Cycling card connection for ODE/Game detection.\n", found_path);
        sio::SetMemoryCardEmulation(false);
        vTaskDelay(250 / portTICK_PERIOD_MS);
        sio::memory_card::flag = sio::memory_card::Flags::kDirectoryUnread;
        sio::memory_card::GoIdle();
        sio::SetMemoryCardEmulation(true);
        return true;
      }

      return LoadCardFromFile(found_path, sio::memory_card::memory_card_ram);
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
      // Stop new console writes first, then unmount. If the unmount fails we
      // re-enable emulation so the still-mounted card keeps working.
      sio::SetMemoryCardEmulation(false);

      if (storage::ready)
      {
        if (sdcard::unmount_sdcard() != ESP_OK)
        {
          ESP_LOGE(kLogPrefix, "SafelyUnmountSD: unmount failed, card kept mounted.\n");
          sio::SetMemoryCardEmulation(true);
          return false;
        }
        storage::ready = false;
      }

      loaded_file_path[0] = '\0';
      ESP_LOGI(kLogPrefix, "SafelyUnmountSD complete: SD is safe to remove.\n");
      return true;
    }

    bool RemountSD()
    {
      StorageLock lock;

      ESP_LOGI(kLogPrefix, "RemountSD: attempting to remount MicroSD card...\n");
      if (storage::ready)
      {
        return true;
      }

      if (sdcard::Init() == ESP_OK)
      {
        strcpy(storage::base_path, "/sdcard");
        storage::ready = true;

        if (!MountDefaultCard(250))
        {
          ESP_LOGW(kLogPrefix, "RemountSD: SD mounted, but no card could be loaded from default.mc.\n");
        }
        ESP_LOGI(kLogPrefix, "RemountSD: successfully remounted MicroSD card!\n");
        return true;
      }
      else
      {
        ESP_LOGE(kLogPrefix, "RemountSD: failed to remount MicroSD card.\n");
        return false;
      }
    }
  } // storage
} // openmc
