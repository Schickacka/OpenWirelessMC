#include "storage/storage.h"

#include "playstation/sio.h"
#include "playstation/sio_memory_card.h"
#include "storage/sdcard.h"
#include "system/timer.h"

#include "logging.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
          sio::memory_card::game_id_updated = false;
          if (sio::memory_card::committed_to_storage == false)
          {
            WriteFile();
            sio::memory_card::committed_to_storage = true;
          }
          ESP_LOGI(kLogPrefix, "PicoStation GameID detected: %s\n", (char *)sio::memory_card::game_id);
          SwitchToGameID((char *)sio::memory_card::game_id);
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
      sprintf(loaded_file_path, "%s/default.mc", base_path);
      if (!LoadCardFromFile(loaded_file_path, sio::memory_card::memory_card_ram))
      {
        sio::memory_card::committed_to_storage = false;
      }
    }

    bool LoadCardFromFile(char *filepath, void *destination)
    {
      FILE *mc_file;
      bool old_mc_status = sio::memory_card_enabled;
      sio::memory_card_enabled = false;

      ESP_LOGI(kLogPrefix, "LoadCardFromFile(%s)\n", filepath);
      mc_file = fopen(filepath, "r");
      uint8_t *dest = (uint8_t *)destination;

      long file_size;

      if (!mc_file)
      {
        ESP_LOGE(kLogPrefix, "Error opening file!\n");
        sio::memory_card_enabled = old_mc_status;
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
          sio::memory_card_enabled = old_mc_status;
          return false;
        }

        rewind(mc_file);

        if (file_size != (128 * 1024))
        {
          ESP_LOGE(kLogPrefix, "Invalid File size!\n");
          fclose(mc_file);
          sio::memory_card_enabled = old_mc_status;
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

      // Make sure MC stays inactive long enough for BIOS to detect change
      // To-do: Adjust lower and test
      // Bios takes roughly 1051ms between MC status polls.
      // Can probably get away with less, but go a bit longer.
      // We need to miss at least one status check to initiate a proper refresh in bios.
      vTaskDelay(1337 / portTICK_PERIOD_MS);
      sio::memory_card_enabled = old_mc_status;

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

      uint8_t buffer[128];

      // Frame 0: Header frame
      memset(buffer, 0x00, 128);
      buffer[0] = 'M'; // 0x4D
      buffer[1] = 'C'; // 0x43
      buffer[127] = 0x0E; // Checksum: 'M' ^ 'C'
      fwrite(buffer, 1, 128, f);

      // Frames 1 to 15: Directory entries for the 15 user blocks
      for (int i = 1; i <= 15; i++)
      {
        memset(buffer, 0x00, 128);
        buffer[0] = 0xA0; // 0xA0 = Free block
        buffer[8] = 0xFF; // Next block = none (end of file)
        buffer[9] = 0xFF;
        buffer[127] = 0xA0; // Checksum: 0xA0 ^ 0xFF ^ 0xFF = 0xA0
        fwrite(buffer, 1, 128, f);
      }

      // Frames 16 to 35: Broken block list (20 frames)
      memset(buffer, 0xFF, 128);
      for (int i = 16; i <= 35; i++)
      {
        fwrite(buffer, 1, 128, f);
      }

      // Frames 36 to 63: Unused (reserved, 28 frames)
      memset(buffer, 0xFF, 128);
      for (int i = 36; i <= 63; i++)
      {
        fwrite(buffer, 1, 128, f);
      }

      // Frames 64 to 1023: User data blocks (15 blocks * 64 frames = 960 frames)
      memset(buffer, 0x00, 128);
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

      char target_path[CONFIG_FATFS_MAX_LFN + 1];
      snprintf(target_path, sizeof(target_path), "%s/%s.mc", base_path, game_id_str);

      if (strcmp(loaded_file_path, target_path) == 0)
      {
        ESP_LOGI(kLogPrefix, "GameID %s is already loaded.\n", game_id_str);
        return true;
      }

      FILE *check = fopen(target_path, "rb");
      if (!check)
      {
        CreateBlankFormattedCard(target_path);
      }
      else
      {
        fclose(check);
      }

      return LoadCardFromFile(target_path, sio::memory_card::memory_card_ram);
    }
  } // storage
} // openmc
