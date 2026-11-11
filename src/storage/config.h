#ifndef _CONFIG_H
#define _CONFIG_H

#include <stdbool.h>

namespace openmc
{
    namespace storage
    {
        namespace config
        {
            enum ConfigSource
            {
                kSourceNone = 0,
                kSourceNvs = 1,
                kSourceFile = 2
            };

            // Le a configuracao: NVS primeiro; se a NVS estiver vazia, importa
            // /sdcard/config.txt (uma vez) e move o arquivo para .sd2psx/.
            bool LoadConfig();

            // Grava mode/ssid/password na NVS. mode: "client", "ap" ou "none".
            bool SaveConfig(const char *mode, const char *ssid, const char *password);

            ConfigSource Source();
            const char *SourceName();
        } // namespace config
    } // namespace storage
} // namespace openmc

#endif // _CONFIG_H
