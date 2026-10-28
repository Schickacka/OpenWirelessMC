#ifndef _STORAGE_H
#define _STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define MAX_WRITE_FAILURES 5

namespace openmc
{
  namespace storage
  {
    static const size_t kCardSizeBytes = 128 * 1024;
    static const size_t kCardSectorCount = 1024;
    static const size_t kCardSectorSize = 128;
    static const size_t kCardPathMax = CONFIG_FATFS_MAX_LFN + 16;
    static const unsigned kCardChannelMin = 1;
    static const unsigned kCardChannelMax = 254;
    static const unsigned kDefaultMaxChannels = 8;
    static const size_t kMaxFolderNameLen = 31;

    extern const char kCardsRoot[];
    extern const char kBootFolder[];
    extern const char kBootCardBase[];
    extern const char kGame2FolderDir[];
    extern const char kGame2FolderFile[];
    extern const char kUserCardsFolder[];

    extern FILE *mc_file;
    extern int rw_fail_count;
    extern bool ready;
    extern char loaded_file_path[];
    extern char base_path[];
    extern SemaphoreHandle_t storage_mutex;

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

    struct LegacyProbeResult
    {
      bool games_dir;
      bool default_mc;
      bool root_game2folder;
      bool multidisc_txt;
      bool detected;
      bool Any() const { return detected || games_dir || default_mc || root_game2folder || multidisc_txt; }
    };

    void Init();
    void Task_Write(void *params);
    bool LoadCardFromFile(char *filepath, void *destination);
    int WriteFile();
    bool FlushPending();
    bool SwitchToGameID(const char *game_id_str);
    bool CreateBlankFormattedCard(const char *filepath);
    bool UnloadCard();
    bool SafelyUnmountSD();
    bool RemountSD();

    bool EnsureDirectoryPublic(const char *rel_path);
    bool IsValidFolderName(const char *name);
    bool NormalizeGameID(const char *raw_id, char *out, size_t out_len);
    bool ResolveGameFolder(const char *normalized_id, char *out_folder, size_t out_len,
                           bool *out_has_parent, bool *out_has_override);
    bool CreateGameCardForID(const char *raw_id, unsigned channel,
                             char *out_path, size_t out_path_len,
                             char *out_folder, size_t out_folder_len,
                             bool *out_exists);
    bool MountCardChannel(const char *folder, unsigned channel);
    bool MountBootCard(uint32_t settle_ms);
    bool PlaceUserCard(const char *from_folder, unsigned from_channel,
                       const char *target_folder,
                       char *out_path, size_t out_path_len, unsigned *out_channel);
    LegacyProbeResult ProbeLegacyLayout(void);
    unsigned ReadFolderMaxChannels(const char *folder, const char *base_name);
    bool ReadFolderChannelName(const char *folder, const char *base_name,
                               unsigned channel, char *out, size_t out_len);
    bool WriteFolderChannelName(const char *folder, const char *base_name,
                                unsigned channel, const char *name);
    bool RemoveFolderChannelName(const char *folder, const char *base_name,
                                 unsigned channel);
    bool ReadCardFolderOverride(const char *parent_id, char *out, size_t out_len);
  } // namespace storage
} // namespace openmc

#endif // _STORAGE_H
