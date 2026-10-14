#include "storage/storage.h"
#include "storage/ps1_empty_card.h"

#include "playstation/sio.h"
#include "playstation/sio_memory_card.h"
#include "playstation/spi.h"
#include "storage/sdcard.h"
#include "system/timer.h"

#include "logging.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

namespace openmc
{
  namespace storage
  {
    char loaded_file_path[CONFIG_FATFS_MAX_LFN + 1];
    char base_path[16];
    bool ready = false;

    void Task_Write(void *params)
    {
      uint8_t write_fail_count = 0;
      uint32_t wait_start_time = 0;

      ESP_LOGI(kLogPrefix, "Entering write task loop\n");
      while (1)
      {
        while (sio::memory_card::committed_to_storage == false)
        {
          if (write_fail_count < MAX_WRITE_FAILURES)
          {
            if (write_fail_count == 0)
            {
              // Using this to watch for total inactivity of SIO traffic
              wait_start_time = system::timer::timestamp;
            }

            // if wait period in ticks or milliseconds reached, proceed with write
            // Roughly 60ish event ticks a second on an NTSC system
            // So in theory this should wait 2 seconds after a write is finished
            if ((sio::event_ticks - sio::memory_card::last_write_tick >= 120) || system::timer::timestamp - wait_start_time >= 2000)
            {
              //ESP_LOGI(kLogPrefix, "event_ticks - last_write_tick = %lli\n", (sio::event_ticks - sio::memory_card::last_write_tick));
              //ESP_LOGI(kLogPrefix, "%lli - %i = %lli", system::timer::timestamp, wait_start_time, system::timer::timestamp - wait_start_time);
              if (WriteFile() != 0)
              {
                write_fail_count++;
                ESP_LOGW(kLogPrefix, "Write fail count %i, delay 1 second\n", write_fail_count);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
              }
              else
              {
                sio::memory_card::committed_to_storage = true;
                write_fail_count = 0;
                wait_start_time = 0;
              }
            }
            else
            {
              vTaskDelay(portTICK_PERIOD_MS);
            }
          }
          else
          {
            // To-do: Implement plan b here.
            // For now, toggle this just so we don't infinite loop
            sio::memory_card::committed_to_storage = true;
            write_fail_count = 0;
            wait_start_time = 0;
            ESP_LOGE(kLogPrefix, "Bailed on write!\n");
            //break; // break from inner loop
          }
        }
        if (sio::memory_card::game_id_updated)
        {
          __sync_synchronize();
          sio::memory_card::game_id_updated = false;
          if (sio::memory_card::committed_to_storage == false)
          {
            WriteFile();
            sio::memory_card::committed_to_storage = true;
          }
          ESP_LOGI(kLogPrefix, "PicoStation GameID detected: %s\n", (char *)sio::memory_card::game_id);
          SwitchToGameID((char *)sio::memory_card::game_id);
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

        static uint32_t last_hb_ms = 0;
        if (system::timer::timestamp - last_hb_ms >= 5000)
        {
          last_hb_ms = system::timer::timestamp;
          ESP_LOGI(kLogPrefix, "[MC_HB] ev_ticks=%u pads=%u b0abt=%u glt=%u flag=0x%02X\n",
                   (uint32_t)sio::event_ticks, (uint32_t)sio::memory_card::pad_poll_count,
                   (uint32_t)sio::memory_card::abort_byte0_count, (uint32_t)spi::sel_glitch_count,
                   sio::memory_card::flag);
        }

        vTaskDelay(portTICK_PERIOD_MS);
      }

      vTaskDelete(NULL);
    }

    void Init()
    {
      if (sdcard::Init() == ESP_OK)
      {
        strcpy(storage::base_path, "/sdcard");
        // Mount successful
        storage::ready = true;
      }
      /*
      // To-do: Figure out how ram filesystem should work
      // File only exists in memory, but should be able to
      // download/upload directly to ram from web ui(overwrite),
      // save, delete, an downlaod.
      else
      {
        // Fallback option if all else fails
        // MC Image only persists in memory
        strcpy(base_path, "/ramfs");
      }*/

      // Load default.mc, create file if it doesn't exist.
      snprintf(loaded_file_path, sizeof(loaded_file_path), "%s/default.mc", base_path);
      if (!LoadCardFromFile(loaded_file_path, sio::memory_card::memory_card_ram))
      {
        sio::memory_card::committed_to_storage = false;
      }
    }

    bool LoadCardFromFile(char *filepath, void *destination)
    {
      FILE *mc_file;
      sio::SetMemoryCardEmulation(false);
      vTaskDelay(250 / portTICK_PERIOD_MS);

      ESP_LOGI(kLogPrefix, "LoadCardFromFile(%s)\n", filepath);
      mc_file = fopen(filepath, "r");
      uint8_t *dest = (uint8_t *)destination;

      long file_size;

      if (!mc_file)
      {
        ESP_LOGE(kLogPrefix, "Error opening file!\n");
        return false;
      }
      else
      {
        fseek(mc_file, 0, SEEK_END);
        file_size = ftell(mc_file);
        if (file_size <= 0)
        {
          ESP_LOGE(kLogPrefix, "File is empty\n");
          fclose(mc_file);
          return false;
        }

        rewind(mc_file);

        if (file_size != (128 * 1024))
        {
          ESP_LOGE(kLogPrefix, "Invalid File size!\n");
          fclose(mc_file);
          return false;
        }

        fread(dest, 1, file_size, mc_file);
        fclose(mc_file);

        strlcpy(loaded_file_path, filepath, sizeof(loaded_file_path));

        sio::memory_card::flag = sio::memory_card::Flags::kDirectoryUnread;
        sio::memory_card::GoIdle();
        sio::memory_card::last_write_tick = sio::event_ticks;
        sio::memory_card::committed_to_storage = true;
      }

      sio::SetMemoryCardEmulation(true);

      return true;
    }

    int WriteFile()
    {
      int result;
      FILE *mc_file;
      uint32_t write_end_time;
      uint32_t write_start_time = system::timer::timestamp;

      if (strcmp(loaded_file_path, "") == 0)
      {
        ESP_LOGE(kLogPrefix, "No file loaded, abort write\n");
        return -4;
      }

      ESP_LOGI(kLogPrefix, "WriteFile(%s)\n", loaded_file_path);
      mc_file = fopen(loaded_file_path, "w");

      if (!mc_file)
      {
        ESP_LOGE(kLogPrefix, "Error opening file!\n");
        result = -1;
      }
      else
      {
        if (fwrite(sio::memory_card::memory_card_ram, 1, (128 * 1024), mc_file) != (128 * 1024))
        {
          ESP_LOGE(kLogPrefix, "Error writing file!\n");
          result = -2;
        }
        else if (ferror(mc_file) != 0)
        {
          ESP_LOGE(kLogPrefix, "Unknown error writing file!\n");
          result = -3;
        }
        else
        {
          write_end_time = system::timer::timestamp;
          ESP_LOGI(kLogPrefix, "File written in %ims\n", write_end_time - write_start_time);
          result = 0;
        }

        fclose(mc_file);
      }

      return result;
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

      // Write Block 0 (8192 bytes = 64 frames) using the authentic Sony template (from sd2psX)
      fwrite(ps1_empty_card, 1, sizeof(ps1_empty_card), f);

      // Write Blocks 1 to 15 (15 * 64 frames = 960 frames) filled with 0xFF
      uint8_t buffer[128];
      memset(buffer, 0xFF, 128);
      for (int i = 64; i < 1024; i++)
      {
        fwrite(buffer, 1, 128, f);
      }

      fclose(f);
      ESP_LOGI(kLogPrefix, "New formatted card created successfully!\n");
      return true;
    }

    bool SwitchToGameID(const char *game_id_str)
    {
      if (!game_id_str || strlen(game_id_str) == 0)
      {
        return false;
      }

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

      // Fallback: If no standard PSX pattern found, sanitize characters directly
      if (clean_id[0] == '\0')
      {
        size_t len = 0;
        for (size_t i = 0; game_id_str[i] != '\0' && len < sizeof(clean_id) - 1; i++)
        {
          char c = game_id_str[i];
          if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
          {
            clean_id[len++] = toupper((unsigned char)c);
          }
        }
        clean_id[len] = '\0';
      }

      if (strlen(clean_id) == 0)
      {
        ESP_LOGW(kLogPrefix, "SwitchToGameID: Invalid/empty sanitized GameID.\n");
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

      // If no candidate exists, create default <GameID>-1.mc
      if (!file_exists)
      {
        snprintf(found_path, sizeof(found_path), "%s/%s-1.mc", base_path, clean_id);
        ESP_LOGI(kLogPrefix, "SwitchToGameID: No existing save file found. Creating '%s'...\n", found_path);
        CreateBlankFormattedCard(found_path);
      }

      if (strcmp(loaded_file_path, found_path) == 0)
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
      ESP_LOGI(kLogPrefix, "UnloadCard: disabling MC emulation and clearing slot.\n");
      // Flush any pending write if not yet committed
      if (sio::memory_card::committed_to_storage == false && strlen(loaded_file_path) > 0)
      {
        WriteFile();
        sio::memory_card::committed_to_storage = true;
      }
      sio::SetMemoryCardEmulation(false);
      loaded_file_path[0] = '\0';
      return true;
    }

    bool SafelyUnmountSD()
    {
      ESP_LOGI(kLogPrefix, "SafelyUnmountSD: committing saves and unmounting FATFS...\n");
      // Flush any pending save before unmounting
      if (sio::memory_card::committed_to_storage == false && strlen(loaded_file_path) > 0)
      {
        WriteFile();
        sio::memory_card::committed_to_storage = true;
      }
      sio::SetMemoryCardEmulation(false);
      loaded_file_path[0] = '\0';

      if (storage::ready)
      {
        sdcard::unmount_sdcard();
        storage::ready = false;
      }
      ESP_LOGI(kLogPrefix, "SafelyUnmountSD complete: SD is safe to remove.\n");
      return true;
    }

    bool RemountSD()
    {
      ESP_LOGI(kLogPrefix, "RemountSD: attempting to remount MicroSD card...\n");
      if (storage::ready)
      {
        return true;
      }

      if (sdcard::Init() == ESP_OK)
      {
        strcpy(storage::base_path, "/sdcard");
        storage::ready = true;

        // Try mounting default.mc
        char target[CONFIG_FATFS_MAX_LFN + 1];
        snprintf(target, sizeof(target), "%s/default.mc", storage::base_path);
        LoadCardFromFile(target, sio::memory_card::memory_card_ram);
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
