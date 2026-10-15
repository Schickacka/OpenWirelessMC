#include "config.h"

#include "logging.h"
#include "system/settings.h"
#include "wifi/wifi.h"

#include <esp_log.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

            bool LoadConfig()
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
                        if (strcmp(mode, "client") == 0)
                        {
                            temp_settings.wifi_mode = wifi::Mode::kClient;
                            found_wifi_mode = true;
                        }
                        else if (strcmp(mode, "ap") == 0)
                        {
                            temp_settings.wifi_mode = wifi::Mode::kAcessPoint;
                            found_wifi_mode = true;
                        }
                        else if (strcmp(mode, "none") == 0)
                        {
                            temp_settings.wifi_mode = wifi::Mode::kNone;
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
                    // Valid configuration file, copy settings
                    system::settings = temp_settings;
                    system::settings.contrast = 0xFF; // Contrast not implemented to config yet
                    return true;
                }

                return false;
            }
        }
    }
}
