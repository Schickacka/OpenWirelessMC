#include "config.h"

#include "logging.h"
#include "storage/storage.h"
#include "system/settings.h"
#include "wifi/wifi.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

#ifndef CONFIG_FILE_PATH
#define CONFIG_FILE_PATH "/sdcard/config.txt"
#endif

#define MIN_SSID_LENGTH (1)
#define MAX_SSID_LENGTH (32)
#define MIN_PASS_LENGTH (8) // WPA2 requires at least 8 characters
#define MAX_PASS_LENGTH (63)
#define MAX_LINE_LENGTH (256)

namespace openmc
{
    namespace storage
    {
        namespace config
        {
            static const char *kNvsNamespace = "owmc";
            static ConfigSource g_source = kSourceNone;

            // Removes trailing CR/LF only (SSIDs and passwords may legitimately contain spaces).
            static void StripLineEnding(char *s)
            {
                size_t len = strlen(s);
                while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n'))
                {
                    s[--len] = '\0';
                }
            }

            // Removes leading/trailing whitespace (used for setting names and the mode value).
            static char *Trim(char *s)
            {
                while (*s != '\0' && isspace((unsigned char)*s))
                {
                    s++;
                }
                size_t len = strlen(s);
                while (len > 0 && isspace((unsigned char)s[len - 1]))
                {
                    s[--len] = '\0';
                }
                return s;
            }

            static void ToLower(char *s)
            {
                for (; *s != '\0'; s++)
                {
                    *s = (char)tolower((unsigned char)*s);
                }
            }

            static bool ParseMode(const char *value, wifi::Mode *out)
            {
                if (strcmp(value, "client") == 0)
                {
                    *out = wifi::Mode::kClient;
                    return true;
                }
                if (strcmp(value, "ap") == 0)
                {
                    *out = wifi::Mode::kAcessPoint;
                    return true;
                }
                if (strcmp(value, "none") == 0)
                {
                    *out = wifi::Mode::kNone;
                    return true;
                }
                return false;
            }

            static const char *ModeToString(wifi::Mode mode)
            {
                switch (mode)
                {
                case wifi::Mode::kClient:
                    return "client";
                case wifi::Mode::kAcessPoint:
                    return "ap";
                default:
                    return "none";
                }
            }

            static bool NvsReady()
            {
                esp_err_t ret = nvs_flash_init();
                if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
                {
                    nvs_flash_erase();
                    ret = nvs_flash_init();
                }
                return ret == ESP_OK;
            }

            static bool NvsGetString(nvs_handle_t handle, const char *key, char *out, size_t out_size)
            {
                size_t len = out_size;
                if (out_size == 0 || nvs_get_str(handle, key, out, &len) != ESP_OK)
                {
                    return false;
                }
                out[out_size - 1] = '\0';
                return true;
            }

            // Copia os valores para a NVS. Nao valida: quem chama ja validou.
            static bool StoreToNvs(const system::Settings &settings)
            {
                if (!NvsReady())
                {
                    ESP_LOGE(kLogPrefix, "config: NVS unavailable");
                    return false;
                }

                nvs_handle_t handle;
                if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK)
                {
                    ESP_LOGE(kLogPrefix, "config: nvs_open failed");
                    return false;
                }

                bool ok = true;
                if (nvs_set_str(handle, "mode", ModeToString(settings.wifi_mode)) != ESP_OK ||
                    nvs_set_str(handle, "ssid", (const char *)settings.ssid) != ESP_OK ||
                    nvs_set_str(handle, "pass", (const char *)settings.password) != ESP_OK)
                {
                    ok = false;
                }
                if (ok && nvs_commit(handle) != ESP_OK)
                {
                    ok = false;
                }
                nvs_close(handle);

                if (!ok)
                {
                    ESP_LOGE(kLogPrefix, "config: failed to persist settings to NVS");
                }
                return ok;
            }

            static bool LoadFromNvs(system::Settings *out)
            {
                if (!NvsReady())
                {
                    return false;
                }

                nvs_handle_t handle;
                if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK)
                {
                    return false;
                }

                char mode_value[16] = {0};
                char ssid_value[MAX_SSID_LENGTH + 1] = {0};
                char pass_value[MAX_PASS_LENGTH + 1] = {0};

                bool found = NvsGetString(handle, "mode", mode_value, sizeof(mode_value)) &&
                             NvsGetString(handle, "ssid", ssid_value, sizeof(ssid_value)) &&
                             NvsGetString(handle, "pass", pass_value, sizeof(pass_value));
                nvs_close(handle);

                if (!found)
                {
                    return false;
                }

                ToLower(mode_value);
                wifi::Mode mode;
                if (!ParseMode(mode_value, &mode))
                {
                    return false;
                }

                size_t ssid_len = strlen(ssid_value);
                size_t pass_len = strlen(pass_value);
                if (mode != wifi::Mode::kNone)
                {
                    if (ssid_len < MIN_SSID_LENGTH || ssid_len > MAX_SSID_LENGTH)
                    {
                        return false;
                    }
                    if (pass_len != 0 && (pass_len < MIN_PASS_LENGTH || pass_len > MAX_PASS_LENGTH))
                    {
                        return false;
                    }
                }

                memset(out, 0, sizeof(*out));
                out->wifi_mode = mode;
                memcpy(out->ssid, ssid_value, ssid_len + 1);
                memcpy(out->password, pass_value, pass_len + 1);
                return true;
            }

            // Parser do /sdcard/config.txt (mesmas regras de sempre).
            static bool ParseConfigFile(system::Settings *out)
            {
                FILE *config_file = fopen(CONFIG_FILE_PATH, "r");
                if (config_file == NULL)
                {
                    return false;
                }

                char line[MAX_LINE_LENGTH];

                system::Settings temp_settings;
                memset(&temp_settings, 0, sizeof(temp_settings));

                bool found_wifi_mode = false;
                bool found_ssid = false;
                bool found_password = false;
                bool first_line = true;

                while (fgets(line, sizeof(line), config_file))
                {
                    size_t len = strlen(line);

                    // Over-long line: discard the rest of it instead of parsing a fragment
                    if (len == sizeof(line) - 1 && line[len - 1] != '\n')
                    {
                        int c;
                        while ((c = fgetc(config_file)) != '\n' && c != EOF)
                        {
                        }
                        first_line = false;
                        continue;
                    }

                    char *text = line;

                    // Skip UTF-8 BOM written by some Windows editors
                    if (first_line && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
                    {
                        text += 3;
                    }
                    first_line = false;

                    while (*text == ' ' || *text == '\t')
                    {
                        text++;
                    }

                    if (*text == '#' || *text == '\r' || *text == '\n' || *text == '\0')
                    {
                        continue;
                    }

                    char *delimiter = strchr(text, '=');
                    if (delimiter == NULL)
                    {
                        continue;
                    }

                    *delimiter = '\0';
                    char *name = Trim(text);
                    ToLower(name);
                    char *value = delimiter + 1;
                    StripLineEnding(value);

                    if (strcmp(name, "mode") == 0)
                    {
                        char *mode = Trim(value);
                        ToLower(mode);
                        if (ParseMode(mode, &temp_settings.wifi_mode))
                        {
                            found_wifi_mode = true;
                        }
                        else
                        {
                            ESP_LOGW(kLogPrefix, "config.txt: unknown mode '%s' (use ap, client or none)", mode);
                        }
                    }
                    else if (strcmp(name, "ssid") == 0)
                    {
                        size_t ssid_len = strlen(value);
                        if (ssid_len >= MIN_SSID_LENGTH && ssid_len <= MAX_SSID_LENGTH)
                        {
                            memcpy(temp_settings.ssid, value, ssid_len + 1);
                            found_ssid = true;
                        }
                        else
                        {
                            ESP_LOGW(kLogPrefix, "config.txt: ssid must have %d-%d characters", MIN_SSID_LENGTH, MAX_SSID_LENGTH);
                        }
                    }
                    else if (strcmp(name, "password") == 0)
                    {
                        // 0 length = open network
                        size_t pass_len = strlen(value);
                        if (pass_len == 0 || (pass_len >= MIN_PASS_LENGTH && pass_len <= MAX_PASS_LENGTH))
                        {
                            memcpy(temp_settings.password, value, pass_len + 1);
                            found_password = true;
                        }
                        else
                        {
                            ESP_LOGW(kLogPrefix, "config.txt: password must be empty or have %d-%d characters", MIN_PASS_LENGTH, MAX_PASS_LENGTH);
                        }
                    }
                }

                fclose(config_file);

                if ((found_wifi_mode && found_ssid && found_password) || (found_wifi_mode && temp_settings.wifi_mode == wifi::Mode::kNone))
                {
                    memset(out, 0, sizeof(*out));
                    *out = temp_settings;
                    return true;
                }

                return false;
            }

            // Depois de importar, o .txt sai da raiz do cartao: fica em
            // .sd2psx/config.txt.imported (nada e apagado).
            static void MoveConfigFileAside()
            {
                char dir[kCardPathMax];
                snprintf(dir, sizeof(dir), "%s/%s", base_path, kGame2FolderDir);
                mkdir(dir, 0777);

                char imported[kCardPathMax + 32];
                snprintf(imported, sizeof(imported), "%s/config.txt.imported", dir);

                if (rename(CONFIG_FILE_PATH, imported) == 0)
                {
                    ESP_LOGI(kLogPrefix, "config.txt imported into NVS; moved to %s", imported);
                }
                else
                {
                    ESP_LOGW(kLogPrefix, "config.txt imported into NVS, but could not be moved (errno %d)", errno);
                }
            }

            bool LoadConfig()
            {
                system::Settings loaded;

                if (LoadFromNvs(&loaded))
                {
                    system::settings = loaded;
                    system::settings.contrast = 0xFF; // Contrast not implemented to config yet
                    g_source = kSourceNvs;
                    return true;
                }

                if (!ParseConfigFile(&loaded))
                {
                    g_source = kSourceNone;
                    return false;
                }

                system::settings = loaded;
                system::settings.contrast = 0xFF; // Contrast not implemented to config yet
                g_source = kSourceFile;

                // Primeiro boot com layout antigo: persiste na NVS e tira o .txt
                // da raiz. Se a NVS falhar, o arquivo fica onde esta.
                if (StoreToNvs(system::settings))
                {
                    MoveConfigFileAside();
                }

                return true;
            }

            bool SaveConfig(const char *mode, const char *ssid, const char *password)
            {
                if (!mode || !ssid || !password)
                {
                    return false;
                }

                char mode_value[16];
                strlcpy(mode_value, mode, sizeof(mode_value));
                ToLower(mode_value);

                system::Settings temp;
                memset(&temp, 0, sizeof(temp));

                if (!ParseMode(mode_value, &temp.wifi_mode))
                {
                    return false;
                }

                size_t ssid_len = strlen(ssid);
                size_t pass_len = strlen(password);
                if (temp.wifi_mode != wifi::Mode::kNone)
                {
                    if (ssid_len < MIN_SSID_LENGTH || ssid_len > MAX_SSID_LENGTH)
                    {
                        return false;
                    }
                    if (pass_len != 0 && (pass_len < MIN_PASS_LENGTH || pass_len > MAX_PASS_LENGTH))
                    {
                        return false;
                    }
                }
                else if (ssid_len > MAX_SSID_LENGTH || pass_len > MAX_PASS_LENGTH)
                {
                    return false;
                }

                memcpy(temp.ssid, ssid, ssid_len + 1);
                memcpy(temp.password, password, pass_len + 1);

                if (!StoreToNvs(temp))
                {
                    return false;
                }

                temp.contrast = 0xFF;
                system::settings = temp;
                g_source = kSourceNvs;
                return true;
            }

            ConfigSource Source()
            {
                return g_source;
            }

            const char *SourceName()
            {
                switch (g_source)
                {
                case kSourceNvs:
                    return "nvs";
                case kSourceFile:
                    return "file";
                default:
                    return "default";
                }
            }
        } // namespace config
    } // namespace storage
} // namespace openmc
