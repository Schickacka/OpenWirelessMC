/* HTTP File Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "web/file_server.h"

#include "logging.h"
#include "gui/gui.h"
#include "playstation/sio.h"
#include "playstation/sio_memory_card.h"
#include "storage/storage.h"
#include "storage/game_lookup.h"
#include "web/percent_decode.h"

#include <cctype>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "storage/config.h"
#include "system/settings.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_vfs.h>
#include <esp_vfs_fat.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_http_server.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <soc/rtc.h>
#include <esp_ota_ops.h>
#include "ff.h"

/* Max length a file path can have on storage */
#define FILE_PATH_MAX CONFIG_FATFS_MAX_LFN

/* Max size of an individual file. Make sure this
 * value is same as that set in upload_script.html */
#define MAX_FILE_SIZE (8 * 1024 * 1024) // 8 MB
#define MAX_FILE_SIZE_STR "8MB"

/* Scratch buffer size */
#define SCRATCH_BUFSIZE 8192

// Safe suffix check (the old macro read before the start of the string for names shorter than the extension)
static inline bool has_file_ext(const char *name, const char *ext)
{
    size_t name_len = strlen(name);
    size_t ext_len = strlen(ext);
    return name_len >= ext_len && strcasecmp(name + name_len - ext_len, ext) == 0;
}
#define IS_FILE_EXT(filename, ext) has_file_ext((filename), (ext))

#ifndef ENABLE_OLED
#define ENABLE_OLED 0
#endif

namespace openmc
{
    namespace web
    {
        namespace file_server
        {
            struct file_server_data
            {
                // Base path of file storage
                char base_path[FILE_PATH_MAX + 1];

                // Scratch buffer for temporary storage during file transfer
                char scratch[SCRATCH_BUFSIZE];
            };

            // Handler to redirect incoming GET request for /index.html to
            // This can be overridden by uploading file with same name
            static esp_err_t index_html_get_handler(httpd_req_t *req)
            {
                httpd_resp_set_status(req, "307 Temporary Redirect");
                httpd_resp_set_hdr(req, "Location", "/");
                httpd_resp_send(req, NULL, 0); // Response body can be empty
                return ESP_OK;
            }

            // Handler to respond with an icon file embedded in flash.
            // Browsers expect to GET website icon at URI /favicon.ico.
            // This can be overridden by uploading file with same name
            static esp_err_t favicon_get_handler(httpd_req_t *req)
            {
                extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
                extern const unsigned char favicon_ico_end[] asm("_binary_favicon_ico_end");
                const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
                httpd_resp_set_type(req, "image/x-icon");
                httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
                return ESP_OK;
            }

            // ---- Output helpers -------------------------------------------------------------------
            // File names come from the SD card (FAT allows spaces, '&', '#', '%', '+', apostrophes...),
            // so they must never be pasted raw into HTML or into URLs.
            // NOTE: httpd_resp_send_chunk() with length 0 ENDS the response, so never send empty chunks.

            static void send_html_escaped(httpd_req_t *req, const char *text)
            {
                char out[96];
                size_t used = 0;

                for (const char *p = text; *p != '\0'; p++)
                {
                    const char *esc = NULL;
                    switch (*p)
                    {
                    case '&':
                        esc = "&amp;";
                        break;
                    case '<':
                        esc = "&lt;";
                        break;
                    case '>':
                        esc = "&gt;";
                        break;
                    case '"':
                        esc = "&quot;";
                        break;
                    case '\'':
                        esc = "&#39;";
                        break;
                    default:
                        break;
                    }

                    size_t need = esc ? strlen(esc) : 1;
                    if (used + need > sizeof(out))
                    {
                        httpd_resp_send_chunk(req, out, used);
                        used = 0;
                    }
                    if (esc)
                    {
                        memcpy(out + used, esc, need);
                    }
                    else
                    {
                        out[used] = *p;
                    }
                    used += need;
                }

                if (used > 0)
                {
                    httpd_resp_send_chunk(req, out, used);
                }
            }

            // Percent-encodes everything except unreserved characters (and '/' when keep_slash is set).
            // The result is safe inside an HTML attribute and inside a single-quoted JavaScript string.
            static void send_url_encoded(httpd_req_t *req, const char *text, bool keep_slash)
            {
                static const char hex[] = "0123456789ABCDEF";
                char out[96];
                size_t used = 0;

                for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++)
                {
                    bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                                 *p == '-' || *p == '_' || *p == '.' || *p == '~' || (keep_slash && *p == '/');
                    size_t need = plain ? 1 : 3;
                    if (used + need > sizeof(out))
                    {
                        httpd_resp_send_chunk(req, out, used);
                        used = 0;
                    }
                    if (plain)
                    {
                        out[used++] = (char)*p;
                    }
                    else
                    {
                        out[used++] = '%';
                        out[used++] = hex[*p >> 4];
                        out[used++] = hex[*p & 0x0F];
                    }
                }

                if (used > 0)
                {
                    httpd_resp_send_chunk(req, out, used);
                }
            }

            // Send HTTP response with a run-time generated html consisting of
            // a list of all files and folders under the requested path.
            // In case of SPIFFS this returns empty list when path is any
            // string other than '/', since SPIFFS doesn't support directories
            static esp_err_t http_resp_dir_html(httpd_req_t *req, const char *dirpath)
            {
                char entrypath[FILE_PATH_MAX];
                char temp_dirpath[FILE_PATH_MAX];
                char entrysize[16];
                const char *entrytype;

                struct dirent *entry;
                struct stat entry_stat;

                DIR *dir;
                size_t dirpath_len;

                // Retrieve the base path of file storage to construct the full path
                strlcpy(entrypath, dirpath, sizeof(entrypath));
                strlcpy(temp_dirpath, entrypath, sizeof(temp_dirpath));

                // Remove trailing slash or directory will fail to open
                if (temp_dirpath[strlen(temp_dirpath) - 1] == '/')
                {
                    temp_dirpath[strlen(temp_dirpath) - 1] = '\0';
                }

                dir = opendir(temp_dirpath);

                dirpath_len = strlen(entrypath);

                // URL of the folder being listed (decoded path without the mount point, e.g. "/" or "/folder/")
                const size_t base_len = strlen(storage::base_path);
                const char *url_prefix = (base_len > 0 && strncmp(dirpath, storage::base_path, base_len) == 0 && dirpath[base_len] == '/')
                                             ? dirpath + base_len
                                             : "/";

                // Get handle to embedded file upload script (Head, CSS, Header, Navigation Tabs, Upload Card)
                extern const unsigned char upload_script_start[] asm("_binary_upload_script_html_start");
                extern const unsigned char upload_script_end[] asm("_binary_upload_script_html_end");
                const size_t upload_script_size = (upload_script_end - upload_script_start);
                httpd_resp_send_chunk(req, (const char *)upload_script_start, upload_script_size);

                // Send loaded file badge & current path navigation
                httpd_resp_sendstr_chunk(req, "<div class=\"loaded-card-bar\"><div><div class=\"loaded-label\" data-i18n=\"active_mc_label\">");
                if (strlen(storage::loaded_file_path) > 0) {
                    httpd_resp_sendstr_chunk(req, "Memory Card Ativo no Console</div><div class=\"loaded-filename\">🎮 ");
                    send_html_escaped(req, storage::loaded_file_path);
                    httpd_resp_sendstr_chunk(req, "</div></div><div><button type=\"button\" class=\"btn btn-red\" onclick=\"ejectCard()\" style=\"padding:8px 14px; font-size:12px; display:inline-flex; align-items:center; gap:6px;\"><span style=\"font-size:14px;\">⏏</span> <span data-i18n=\"btn_eject\">Desmontar (Liberar Slot)</span></button></div>");
                } else {
                    httpd_resp_sendstr_chunk(req, "Status do Slot no Console</div><div class=\"loaded-filename\"><span style=\"color:var(--text-muted); font-style:italic;\" data-i18n=\"empty_slot_desc\">📭 Nenhum Save Montado (Slot Liberado para Cartão Físico)</span></div></div>");
                }
                httpd_resp_sendstr_chunk(req, "</div>");

                char ps1_root[FILE_PATH_MAX];
                snprintf(ps1_root, sizeof(ps1_root), "%s/MemoryCards/PS1", storage::base_path);

                const char *display_path = (strncmp(temp_dirpath, storage::base_path, base_len) == 0 && temp_dirpath[base_len] == '/')
                                               ? temp_dirpath + base_len + 1
                                               : temp_dirpath;

                httpd_resp_sendstr_chunk(req, "<div class=\"path-nav\"><span>Pasta Atual: <b>");
                send_html_escaped(req, display_path);
                httpd_resp_sendstr_chunk(req, "</b></span>");

                httpd_resp_sendstr_chunk(req, "<div style=\"display:flex; align-items:center; gap:8px; flex-wrap:wrap;\">");

                bool is_base = (strcmp(temp_dirpath, storage::base_path) == 0 ||
                                strcmp(temp_dirpath, ps1_root) == 0 ||
                                (strncmp(temp_dirpath, storage::base_path, base_len) == 0 &&
                                 temp_dirpath[base_len] == '/' && temp_dirpath[base_len + 1] == '\0'));
                if (!is_base) {
                    httpd_resp_sendstr_chunk(req, "<a href=\"../\" class=\"btn btn-grey\" style=\"padding:5px 11px; font-size:12px; display:inline-flex; align-items:center; gap:5px; text-decoration:none;\"><span>⬆️</span> <span>Subir de Pasta (Parent)</span></a>");
                }

                if (is_base) {
                    httpd_resp_sendstr_chunk(req, "<button type=\"button\" id=\"btn-filter-folders\" class=\"btn btn-grey\" onclick=\"toggleFolderFilter()\" style=\"padding:5px 11px; font-size:12px; display:inline-flex; align-items:center; gap:5px; cursor:pointer;\"><span>📂</span> <span id=\"lbl-filter-folders\">Filtro: Todas as Pastas</span></button>");
                }

                httpd_resp_sendstr_chunk(req, "</div></div>");

                httpd_resp_sendstr_chunk(req, "<div class=\"table-wrapper\"><table id=\"saves-table\" class=\"ps-table\"><thead><tr>"
                                              "<th style=\"cursor:pointer; user-select:none;\" onclick=\"sortTable(0)\" title=\"Clique para ordenar\">Título <span id=\"sort-icon-0\" style=\"color:var(--ps-blue);\">▲</span></th>"
                                              "<th style=\"cursor:pointer; user-select:none;\" onclick=\"sortTable(1)\" title=\"Clique para ordenar\">Nome do Arquivo <span id=\"sort-icon-1\" style=\"color:var(--text-muted);\">↕</span></th>"
                                              "<th style=\"cursor:pointer; user-select:none;\" onclick=\"sortTable(2)\" title=\"Clique para ordenar\">Tipo <span id=\"sort-icon-2\" style=\"color:var(--text-muted);\">↕</span></th>"
                                              "<th style=\"cursor:pointer; user-select:none;\" onclick=\"sortTable(3)\" title=\"Clique para ordenar\">Tamanho <span id=\"sort-icon-3\" style=\"color:var(--text-muted);\">↕</span></th>"
                                              "<th>Ações</th></tr></thead><tbody>");

                if (dir != NULL)
                {
                    /* Iterate over all files / folders and fetch their names and sizes */
                    while ((entry = readdir(dir)) != NULL)
                    {
#if defined(_WIN32)
                        strlcpy(entrypath + dirpath_len, entry->d_name, sizeof(entrypath) - dirpath_len);
                        struct stat ent_check;
                        bool is_dir = (stat(entrypath, &ent_check) == 0 && S_ISDIR(ent_check.st_mode));
#else
                        bool is_dir = (entry->d_type == DT_DIR);
#endif
                        // Ignore Windows system folder, internal config.txt, and titles database
                        if ((strcmp(entry->d_name, "System Volume Information") == 0 && is_dir) ||
                            (strcasecmp(entry->d_name, "config.txt") == 0) ||
                            (strcasecmp(entry->d_name, "titles.db") == 0))
                        {
                            continue;
                        }

                        if (is_dir)
                        {
                            entrytype = "Directory";
                        }
                        else
                        {
                            if (IS_FILE_EXT(entry->d_name, ".mc") || IS_FILE_EXT(entry->d_name, ".mcd") || IS_FILE_EXT(entry->d_name, ".mcr"))
                            {
                                entrytype = "Memory Card Image";
                            }
                            else
                            {
                                entrytype = "File";
                            }
                        }

                        strlcpy(entrypath + dirpath_len, entry->d_name, sizeof(entrypath) - dirpath_len);
                        if (stat(entrypath, &entry_stat) == -1)
                        {
                            ESP_LOGE(kLogPrefix, "Failed to stat %s : %s", entrytype, entry->d_name);
                            continue;
                        }
                        sprintf(entrysize, "%ld", entry_stat.st_size);

                        bool is_mc = (!is_dir && (IS_FILE_EXT(entry->d_name, ".mc") || IS_FILE_EXT(entry->d_name, ".mcd") || IS_FILE_EXT(entry->d_name, ".mcr")));

                        bool is_mounted = (strlen(storage::loaded_file_path) > 0 && strcmp(entrypath, storage::loaded_file_path) == 0);

                        if (is_dir)
                        {
                            unsigned mc_count = 0;
                            DIR *subdir = opendir(entrypath);
                            if (subdir)
                            {
                                struct dirent *subentry;
                                while ((subentry = readdir(subdir)) != NULL)
                                {
                                    if (IS_FILE_EXT(subentry->d_name, ".mc") || IS_FILE_EXT(subentry->d_name, ".mcd") || IS_FILE_EXT(subentry->d_name, ".mcr"))
                                    {
                                        mc_count++;
                                    }
                                }
                                closedir(subdir);
                            }

                            bool has_mc = (mc_count > 0);

                            if (has_mc)
                            {
                                httpd_resp_sendstr_chunk(req, "<tr data-type=\"folder\" data-empty=\"false\"><td class=\"game-col\" data-name=\"");
                            }
                            else
                            {
                                httpd_resp_sendstr_chunk(req, "<tr data-type=\"folder\" data-empty=\"true\"><td class=\"game-col\" data-name=\"");
                            }
                            send_html_escaped(req, entry->d_name);
                            httpd_resp_sendstr_chunk(req, "\"><span class=\"game-title-text\">-</span></td><td><a href=\"");
                            send_url_encoded(req, url_prefix, true);
                            send_url_encoded(req, entry->d_name, false);
                            httpd_resp_sendstr_chunk(req, "/\">📁 ");
                            send_html_escaped(req, entry->d_name);
                            httpd_resp_sendstr_chunk(req, "/</a></td><td>");

                            if (has_mc)
                            {
                                httpd_resp_sendstr_chunk(req, "<span class=\"tag tag-dir\">Pasta</span>");
                            }
                            else
                            {
                                httpd_resp_sendstr_chunk(req, "<span class=\"tag tag-empty\" style=\"background:rgba(255,149,0,0.15); color:#ff9f0a; border:1px solid rgba(255,149,0,0.3); font-size:11px; padding:2px 8px; border-radius:4px;\">Vazia</span>");
                            }

                            httpd_resp_sendstr_chunk(req, "</td><td>");
                            char count_buf[128];
                            if (mc_count == 0)
                            {
                                snprintf(count_buf, sizeof(count_buf), "<span style=\"color:var(--text-muted); font-size:12px;\">0 cartões</span>");
                            }
                            else if (mc_count == 1)
                            {
                                snprintf(count_buf, sizeof(count_buf), "<span style=\"color:var(--text-muted); font-size:12px;\">1 cartão</span>");
                            }
                            else
                            {
                                snprintf(count_buf, sizeof(count_buf), "<span style=\"color:var(--text-muted); font-size:12px;\">%u cartões</span>", mc_count);
                            }
                            httpd_resp_sendstr_chunk(req, count_buf);

                            httpd_resp_sendstr_chunk(req, "</td><td><div class=\"actions-cell\"><a href=\"");
                            send_url_encoded(req, url_prefix, true);
                            send_url_encoded(req, entry->d_name, false);
                            httpd_resp_sendstr_chunk(req, "/\" class=\"btn btn-green\">Abrir 📂</a>");

                            if (!has_mc)
                            {
                                httpd_resp_sendstr_chunk(req, "<button type=\"button\" class=\"btn btn-red\" onclick=\"deleteEmptyFolder('");
                                send_url_encoded(req, url_prefix, true);
                                send_url_encoded(req, entry->d_name, false);
                                httpd_resp_sendstr_chunk(req, "', '");
                                send_html_escaped(req, entry->d_name);
                                httpd_resp_sendstr_chunk(req, "')\">● Excluir</button>");
                            }

                            httpd_resp_sendstr_chunk(req, "</div></td></tr>\n");
                        }
                        else
                        {
                            httpd_resp_sendstr_chunk(req, "<tr><td class=\"game-col\" data-name=\"");
                            send_html_escaped(req, entry->d_name);
                            httpd_resp_sendstr_chunk(req, "\"><span class=\"game-title-text\">-</span></td><td><a href=\"");
                            send_url_encoded(req, url_prefix, true);
                            send_url_encoded(req, entry->d_name, false);
                            httpd_resp_sendstr_chunk(req, "\">");
                            if (is_mc) {
                                httpd_resp_sendstr_chunk(req, "🎮 ");
                            } else {
                                httpd_resp_sendstr_chunk(req, "📄 ");
                            }
                            send_html_escaped(req, entry->d_name);
                            httpd_resp_sendstr_chunk(req, "</a></td><td>");
                            if (is_mc) {
                                if (entry_stat.st_size > (128 * 1024)) {
                                    httpd_resp_sendstr_chunk(req, "<span class=\"tag tag-mc\" style=\"background:#9c27b0;\">Save PS2</span>");
                                } else {
                                    httpd_resp_sendstr_chunk(req, "<span class=\"tag tag-mc\">Save PS1</span>");
                                }
                            } else {
                                httpd_resp_sendstr_chunk(req, "<span class=\"tag tag-file\">Arquivo</span>");
                            }
                            httpd_resp_sendstr_chunk(req, "</td><td>");
                            httpd_resp_sendstr_chunk(req, entrysize);
                            httpd_resp_sendstr_chunk(req, " B</td><td><div class=\"actions-cell\">");
                            
                            // Download button
                            httpd_resp_sendstr_chunk(req, "<a href=\"");
                            send_url_encoded(req, url_prefix, true);
                            send_url_encoded(req, entry->d_name, false);
                            httpd_resp_sendstr_chunk(req, "\" download class=\"btn btn-blue\"><span>⬇️</span> <span data-i18n=\"btn_download\">Baixar</span></a>");

                            if (is_mc) {
                                if (!is_mounted) {
                                    httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/mount");
                                    send_url_encoded(req, url_prefix, true);
                                    send_url_encoded(req, entry->d_name, false);
                                    httpd_resp_sendstr_chunk(req, "\" style=\"margin:0; display:inline-flex;\"><button type=\"submit\" class=\"btn btn-green\">▲ Montar</button></form>");

                                    // Duplicate button
                                    httpd_resp_sendstr_chunk(req, "<button type=\"button\" class=\"btn btn-purple\" onclick=\"duplicateCard(decodeURIComponent('");
                                    send_url_encoded(req, entrypath, false);
                                    httpd_resp_sendstr_chunk(req, "'), decodeURIComponent('");
                                    send_url_encoded(req, entry->d_name, false);
                                    httpd_resp_sendstr_chunk(req, "'))\">❐ Duplicar</button>");

                                    // Rename button
                                    httpd_resp_sendstr_chunk(req, "<button type=\"button\" class=\"btn btn-grey\" onclick=\"renameCard(decodeURIComponent('");
                                    send_url_encoded(req, entrypath, false);
                                    httpd_resp_sendstr_chunk(req, "'), decodeURIComponent('");
                                    send_url_encoded(req, entry->d_name, false);
                                    httpd_resp_sendstr_chunk(req, "'))\">✏️ Renomear</button>");
                                }
                            }

                            if (!is_mounted) {
                                httpd_resp_sendstr_chunk(req, "<button type=\"button\" class=\"btn btn-red\" onclick=\"deleteCard(decodeURIComponent('");
                                send_url_encoded(req, entrypath, false);
                                httpd_resp_sendstr_chunk(req, "'), decodeURIComponent('");
                                send_url_encoded(req, entry->d_name, false);
                                httpd_resp_sendstr_chunk(req, "'))\">● Excluir</button>");
                            } else {
                                httpd_resp_sendstr_chunk(req, "<span style=\"font-size:11px; color:var(--ps-green); font-weight:600; display:inline-flex; align-items:center; height:30px;\">🔒 Em Uso</span>");
                            }
                            httpd_resp_sendstr_chunk(req, "</div></td></tr>\n");
                        }
                    }
                    closedir(dir);
                }
                else
                {
                    httpd_resp_sendstr_chunk(req, "<tr><td colspan=\"5\" style=\"text-align:center; padding:35px 15px; color:var(--text-muted);\">"
                                                  "<div style=\"font-size:32px; margin-bottom:10px;\">💾</div>"
                                                  "<div style=\"font-size:16px; font-weight:700; color:#fff; margin-bottom:6px;\">Cartão MicroSD Desmontado</div>"
                                                  "<div style=\"font-size:12px; margin-bottom:18px;\">Insira o cartão MicroSD e clique no botão abaixo para montar e ler os saves.</div>"
                                                  "<button type=\"button\" class=\"btn btn-green\" onclick=\"mountMicroSD()\" style=\"padding:10px 22px; font-size:13px; font-weight:600; display:inline-flex; align-items:center; gap:8px;\"><span>🔄</span> <span>Montar Cartão SD</span></button>"
                                                  "</td></tr>");
                }

                /* Finish the file list table */
                httpd_resp_sendstr_chunk(req, "</tbody></table></div>");

                /* Send footer.html (Tab 2, Tab 3, Tab 4 and JS) */
                extern const unsigned char footer_start[] asm("_binary_footer_html_start");
                extern const unsigned char footer_end[] asm("_binary_footer_html_end");
                const size_t footer_size = (footer_end - footer_start);
                httpd_resp_send_chunk(req, (const char *)footer_start, footer_size);

                /* Send empty chunk to signal HTTP response completion */
                httpd_resp_send_chunk(req, NULL, 0);

                return ESP_OK;
            }

            /* Set HTTP response content type according to file extension */
            static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
            {
                if (IS_FILE_EXT(filename, ".pdf"))
                {
                    return httpd_resp_set_type(req, "application/pdf");
                }
                else if (IS_FILE_EXT(filename, ".html"))
                {
                    return httpd_resp_set_type(req, "text/html");
                }
                else if (IS_FILE_EXT(filename, ".jpeg"))
                {
                    return httpd_resp_set_type(req, "image/jpeg");
                }
                else if (IS_FILE_EXT(filename, ".ico"))
                {
                    return httpd_resp_set_type(req, "image/x-icon");
                }
                else if (IS_FILE_EXT(filename, ".mc") || IS_FILE_EXT(filename, ".mcd") || IS_FILE_EXT(filename, ".mcr") || IS_FILE_EXT(filename, ".bin"))
                {
                    // Binary card images: force a download instead of showing garbage as text
                    httpd_resp_set_hdr(req, "Content-Disposition", "attachment");
                    return httpd_resp_set_type(req, "application/octet-stream");
                }
                /* This is a limited set only */
                /* For any other type always set as plain text */
                return httpd_resp_set_type(req, "text/plain");
            }

            /* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
            static bool is_protected_config_file(const char *path)
            {
                if (!path || path[0] == '\0')
                {
                    return false;
                }
                const char *last_slash = strrchr(path, '/');
                const char *basename = last_slash ? (last_slash + 1) : path;
                return (strcasecmp(basename, "config.txt") == 0);
            }

            static void json_escape_string(const char *in, char *out, size_t out_size)
            {
                size_t j = 0;
                if (!in)
                {
                    if (out_size > 0) out[0] = '\0';
                    return;
                }
                for (size_t i = 0; in[i] != '\0' && j + 2 < out_size; i++)
                {
                    unsigned char c = (unsigned char)in[i];
                    if (c == '"' || c == '\\')
                    {
                        out[j++] = '\\';
                        out[j++] = c;
                    }
                    else if (c == '\n')
                    {
                        out[j++] = '\\';
                        out[j++] = 'n';
                    }
                    else if (c == '\r')
                    {
                        out[j++] = '\\';
                        out[j++] = 'r';
                    }
                    else if (c == '\t')
                    {
                        out[j++] = '\\';
                        out[j++] = 't';
                    }
                    else if (c >= 32)
                    {
                        out[j++] = c;
                    }
                }
                out[j] = '\0';
            }

            /* True if any path segment is exactly "." or ".." (directory traversal / normalization bypass) */
            static bool has_dot_segment(const char *p)
            {
                while (*p)
                {
                    while (*p == '/')
                    {
                        p++;
                    }
                    const char *segment = p;
                    while (*p && *p != '/')
                    {
                        p++;
                    }
                    size_t seg_len = (size_t)(p - segment);
                    if (seg_len == 1 && segment[0] == '.')
                    {
                        return true;
                    }
                    if (seg_len == 2 && segment[0] == '.' && segment[1] == '.')
                    {
                        return true;
                    }
                }
                return false;
            }


            static const char *get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
            {
                char raw_path[513];
                char decoded_uri[513];

                // The query string starts at the first RAW '?'. (An encoded %3F or %23 is part of the file name.)
                const size_t raw_len = strcspn(uri, "?");
                if (raw_len >= sizeof(raw_path))
                {
                    return NULL;
                }
                memcpy(raw_path, uri, raw_len);
                raw_path[raw_len] = '\0';

                // '+' is a literal character in URL paths (only form bodies treat it as a space)
                if (!web::PercentDecode(raw_path, decoded_uri, sizeof(decoded_uri), false))
                {
                    return NULL;
                }

                if (has_dot_segment(decoded_uri) || strchr(decoded_uri, '\\') != NULL)
                {
                    return NULL;
                }

                // Normalize consecutive slashes (e.g. "//config.txt" -> "/config.txt")
                char normalized[513];
                size_t norm_idx = 0;
                bool prev_slash = false;
                for (size_t i = 0; decoded_uri[i] != '\0' && norm_idx < sizeof(normalized) - 1; i++)
                {
                    if (decoded_uri[i] == '/')
                    {
                        if (!prev_slash)
                        {
                            normalized[norm_idx++] = '/';
                            prev_slash = true;
                        }
                    }
                    else
                    {
                        normalized[norm_idx++] = decoded_uri[i];
                        prev_slash = false;
                    }
                }
                normalized[norm_idx] = '\0';

                const size_t base_pathlen = strlen(base_path);
                const size_t pathlen = strlen(normalized);

                if (base_pathlen + pathlen + 1 > destsize)
                {
                    /* Full path string won't fit into destination buffer */
                    return NULL;
                }

                memcpy(dest, base_path, base_pathlen);
                memcpy(dest + base_pathlen, normalized, pathlen + 1);

                /* Return pointer to path, skipping the base */
                return dest + base_pathlen;
            }

            /* Handler to download a file kept on the server */
            static esp_err_t download_get_handler(httpd_req_t *req)
            {
                char filepath[FILE_PATH_MAX];
                FILE *fd = NULL;
                struct stat file_stat;

                const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                                         req->uri, sizeof(filepath));

                //URLDecode(filepath);
                if (!filename)
                {
                    ESP_LOGE(kLogPrefix, "Filename is too long");
                    /* Respond with 500 Internal Server Error */
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
                    return ESP_FAIL;
                }

                if (strcmp(filename, "/") == 0 || strcmp(filename, "/index.html") == 0 ||
                    strcmp(filename, "/MemoryCards") == 0 || strcmp(filename, "/MemoryCards/") == 0 ||
                    strcmp(filename, "/MemoryCards/PS1") == 0)
                {
                    httpd_resp_set_status(req, "302 Found");
                    httpd_resp_set_hdr(req, "Location", "/MemoryCards/PS1/");
                    httpd_resp_send(req, NULL, 0);
                    return ESP_OK;
                }

                /* If name has trailing '/', respond with directory contents */
                if (filename[strlen(filename) - 1] == '/')
                {
                    return http_resp_dir_html(req, filepath);
                }

                /* config.txt holds the Wi-Fi password: never serve it regardless of path format */
                if (is_protected_config_file(filepath))
                {
                    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden");
                    return ESP_FAIL;
                }

                if (is_protected_config_file(filepath))
                {
                    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden: config.txt cannot be deleted");
                    return ESP_FAIL;
                }

                if (stat(filepath, &file_stat) == -1)
                {
                    /* If file not present on SPIFFS check if URI
         * corresponds to one of the hardcoded paths */
                    if (strcmp(filename, "/index.html") == 0)
                    {
                        return index_html_get_handler(req);
                    }
                    else if (strcmp(filename, "/favicon.ico") == 0)
                    {
                        return favicon_get_handler(req);
                    }
                    ESP_LOGE(kLogPrefix, "Failed to stat file : %s", filepath);
                    /* Respond with 404 Not Found */
                    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
                    return ESP_FAIL;
                }

                fd = fopen(filepath, "rb");
                if (!fd)
                {
                    ESP_LOGE(kLogPrefix, "Failed to read existing file : %s", filepath);
                    /* Respond with 500 Internal Server Error */
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
                    return ESP_FAIL;
                }

                //ESP_LOGI(kLogPrefix, "Sending file : %s (%ld bytes)...", filename, file_stat.st_size);
                set_content_type_from_file(req, filename);

                /* Retrieve the pointer to scratch buffer for temporary storage */
                char *chunk = ((struct file_server_data *)req->user_ctx)->scratch;
                size_t chunksize;
                do
                {
                    /* Read file in chunks into the scratch buffer */
                    chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);

                    if (chunksize > 0)
                    {
                        /* Send the buffer contents as HTTP response chunk */
                        if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK)
                        {
                            fclose(fd);
                            ESP_LOGE(kLogPrefix, "File sending failed!");
                            /* Abort sending file */
                            httpd_resp_sendstr_chunk(req, NULL);
                            /* Respond with 500 Internal Server Error */
                            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                            return ESP_FAIL;
                        }
                    }

                    /* Keep looping till the whole file is sent */
                } while (chunksize != 0);

                /* Close file after sending complete */
                fclose(fd);
                //ESP_LOGI(kLogPrefix, "File sending complete");

                /* Respond with an empty chunk to signal HTTP response completion */
                httpd_resp_send_chunk(req, NULL, 0);
                return ESP_OK;
            }

            
static bool fs_transfer_busy = false;

static bool IsFsPathAllowed(const char *rel_path)
{
    if (!rel_path || rel_path[0] == '\0' || rel_path[0] == '/')
    {
        return false;
    }
    if (strstr(rel_path, "..") || strchr(rel_path, ':'))
    {
        return false;
    }
    if (strlen(rel_path) > storage::kCardPathMax)
    {
        return false;
    }

    const char *base_name = strrchr(rel_path, '/');
    base_name = (base_name != NULL) ? (base_name + 1) : rel_path;
    if (strcasecmp(base_name, "config.txt") == 0 || strcasecmp(base_name, "titles.db") == 0)
    {
        return false;
    }

    if (strcmp(rel_path, "MemoryCards") == 0 ||
        strcmp(rel_path, "MemoryCards/PS1") == 0 ||
        strncmp(rel_path, "MemoryCards/PS1/", 16) == 0 ||
        strcmp(rel_path, ".sd2psx") == 0 ||
        strncmp(rel_path, ".sd2psx/", 8) == 0 ||
        strcmp(rel_path, "games") == 0 ||
        strncmp(rel_path, "games/", 6) == 0)
    {
        return true;
    }

    if (strcmp(rel_path, "default.mc") == 0 ||
        strcmp(rel_path, "Game2Folder.ini") == 0 ||
        strcmp(rel_path, "multidisc.txt") == 0)
    {
        return true;
    }

    if (strncmp(rel_path, "default.mc.bak", 14) == 0 ||
        strncmp(rel_path, "Game2Folder.ini.bak", 19) == 0 ||
        strncmp(rel_path, "multidisc.txt.bak", 17) == 0)
    {
        return true;
    }

    // Sem catch-all por extensao: cartao e .ini so dentro das raizes acima.
    // O script de migracao so precisa de MemoryCards/PS1, .sd2psx, games e dos
    // tres arquivos legados da raiz (com seus .bak), todos ja cobertos.
    return false;
}

// Raizes que o firmware/script possuem: so saem vazias, nunca por remocao de
// conteudo. Qualquer outra pasta de jogo sai quando nao tem cartao dentro.
static bool IsProtectedFolder(const char *rel_path)
{
    return strcasecmp(rel_path, "MemoryCards") == 0 ||
           strcasecmp(rel_path, "MemoryCards/PS1") == 0 ||
           strcasecmp(rel_path, ".sd2psx") == 0 ||
           strcasecmp(rel_path, "games") == 0;
}

/* Handler to upload a file onto the server */
            static esp_err_t upload_post_handler(httpd_req_t *req)
            {
                char filepath[FILE_PATH_MAX];
                const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                                         req->uri + sizeof("/upload") - 1, sizeof(filepath));
                if (!filename || filename[strlen(filename) - 1] == '/')
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
                    return ESP_FAIL;
                }

                const char *dot = strrchr(filename, '.');
                if (!dot || (strcasecmp(dot, ".mc") != 0 && strcasecmp(dot, ".mcd") != 0 && strcasecmp(dot, ".mcr") != 0))
                {
                    httpd_resp_set_status(req, "415 Unsupported Media Type");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Only .mcd/.mc/.mcr allowed\"}");
                    return ESP_FAIL;
                }

                char clean_name[128];
                strlcpy(clean_name, filename, sizeof(clean_name));
                char *clean_dot = strrchr(clean_name, '.');
                if (clean_dot) *clean_dot = '\0';

                storage::EnsureDirectoryPublic("MemoryCards/PS1/UserCards");
                unsigned max_channels = storage::ReadFolderMaxChannels(storage::kUserCardsFolder, storage::kUserCardsFolder);
                unsigned target_ch = 0;
                char card_path[storage::kCardPathMax];
                struct stat st;

                for (unsigned ch = storage::kCardChannelMin; ch <= max_channels; ch++)
                {
                    snprintf(card_path, sizeof(card_path), "%s/%s/%s/%s-%u.mcd",
                             storage::base_path, storage::kCardsRoot, storage::kUserCardsFolder, storage::kUserCardsFolder, ch);
                    if (stat(card_path, &st) != 0)
                    {
                        target_ch = ch;
                        break;
                    }
                }

                if (target_ch == 0)
                {
                    httpd_resp_set_status(req, "409 Conflict");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"UserCards full\"}");
                    return ESP_FAIL;
                }

                FILE *fd = fopen(card_path, "wb");
                if (!fd)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create card image");
                    return ESP_FAIL;
                }

                char *buf = ((struct file_server_data *)req->user_ctx)->scratch;
                int remaining = req->content_len;
                int total_got = 0;
                bool err = false;

                while (remaining > 0)
                {
                    int to_read = remaining > 4096 ? 4096 : remaining;
                    int received = httpd_req_recv(req, buf, to_read);
                    if (received <= 0)
                    {
                        err = true;
                        break;
                    }
                    if (fwrite(buf, 1, received, fd) != (size_t)received)
                    {
                        err = true;
                        break;
                    }
                    total_got += received;
                    remaining -= received;
                }

                if (err || total_got != (int)storage::kCardSizeBytes || fflush(fd) != 0)
                {
                    fclose(fd);
                    unlink(card_path);
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File upload failed or invalid size (expected 128KB)");
                    return ESP_FAIL;
                }

                fsync(fileno(fd));
                fclose(fd);

                storage::WriteFolderChannelName(storage::kUserCardsFolder, storage::kUserCardsFolder, target_ch, clean_name);

                storage::GameMatch matches[kMaxGameMatches];
                size_t match_count = 0;
                storage::LookupGameByName(clean_name, matches, kMaxGameMatches, &match_count);

                char resp[1024];
                int offset = snprintf(resp, sizeof(resp),
                    "{\"created\":{\"folder\":\"UserCards\",\"channel\":%u,\"path\":\"%s/%s/%s-%u.mcd\"},\"channel\":%u",
                    target_ch, storage::kCardsRoot, storage::kUserCardsFolder, storage::kUserCardsFolder, target_ch, target_ch);

                if (match_count > 0)
                {
                    offset += snprintf(resp + offset, sizeof(resp) - offset,
                        ",\"suggestion\":{\"id\":\"%s\",\"title\":\"%s\",\"folder\":\"%s\",\"confidence\":%.2f}",
                        matches[0].id, matches[0].title, matches[0].folder, (float)matches[0].confidence / 100.0f);

                    offset += snprintf(resp + offset, sizeof(resp) - offset, ",\"alternatives\":[");
                    for (size_t i = 1; i < match_count; i++)
                    {
                        offset += snprintf(resp + offset, sizeof(resp) - offset,
                            "%s{\"id\":\"%s\",\"title\":\"%s\",\"folder\":\"%s\",\"confidence\":%.2f}",
                            (i > 1 ? "," : ""), matches[i].id, matches[i].title, matches[i].folder, (float)matches[i].confidence / 100.0f);
                    }
                    offset += snprintf(resp + offset, sizeof(resp) - offset, "]}");
                }
                else
                {
                    offset += snprintf(resp + offset, sizeof(resp) - offset, ",\"suggestion\":null,\"alternatives\":[]}");
                }

                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, resp, strlen(resp));
            }

            /* Handler to delete a file from the server */
            static esp_err_t delete_post_handler(httpd_req_t *req)
            {
                char filepath[FILE_PATH_MAX];
                const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                                         req->uri + sizeof("/delete") - 1, sizeof(filepath));
                if (!filename || filename[strlen(filename) - 1] == '/')
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cannot delete directory");
                    return ESP_FAIL;
                }

                struct stat file_stat;
                if (stat(filepath, &file_stat) == -1 || S_ISDIR(file_stat.st_mode))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cannot delete directory or missing file");
                    return ESP_FAIL;
                }

                const char *ext = strrchr(filepath, '.');
                if (!ext || strcasecmp(ext, ".mcd") != 0)
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Only .mcd files can be deleted");
                    return ESP_FAIL;
                }

                if (strcasecmp(filepath, storage::loaded_file_path) == 0)
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cannot delete currently mounted card");
                    return ESP_FAIL;
                }

                unlink(filepath);
                char redirect_path[FILE_PATH_MAX];
                strlcpy(redirect_path, filename, sizeof(redirect_path));
                char *end_slash = strrchr(redirect_path, '/');
                if (end_slash)
                {
                    end_slash[1] = '\0';
                }
                else
                {
                    strlcpy(redirect_path, "/MemoryCards/PS1/", sizeof(redirect_path));
                }
                httpd_resp_set_status(req, "303 See Other");
                httpd_resp_set_hdr(req, "Location", redirect_path);
                return httpd_resp_sendstr(req, "Deleted successfully");
            }

            static esp_err_t mount_post_handler(httpd_req_t *req)
            {
                char filepath[FILE_PATH_MAX];
                char redirect_path[FILE_PATH_MAX];
                char *end_of_redirect_path;
                struct stat file_stat;

                /* Skip leading "/mount" from URI to get filename */
                /* Note sizeof() counts NULL termination hence the -1 */
                const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                                         req->uri + sizeof("/mount") - 1, sizeof(filepath));
                //URLDecode(filepath);
                if (!filename)
                {
                    /* Respond with 500 Internal Server Error */
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
                    return ESP_FAIL;
                }

                //ESP_LOGI(kLogPrefix, "filename = %s, filepath = %s\n", filename, filepath);

                /* Filename cannot have a trailing '/' */
                if (filename[strlen(filename) - 1] == '/')
                {
                    ESP_LOGE(kLogPrefix, "Invalid filename : %s", filename);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
                    return ESP_FAIL;
                }

                if (stat(filepath, &file_stat) == -1)
                {
                    ESP_LOGE(kLogPrefix, "File does not exist : %s\n", filename);
                    ESP_LOGE(kLogPrefix, "filename = %s, filepath = %s\n", filename, filepath);
                    /* Respond with 400 Bad Request */
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File does not exist");
                    return ESP_FAIL;
                }

                strlcpy(redirect_path, filename, sizeof(redirect_path));
                end_of_redirect_path = strrchr(redirect_path, '/');
                if (end_of_redirect_path)
                {
                    end_of_redirect_path[1] = '\0';
                }

                if (storage::LoadCardFromFile(filepath, sio::memory_card::memory_card_ram))
                {
                    // Successfully loaded the memory card image from storage
                    httpd_resp_set_status(req, "303 See Other");
                    httpd_resp_set_hdr(req, "Location", redirect_path);
                    httpd_resp_sendstr(req, "File mounted successfully");
#if ENABLE_OLED
                    gui::WakeDisplay();
#endif
                }
                else
                {
                    // Some failure occurred when loading the file
                    httpd_resp_set_status(req, "303 See Other");
                    httpd_resp_set_hdr(req, "Location", redirect_path);
                    httpd_resp_sendstr(req, "Failed to mount file");
                }
                return ESP_OK;
            }

            static bool GetFormField(const char *body, const char *key, char *out_val, size_t out_len)
            {
                if (!body || !key || !out_val || out_len == 0)
                {
                    return false;
                }
                out_val[0] = '\0';
                char temp[256];
                strlcpy(temp, body, sizeof(temp));

                char *saveptr = NULL;
                char *p = strtok_r(temp, "&", &saveptr);
                while (p != NULL)
                {
                    char *eq = strchr(p, '=');
                    if (eq)
                    {
                        *eq = '\0';
                        if (strcmp(p, key) == 0)
                        {
                            web::PercentDecode(eq + 1, out_val, out_len);
                            return true;
                        }
                    }
                    p = strtok_r(NULL, "&", &saveptr);
                }
                return false;
            }

            /* POST /api/card/create and POST /api/create */
            static esp_err_t api_card_create_handler(httpd_req_t *req)
            {
                char buf[256];
                int remaining = req->content_len;
                if (remaining <= 0 || remaining >= sizeof(buf))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
                    return ESP_FAIL;
                }
                int received = httpd_req_recv(req, buf, remaining);
                if (received <= 0)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request");
                    return ESP_FAIL;
                }
                buf[received] = '\0';

                char name[128];
                if (!GetFormField(buf, "name", name, sizeof(name)) || name[0] == '\0')
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing card name");
                    return ESP_FAIL;
                }

                // Strip any trailing file extension (.mc, .mcd, .mcr)
                char clean_name[128];
                strlcpy(clean_name, name, sizeof(clean_name));
                char *dot = strrchr(clean_name, '.');
                if (dot && (strcasecmp(dot, ".mc") == 0 || strcasecmp(dot, ".mcd") == 0 || strcasecmp(dot, ".mcr") == 0))
                {
                    *dot = '\0';
                }

                char target_folder[64];
                char norm_id[64];
                char channel_label[128] = {0};
                bool is_game_id = storage::NormalizeGameID(clean_name, norm_id, sizeof(norm_id));

                if (is_game_id)
                {
                    if (!storage::ResolveGameFolder(norm_id, target_folder, sizeof(target_folder), NULL, NULL))
                    {
                        strlcpy(target_folder, norm_id, sizeof(target_folder));
                    }
                }
                else
                {
                    // Nome livre NUNCA vira pasta: o cartao vai para UserCards e o
                    // nome digitado fica apenas no [ChannelName] do canal.
                    if (!storage::IsValidFolderName(clean_name))
                    {
                        httpd_resp_set_status(req, "400 Bad Request");
                        httpd_resp_set_type(req, "application/json");
                        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Nome de cartao ou pasta invalido\"}");
                    }
                    strlcpy(target_folder, storage::kUserCardsFolder, sizeof(target_folder));
                    strlcpy(channel_label, clean_name, sizeof(channel_label));
                }

                char rel_dir[128];
                snprintf(rel_dir, sizeof(rel_dir), "MemoryCards/PS1/%s", target_folder);
                storage::EnsureDirectoryPublic(rel_dir);

                unsigned max_channels = storage::ReadFolderMaxChannels(target_folder, target_folder);
                unsigned target_ch = 0;
                char card_path[storage::kCardPathMax];
                struct stat st;

                for (unsigned ch = storage::kCardChannelMin; ch <= max_channels && ch <= storage::kCardChannelMax; ch++)
                {
                    snprintf(card_path, sizeof(card_path), "%s/%s/%s/%s-%u.mcd",
                             storage::base_path, storage::kCardsRoot, target_folder, target_folder, ch);
                    if (stat(card_path, &st) != 0)
                    {
                        target_ch = ch;
                        break;
                    }
                }

                if (target_ch == 0)
                {
                    httpd_resp_set_status(req, "409 Conflict");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Limite de canais atingido para esta pasta\"}");
                }

                {
                    storage::StorageLock lock;
                    storage::FlushPending();
                    if (!storage::CreateBlankFormattedCard(card_path))
                    {
                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create card image");
                        return ESP_FAIL;
                    }
                    if (channel_label[0] != '\0')
                    {
                        storage::WriteFolderChannelName(target_folder, target_folder, target_ch, channel_label);
                    }
                }

                char resp[1024];
                snprintf(resp, sizeof(resp),
                    "{\"status\":\"ok\",\"folder\":\"%s\",\"channel\":%u,\"path\":\"%s\","
                    "\"created\":{\"folder\":\"%s\",\"channel\":%u,\"path\":\"%s\"}}",
                    target_folder, target_ch, card_path, target_folder, target_ch, card_path);

                httpd_resp_set_status(req, "200 OK");
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, resp, strlen(resp));
            }

            /* POST /api/card/place */
            static esp_err_t api_card_place_handler(httpd_req_t *req)
            {
                char buf[256];
                int remaining = req->content_len;
                if (remaining <= 0 || remaining >= sizeof(buf))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
                    return ESP_FAIL;
                }
                int received = httpd_req_recv(req, buf, remaining);
                if (received <= 0)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request");
                    return ESP_FAIL;
                }
                buf[received] = '\0';

                char folder[64], target[64], channel_str[16];
                if (!GetFormField(buf, "folder", folder, sizeof(folder)) ||
                    !GetFormField(buf, "channel", channel_str, sizeof(channel_str)))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing folder or channel");
                    return ESP_FAIL;
                }
                GetFormField(buf, "target", target, sizeof(target));

                unsigned channel = (unsigned)atoi(channel_str);
                if (target[0] == '\0')
                {
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_send(req, "{\"status\":\"ok\",\"action\":\"kept_in_usercards\"}", HTTPD_RESP_USE_STRLEN);
                }

                char out_path[storage::kCardPathMax];
                unsigned new_channel = 0;
                if (!storage::PlaceUserCard(folder, channel, target, out_path, sizeof(out_path), &new_channel))
                {
                    httpd_resp_set_status(req, "409 Conflict");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Placement failed\"}");
                    return ESP_FAIL;
                }

                char resp[512];
                snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"path\":\"%s\",\"channel\":%u}", out_path, new_channel);
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, resp, strlen(resp));
            }

            /* POST /api/card/name */
            static esp_err_t api_card_name_handler(httpd_req_t *req)
            {
                char buf[256];
                int remaining = req->content_len;
                if (remaining <= 0 || remaining >= sizeof(buf))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
                    return ESP_FAIL;
                }
                int received = httpd_req_recv(req, buf, remaining);
                if (received <= 0)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request");
                    return ESP_FAIL;
                }
                buf[received] = '\0';

                char folder[64], channel_str[16], name[128];
                if (!GetFormField(buf, "folder", folder, sizeof(folder)) ||
                    !GetFormField(buf, "channel", channel_str, sizeof(channel_str)) ||
                    !GetFormField(buf, "name", name, sizeof(name)))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing parameters");
                    return ESP_FAIL;
                }

                unsigned channel = (unsigned)atoi(channel_str);
                if (channel < storage::kCardChannelMin || channel > storage::kCardChannelMax)
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channel");
                    return ESP_FAIL;
                }

                if (!storage::IsValidFolderName(folder))
                {
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Pasta invalida\"}");
                }

                if (!storage::WriteFolderChannelName(folder, folder, channel, name))
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to update channel name");
                    return ESP_FAIL;
                }

                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
            }

            /* GET /api/channels?folder=<folder> */
            static esp_err_t api_channels_get_handler(httpd_req_t *req)
            {
                char folder[64] = {0};
                char query[128];
                if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
                {
                    httpd_query_key_value(query, "folder", folder, sizeof(folder));
                }
                if (folder[0] == '\0')
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing folder parameter");
                    return ESP_FAIL;
                }

                unsigned max_channels = storage::ReadFolderMaxChannels(folder, folder);
                unsigned active_channel = 0;

                char active_prefix[storage::kCardPathMax + 64];
                snprintf(active_prefix, sizeof(active_prefix), "%s/%s/%s/%s-",
                         storage::base_path, storage::kCardsRoot, folder, folder);
                size_t p_len = strlen(active_prefix);
                if (strncmp(storage::loaded_file_path, active_prefix, p_len) == 0)
                {
                    active_channel = (unsigned)atoi(storage::loaded_file_path + p_len);
                }

                httpd_resp_set_type(req, "application/json");
                char chunk[1024];
                snprintf(chunk, sizeof(chunk), "{\"folder\":\"%s\",\"max\":%u,\"active\":%u,\"channels\":[",
                         folder, max_channels, active_channel);
                httpd_resp_send_chunk(req, chunk, strlen(chunk));

                struct stat st;
                char card_path[storage::kCardPathMax];
                char ch_name[128];

                for (unsigned ch = storage::kCardChannelMin; ch <= max_channels; ch++)
                {
                    snprintf(card_path, sizeof(card_path), "%s/%s/%s/%s-%u.mcd",
                             storage::base_path, storage::kCardsRoot, folder, folder, ch);
                    bool exists = (stat(card_path, &st) == 0 && st.st_size == (off_t)storage::kCardSizeBytes);
                    ch_name[0] = '\0';
                    storage::ReadFolderChannelName(folder, folder, ch, ch_name, sizeof(ch_name));

                    char esc_name[256];
                    json_escape_string(ch_name, esc_name, sizeof(esc_name));

                    snprintf(chunk, sizeof(chunk), "%s{\"n\":%u,\"exists\":%s,\"name\":\"%s\"}",
                             (ch > storage::kCardChannelMin ? "," : ""), ch, exists ? "true" : "false", esc_name);
                    httpd_resp_send_chunk(req, chunk, strlen(chunk));
                }

                httpd_resp_send_chunk(req, "]}", 2);
                httpd_resp_send_chunk(req, NULL, 0);
                return ESP_OK;
            }

            /* POST /api/channel */
            static esp_err_t api_channel_post_handler(httpd_req_t *req)
            {
                char buf[256];
                int remaining = req->content_len;
                if (remaining <= 0 || remaining >= sizeof(buf))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
                    return ESP_FAIL;
                }
                int received = httpd_req_recv(req, buf, remaining);
                if (received <= 0)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request");
                    return ESP_FAIL;
                }
                buf[received] = '\0';

                char folder[64], channel_str[16];
                if (!GetFormField(buf, "folder", folder, sizeof(folder)) ||
                    !GetFormField(buf, "channel", channel_str, sizeof(channel_str)))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing folder or channel");
                    return ESP_FAIL;
                }

                unsigned channel = (unsigned)atoi(channel_str);
                unsigned max_channels = storage::ReadFolderMaxChannels(folder, folder);
                if (channel < storage::kCardChannelMin || channel > max_channels)
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Channel out of bounds");
                    return ESP_FAIL;
                }

                if (!storage::MountCardChannel(folder, channel))
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to mount channel");
                    return ESP_FAIL;
                }

                char resp[512];
                snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"folder\":\"%s\",\"channel\":%u,\"path\":\"%s\"}",
                         folder, channel, storage::loaded_file_path);
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, resp, strlen(resp));
            }

            /* POST /api/create_game */
            static esp_err_t api_create_game_handler(httpd_req_t *req)
            {
                char buf[256];
                int remaining = req->content_len;
                if (remaining <= 0 || remaining >= sizeof(buf))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
                    return ESP_FAIL;
                }
                int received = httpd_req_recv(req, buf, remaining);
                if (received <= 0)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
                    return ESP_FAIL;
                }
                buf[received] = '\0';

                char game_id[64] = "";
                char channel_str[16] = "1";
                GetFormField(buf, "game_id", game_id, sizeof(game_id));
                GetFormField(buf, "channel", channel_str, sizeof(channel_str));

                unsigned channel = (unsigned)atoi(channel_str);
                if (channel == 0) channel = 1;

                char out_path[storage::kCardPathMax];
                char out_folder[64];
                bool out_exists = false;

                if (!storage::CreateGameCardForID(game_id, channel, out_path, sizeof(out_path), out_folder, sizeof(out_folder), &out_exists))
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create card");
                    return ESP_FAIL;
                }

                char resp[512];
                snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"folder\":\"%s\",\"channel\":%u,\"path\":\"%s\",\"exists\":%s}",
                         out_folder, channel, out_path, out_exists ? "true" : "false");
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, resp, strlen(resp));
            }

            /* GET /api/fs/list?path=<rel> */
            static esp_err_t api_fs_list_handler(httpd_req_t *req)
            {
                char query[256] = {0};
                char raw_path[storage::kCardPathMax] = {0};
                char rel_path[storage::kCardPathMax] = {0};
                if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
                {
                    httpd_query_key_value(query, "path", raw_path, sizeof(raw_path));
                    if (strchr(raw_path, '%'))
                    {
                        web::PercentDecode(raw_path, rel_path, sizeof(rel_path), false);
                    }
                    else
                    {
                        strlcpy(rel_path, raw_path, sizeof(rel_path));
                    }
                }

                if (rel_path[0] != '\0' && !IsFsPathAllowed(rel_path))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or disallowed path");
                    return ESP_FAIL;
                }

                char full_dir[storage::kCardPathMax + 32];
                if (rel_path[0] == '\0')
                {
                    snprintf(full_dir, sizeof(full_dir), "%s", storage::base_path);
                }
                else
                {
                    snprintf(full_dir, sizeof(full_dir), "%s/%s", storage::base_path, rel_path);
                }

                DIR *d = opendir(full_dir);
                if (!d)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open directory");
                    return ESP_FAIL;
                }

                httpd_resp_set_type(req, "application/json");
                char chunk[1024];
                snprintf(chunk, sizeof(chunk), "{\"path\":\"%s\",\"entries\":[", rel_path);
                httpd_resp_send_chunk(req, chunk, strlen(chunk));

                struct dirent *de;
                bool first = true;
                while ((de = readdir(d)) != NULL)
                {
                    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                    {
                        continue;
                    }

                    char entry_path[storage::kCardPathMax + 300];
                    snprintf(entry_path, sizeof(entry_path), "%s/%s", full_dir, de->d_name);
                    struct stat st;
                    if (stat(entry_path, &st) != 0)
                    {
                        continue;
                    }

                    char esc_name[CONFIG_FATFS_MAX_LFN * 2 + 1];
                    json_escape_string(de->d_name, esc_name, sizeof(esc_name));

                    bool is_dir = S_ISDIR(st.st_mode);
                    snprintf(chunk, sizeof(chunk), "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%ld}",
                             first ? "" : ",", esc_name, is_dir ? "true" : "false", is_dir ? 0L : (long)st.st_size);
                    first = false;
                    httpd_resp_send_chunk(req, chunk, strlen(chunk));
                }

                closedir(d);
                httpd_resp_send_chunk(req, "]}", 2);
                httpd_resp_send_chunk(req, NULL, 0);
                return ESP_OK;
            }

            /* POST /api/fs/put?path=<rel>[&overwrite=1] */
            static esp_err_t api_fs_put_handler(httpd_req_t *req)
            {
                if (fs_transfer_busy)
                {
                    httpd_resp_set_status(req, "409 Conflict");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Transfer in progress\"}");
                    return ESP_FAIL;
                }
                fs_transfer_busy = true;

                if (req->content_len <= 0 || req->content_len > MAX_FILE_SIZE)
                {
                    fs_transfer_busy = false;
                    httpd_resp_set_status(req, "413 Payload Too Large");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Arquivo maior que 8MB\"}");
                }

                char query[128] = {0};
                char rel_path[storage::kCardPathMax] = {0};
                char overwrite_str[16] = {0};
                if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
                {
                    httpd_query_key_value(query, "path", rel_path, sizeof(rel_path));
                    httpd_query_key_value(query, "overwrite", overwrite_str, sizeof(overwrite_str));
                }

                if (strchr(rel_path, '%'))
                {
                    char decoded_path[storage::kCardPathMax];
                    if (web::PercentDecode(rel_path, decoded_path, sizeof(decoded_path), false))
                    {
                        strlcpy(rel_path, decoded_path, sizeof(rel_path));
                    }
                }

                if (!IsFsPathAllowed(rel_path))
                {
                    fs_transfer_busy = false;
                    httpd_resp_set_status(req, "403 Forbidden");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Path disallowed\"}");
                    return ESP_FAIL;
                }

                char final_path[storage::kCardPathMax + 32];
                snprintf(final_path, sizeof(final_path), "%s/%s", storage::base_path, rel_path);

                bool overwrite = (strcmp(overwrite_str, "1") == 0);
                struct stat st;
                char part_path[storage::kCardPathMax + 40];
                snprintf(part_path, sizeof(part_path), "%s.part", final_path);

                char parent_dir[storage::kCardPathMax];
                strlcpy(parent_dir, rel_path, sizeof(parent_dir));

                // Lock curto: as checagens iniciais e a criacao da pasta. O
                // recebimento do corpo (abaixo) roda FORA do lock, para nao
                // travar a tarefa que grava os saves do console.
                {
                    storage::StorageLock lock;
                    storage::FlushPending();

                    if (storage::loaded_file_path[0] != '\0' && strcasecmp(storage::loaded_file_path, final_path) == 0)
                    {
                        fs_transfer_busy = false;
                        httpd_resp_set_status(req, "409 Conflict");
                        httpd_resp_set_type(req, "application/json");
                        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Cannot overwrite currently mounted card\"}");
                    }

                    if (stat(final_path, &st) == 0 && !overwrite)
                    {
                        fs_transfer_busy = false;
                        httpd_resp_set_status(req, "409 Conflict");
                        httpd_resp_set_type(req, "application/json");
                        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"File exists\"}");
                    }

                    char *last_slash = strrchr(parent_dir, '/');
                    if (last_slash)
                    {
                        *last_slash = '\0';
                        if (!storage::EnsureDirectoryPublic(parent_dir))
                        {
                            fs_transfer_busy = false;
                            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create parent dir");
                            return ESP_FAIL;
                        }
                    }
                }

                FILE *f = fopen(part_path, "wb");
                if (!f)
                {
                    fs_transfer_busy = false;
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open part file");
                    return ESP_FAIL;
                }

                uint8_t *chunk = (uint8_t *)malloc(8192);
                if (!chunk)
                {
                    fclose(f);
                    unlink(part_path);
                    fs_transfer_busy = false;
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
                    return ESP_FAIL;
                }

                int remaining = req->content_len;
                bool err = false;
                while (remaining > 0)
                {
                    int to_read = remaining > 8192 ? 8192 : remaining;
                    int got = httpd_req_recv(req, (char *)chunk, to_read);
                    if (got <= 0)
                    {
                        err = true;
                        break;
                    }
                    if (fwrite(chunk, 1, got, f) != (size_t)got)
                    {
                        err = true;
                        break;
                    }
                    remaining -= got;
                }

                free(chunk);

                if (err || fflush(f) != 0)
                {
                    fclose(f);
                    unlink(part_path);
                    fs_transfer_busy = false;
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write error");
                    return ESP_FAIL;
                }

                fsync(fileno(f));
                fclose(f);

                // Commit sob lock: o cartao pode ter sido montado durante o envio.
                {
                    storage::StorageLock lock;
                    storage::FlushPending();

                    if (storage::loaded_file_path[0] != '\0' && strcasecmp(storage::loaded_file_path, final_path) == 0)
                    {
                        unlink(part_path);
                        fs_transfer_busy = false;
                        httpd_resp_set_status(req, "409 Conflict");
                        httpd_resp_set_type(req, "application/json");
                        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Card mounted during upload\"}");
                    }

                    char bak_path[storage::kCardPathMax + 40];
                    snprintf(bak_path, sizeof(bak_path), "%s.bak", final_path);
                    bool dest_existed = (stat(final_path, &st) == 0);
                    if (dest_existed)
                    {
                        unlink(bak_path);
                        if (rename(final_path, bak_path) != 0)
                        {
                            unlink(part_path);
                            fs_transfer_busy = false;
                            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Backup rename failed");
                            return ESP_FAIL;
                        }
                    }

                    if (rename(part_path, final_path) != 0)
                    {
                        unlink(part_path);
                        if (dest_existed)
                        {
                            rename(bak_path, final_path);
                        }
                        fs_transfer_busy = false;
                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
                        return ESP_FAIL;
                    }

                    if (dest_existed)
                    {
                        unlink(bak_path);
                    }
                }

                storage::ProbeLegacyLayout();
                fs_transfer_busy = false;

                httpd_resp_set_status(req, "201 Created");
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
            }

            /* POST /api/fs/delete?path=<rel> */
            static esp_err_t api_fs_delete_handler(httpd_req_t *req)
            {
                char raw_rel[storage::kCardPathMax] = {0};
                char rel_path[storage::kCardPathMax] = {0};
                char query[512] = {0};
                if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
                {
                    httpd_query_key_value(query, "path", raw_rel, sizeof(raw_rel));
                }

                if (raw_rel[0] == '\0' && req->content_len > 0 && req->content_len < 512)
                {
                    char buf[512];
                    int ret = httpd_req_recv(req, buf, req->content_len);
                    if (ret > 0)
                    {
                        buf[ret] = '\0';
                        GetFormField(buf, "path", raw_rel, sizeof(raw_rel));
                    }
                }

                if (strchr(raw_rel, '%'))
                {
                    web::PercentDecode(raw_rel, rel_path, sizeof(rel_path), false);
                }
                else
                {
                    strlcpy(rel_path, raw_rel, sizeof(rel_path));
                }

                char *clean = rel_path;
                if (strncmp(clean, "/sdcard/", 8) == 0) clean += 8;
                else if (strncmp(clean, "sdcard/", 7) == 0) clean += 7;
                while (*clean == '/') clean++;
                if (clean != rel_path)
                {
                    memmove(rel_path, clean, strlen(clean) + 1);
                }

                if (!IsFsPathAllowed(rel_path))
                {
                    httpd_resp_set_status(req, "403 Forbidden");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Caminho nao permitido\"}");
                }

                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s/%s", storage::base_path, rel_path);

                struct stat st;
                if (stat(full_path, &st) != 0)
                {
                    httpd_resp_set_status(req, "404 Not Found");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Arquivo nao encontrado\"}");
                }

                storage::StorageLock lock;

                if (strcasecmp(storage::loaded_file_path, full_path) == 0)
                {
                    httpd_resp_set_status(req, "409 Conflict");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Desmonte o Memory Card antes de exclui-lo\"}");
                }

                if (S_ISDIR(st.st_mode))
                {
                    if (IsProtectedFolder(rel_path))
                    {
                        // Raizes: so saem vazias. Nunca apagar conteudo delas
                        // (o script de migracao chega aqui para remover
                        // diretorio que ja esta vazio).
                        if (rmdir(full_path) != 0)
                        {
                            httpd_resp_set_status(req, "400 Bad Request");
                            httpd_resp_set_type(req, "application/json");
                            return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Pasta nao esta vazia\"}");
                        }
                    }
                    else
                    {
                        // Pasta de jogo: sai quando NAO ha Memory Card dentro. Os
                        // arquivos auxiliares (rotulos [ChannelName] .ini) sao
                        // removidos junto, senao a pasta nunca mais seria
                        // apagavel depois de o usuario excluir os cartoes.
                        DIR *d = opendir(full_path);
                        if (d == NULL)
                        {
                            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open directory");
                            return ESP_FAIL;
                        }
                        bool has_card = false;
                        bool has_subdir = false;
                        char sub[512];
                        struct dirent *de;
                        while ((de = readdir(d)) != NULL)
                        {
                            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                            {
                                continue;
                            }
                            if (IS_FILE_EXT(de->d_name, ".mc") || IS_FILE_EXT(de->d_name, ".mcd") ||
                                IS_FILE_EXT(de->d_name, ".mcr"))
                            {
                                has_card = true;
                                break;
                            }
                            if (snprintf(sub, sizeof(sub), "%s/%s", full_path, de->d_name) < (int)sizeof(sub))
                            {
                                struct stat entry_st;
                                if (stat(sub, &entry_st) == 0 && S_ISDIR(entry_st.st_mode))
                                {
                                    has_subdir = true;
                                    break;
                                }
                            }
                        }
                        if (has_card || has_subdir)
                        {
                            closedir(d);
                            httpd_resp_set_status(req, "400 Bad Request");
                            httpd_resp_set_type(req, "application/json");
                            return httpd_resp_sendstr(req,
                                has_card
                                    ? "{\"status\":\"error\",\"message\":\"Exclua os Memory Cards da pasta antes de remove-la\"}"
                                    : "{\"status\":\"error\",\"message\":\"A pasta contem subpastas\"}");
                        }
                        rewinddir(d);
                        while ((de = readdir(d)) != NULL)
                        {
                            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                            {
                                continue;
                            }
                            if (snprintf(sub, sizeof(sub), "%s/%s", full_path, de->d_name) < (int)sizeof(sub))
                            {
                                unlink(sub);
                            }
                        }
                        closedir(d);
                        if (rmdir(full_path) != 0)
                        {
                            httpd_resp_set_status(req, "400 Bad Request");
                            httpd_resp_set_type(req, "application/json");
                            return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Nao foi possivel remover a pasta\"}");
                        }
                    }
                }
                else
                {
                    if (unlink(full_path) != 0)
                    {
                        httpd_resp_set_status(req, "500 Internal Server Error");
                        httpd_resp_set_type(req, "application/json");
                        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Falha ao excluir arquivo no SD\"}");
                    }
                }

                storage::ProbeLegacyLayout();

                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
            }

            /* API Handler: POST /api/unload (Eject Memory Card) */
            static esp_err_t api_unload_handler(httpd_req_t *req)
            {
                bool ok = storage::UnloadCard();
                httpd_resp_set_type(req, "application/json");
                if (!ok)
                {
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Falha ao gravar as alteracoes pendentes no MicroSD. O cartao continua montado.\"}");
                }
                return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"quote\":\"I can read your mind!\"}");
            }

            /* API Handler: POST /api/eject_sd (Safely Eject SD Card) */
            static esp_err_t api_eject_sd_handler(httpd_req_t *req)
            {
                bool ok = storage::SafelyUnmountSD();
                httpd_resp_set_type(req, "application/json");
                if (!ok)
                {
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Falha ao gravar as alteracoes pendentes. NAO remova o cartao; tente novamente.\"}");
                }
                return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Cartao MicroSD ejetado com seguranca! Pronto para remocao fisica.\"}");
            }

            /* API Handler: POST /api/mount_sd (Remount SD Card) */
            static esp_err_t api_mount_sd_handler(httpd_req_t *req)
            {
                bool ok = storage::RemountSD();
                httpd_resp_set_type(req, "application/json");
                if (ok)
                {
                    return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Cartao MicroSD montado com sucesso!\"}");
                }
                else
                {
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Falha ao montar cartao. Verifique se o MicroSD esta inserido.\"}");
                }
            }

            /* API Handler: POST /api/sio_toggle (Toggle Emulation vs Physical Card Passthrough) */
            static esp_err_t api_sio_toggle_handler(httpd_req_t *req)
            {
                bool new_state = !sio::memory_card_enabled;

                // Without a mounted image there is nothing to present, and the stale RAM contents could not be saved
                if (new_state && storage::loaded_file_path[0] == '\0')
                {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, "409 Conflict");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Nenhum Memory Card montado.\"}");
                }

                sio::SetMemoryCardEmulation(new_state);

                ESP_LOGI(kLogPrefix, "SIO Mode toggled: %s", new_state ? "EMULATION ACTIVE" : "PASSTHROUGH / PHYSICAL CARD");

                char resp[128];
                snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"emulation_active\":%s}",
                         new_state ? "true" : "false");

                httpd_resp_set_type(req, "application/json");
                return httpd_resp_sendstr(req, resp);
            }

            /* API Handler: POST /api/rename */
            static esp_err_t api_rename_handler(httpd_req_t *req)
            {
                char buf[256];
                int remaining = req->content_len;
                if (remaining <= 0 || remaining >= sizeof(buf))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
                    return ESP_FAIL;
                }
                int received = httpd_req_recv(req, buf, remaining);
                if (received <= 0)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
                    return ESP_FAIL;
                }
                buf[received] = '\0';

                char old_path_raw[FILE_PATH_MAX] = {0};
                char new_name[128] = {0};
                if (!GetFormField(buf, "old_path", old_path_raw, sizeof(old_path_raw)) ||
                    !GetFormField(buf, "new_name", new_name, sizeof(new_name)) ||
                    old_path_raw[0] == '\0' || new_name[0] == '\0')
                {
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Parametros ausentes\"}");
                }

                const char *rel_old = old_path_raw;
                if (strncmp(rel_old, "/sdcard/", 8) == 0) rel_old += 8;
                else if (strncmp(rel_old, "sdcard/", 7) == 0) rel_old += 7;
                while (*rel_old == '/') rel_old++;

                if (!IsFsPathAllowed(rel_old))
                {
                    httpd_resp_set_status(req, "403 Forbidden");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Caminho nao permitido\"}");
                }

                char full_old[512];
                snprintf(full_old, sizeof(full_old), "%s/%s", storage::base_path, rel_old);

                struct stat st;
                if (stat(full_old, &st) != 0)
                {
                    httpd_resp_set_status(req, "404 Not Found");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Arquivo original nao encontrado\"}");
                }

                if (strcasecmp(storage::loaded_file_path, full_old) == 0)
                {
                    httpd_resp_set_status(req, "409 Conflict");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Desmonte o Memory Card antes de renomea-lo\"}");
                }

                char rel_dir[storage::kCardPathMax];
                strlcpy(rel_dir, rel_old, sizeof(rel_dir));
                char *slash = strrchr(rel_dir, '/');
                if (slash) *slash = '\0';
                else rel_dir[0] = '\0';

                char clean_new[128];
                if (strlen(new_name) >= sizeof(clean_new))
                {
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Nome muito longo\"}");
                }
                strlcpy(clean_new, new_name, sizeof(clean_new));
                // O destino e sempre um nome de arquivo simples dentro da pasta
                // de origem: nada de separador, drive ou caractere de controle.
                if (strpbrk(clean_new, "/\\:*?\"<>|") != NULL ||
                    clean_new[0] == '.' || clean_new[0] == '\0')
                {
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Nome de arquivo invalido\"}");
                }

                const char *old_dot = strrchr(rel_old, '.');
                const char *new_dot = strrchr(clean_new, '.');
                if (old_dot == NULL)
                {
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Origem sem extensao\"}");
                }
                if (new_dot == NULL)
                {
                    strlcat(clean_new, old_dot, sizeof(clean_new));
                }
                else if (strcasecmp(new_dot, old_dot) != 0)
                {
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"A extensao do cartao deve ser mantida\"}");
                }

                char full_new[512];
                if (rel_dir[0])
                {
                    snprintf(full_new, sizeof(full_new), "%s/%s/%s", storage::base_path, rel_dir, clean_new);
                }
                else
                {
                    snprintf(full_new, sizeof(full_new), "%s/%s", storage::base_path, clean_new);
                }

                if (stat(full_new, &st) == 0 && strcasecmp(full_old, full_new) != 0)
                {
                    httpd_resp_set_status(req, "409 Conflict");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Ja existe um arquivo com esse nome\"}");
                }

                {
                    storage::StorageLock lock;
                    storage::FlushPending();
                    if (rename(full_old, full_new) != 0)
                    {
                        httpd_resp_set_status(req, "500 Internal Server Error");
                        httpd_resp_set_type(req, "application/json");
                        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Falha ao renomear arquivo no cartao SD\"}");
                    }
                }

                httpd_resp_set_type(req, "application/json");
                return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Renomeado com sucesso!\"}");
            }

            /* API Handler: POST /api/card/duplicate */
            static esp_err_t api_card_duplicate_handler(httpd_req_t *req)
            {
                char buf[256];
                int remaining = req->content_len;
                if (remaining <= 0 || remaining >= sizeof(buf))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
                    return ESP_FAIL;
                }
                int received = httpd_req_recv(req, buf, remaining);
                if (received <= 0)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
                    return ESP_FAIL;
                }
                buf[received] = '\0';

                char path_raw[FILE_PATH_MAX] = {0};
                if (!GetFormField(buf, "path", path_raw, sizeof(path_raw)) || path_raw[0] == '\0')
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
                    return ESP_FAIL;
                }

                const char *rel_path = path_raw;
                if (strncmp(rel_path, "/sdcard/", 8) == 0) rel_path += 8;
                else if (strncmp(rel_path, "sdcard/", 7) == 0) rel_path += 7;
                while (*rel_path == '/') rel_path++;

                if (!IsFsPathAllowed(rel_path))
                {
                    httpd_resp_set_status(req, "403 Forbidden");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Path disallowed\"}");
                }

                char full_src[512];
                snprintf(full_src, sizeof(full_src), "%s/%s", storage::base_path, rel_path);

                struct stat st;
                if (stat(full_src, &st) != 0 || S_ISDIR(st.st_mode))
                {
                    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Card not found");
                    return ESP_FAIL;
                }

                char dir_path[storage::kCardPathMax];
                strlcpy(dir_path, rel_path, sizeof(dir_path));
                char *slash = strrchr(dir_path, '/');
                char filename[128];
                if (slash)
                {
                    strlcpy(filename, slash + 1, sizeof(filename));
                    *slash = '\0';
                }
                else
                {
                    strlcpy(filename, rel_path, sizeof(filename));
                    dir_path[0] = '\0';
                }

                const char *folder_name = strrchr(dir_path, '/');
                if (folder_name) folder_name++;
                else folder_name = dir_path;

                char base_name[128];
                strlcpy(base_name, filename, sizeof(base_name));
                char *dot = strrchr(base_name, '.');
                char ext[16] = ".mcd";
                if (dot)
                {
                    strlcpy(ext, dot, sizeof(ext));
                    *dot = '\0';
                }
                char *dash = strrchr(base_name, '-');
                if (dash && isdigit((unsigned char)*(dash + 1)))
                {
                    *dash = '\0';
                }

                unsigned max_channels = storage::ReadFolderMaxChannels(folder_name, base_name);
                unsigned target_ch = 0;
                char target_full[512];

                for (unsigned ch = 1; ch <= max_channels && ch <= storage::kCardChannelMax; ch++)
                {
                    if (dir_path[0])
                    {
                        snprintf(target_full, sizeof(target_full), "%s/%s/%s-%u%s",
                                 storage::base_path, dir_path, base_name, ch, ext);
                    }
                    else
                    {
                        snprintf(target_full, sizeof(target_full), "%s/%s-%u%s",
                                 storage::base_path, base_name, ch, ext);
                    }
                    if (stat(target_full, &st) != 0)
                    {
                        target_ch = ch;
                        break;
                    }
                }

                if (target_ch == 0)
                {
                    httpd_resp_set_status(req, "409 Conflict");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Todos os canais (1 a 8) ja estao em uso nesta pasta\"}");
                }

                {
                    storage::StorageLock lock;
                    storage::FlushPending();

                    FILE *fsrc = fopen(full_src, "rb");
                    if (!fsrc)
                    {
                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open source file");
                        return ESP_FAIL;
                    }
                    FILE *fdst = fopen(target_full, "wb");
                    if (!fdst)
                    {
                        fclose(fsrc);
                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create target file");
                        return ESP_FAIL;
                    }

                    char *copy_buf = (char *)malloc(4096);
                    if (!copy_buf)
                    {
                        fclose(fdst);
                        fclose(fsrc);
                        unlink(target_full);
                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
                        return ESP_FAIL;
                    }

                    size_t bytes_read = 0;
                    bool copy_ok = true;
                    while ((bytes_read = fread(copy_buf, 1, 4096, fsrc)) > 0)
                    {
                        if (fwrite(copy_buf, 1, bytes_read, fdst) != bytes_read)
                        {
                            copy_ok = false;
                            break;
                        }
                    }
                    free(copy_buf);
                    fflush(fdst);
                    fclose(fdst);
                    fclose(fsrc);

                    if (!copy_ok)
                    {
                        unlink(target_full);
                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Copy write failed");
                        return ESP_FAIL;
                    }
                }

                char resp[1024];
                snprintf(resp, sizeof(resp),
                    "{\"status\":\"ok\",\"channel\":%u,\"path\":\"%s\",\"message\":\"Memory Card duplicado com sucesso!\"}",
                    target_ch, target_full);
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, resp, strlen(resp));
            }



            /* API Handler: GET /api/status */
            static esp_err_t api_status_handler(httpd_req_t *req)
            {
                static uint32_t last_df_time = 0;
                static uint64_t cached_total_bytes = 0;
                static uint64_t cached_free_bytes = 0;
                uint32_t now_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);

                if (storage::ready && (now_sec - last_df_time > 30 || last_df_time == 0))
                {
                    FATFS *fs = NULL;
                    DWORD fre_clust = 0;
                    if (f_getfree("0:", &fre_clust, &fs) == FR_OK && fs != NULL)
                    {
                        uint64_t tot_sect = (uint64_t)(fs->n_fatent - 2) * fs->csize;
                        uint64_t fre_sect = (uint64_t)fre_clust * fs->csize;
#if FF_MAX_SS != FF_MIN_SS
                        cached_total_bytes = tot_sect * fs->ssize;
                        cached_free_bytes = fre_sect * fs->ssize;
#else
                        cached_total_bytes = tot_sect * FF_SS;
                        cached_free_bytes = fre_sect * FF_SS;
#endif
                    }
                    last_df_time = now_sec;
                }
                uint64_t total_bytes = storage::ready ? cached_total_bytes : 0;
                uint64_t free_bytes = storage::ready ? cached_free_bytes : 0;

                int8_t rssi = 0;
                wifi_ap_record_t ap_info;
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
                {
                    rssi = ap_info.rssi;
                }

                static uint32_t last_count_time = 0;
                static int cached_ps1_count = 0;
                static int cached_ps2_count = 0;
                if (storage::ready && (now_sec - last_count_time > 5 || last_count_time == 0))
                {
                    cached_ps1_count = 0;
                    cached_ps2_count = 0;
                    char cards_root[storage::kCardPathMax];
                    snprintf(cards_root, sizeof(cards_root), "%s/%s", storage::base_path, storage::kCardsRoot);
                    DIR *d = opendir(cards_root);
                    if (d)
                    {
                        struct dirent *de;
                        while ((de = readdir(d)) != NULL)
                        {
                            if (de->d_name[0] == '.')
                            {
                                continue;
                            }
                            char folder_path[storage::kCardPathMax];
                            if (snprintf(folder_path, sizeof(folder_path), "%s/%s", cards_root, de->d_name) >= (int)sizeof(folder_path))
                            {
                                continue;
                            }
                            DIR *fd = opendir(folder_path);
                            if (!fd)
                            {
                                continue;
                            }
                            struct dirent *card;
                            while ((card = readdir(fd)) != NULL)
                            {
                                if (IS_FILE_EXT(card->d_name, ".mcd") || IS_FILE_EXT(card->d_name, ".mc") ||
                                    IS_FILE_EXT(card->d_name, ".mcr"))
                                {
                                    cached_ps1_count++;
                                }
                            }
                            closedir(fd);
                        }
                        closedir(d);
                    }
                    last_count_time = now_sec;
                }
                int ps1_count = cached_ps1_count;
                int ps2_count = cached_ps2_count;

                char ip_str[32] = "192.168.4.1";
                esp_netif_ip_info_t ip_info;
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey(system::settings.wifi_mode == wifi::Mode::kAcessPoint ? "WIFI_AP_DEF" : "WIFI_STA_DEF");
                if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
                {
                    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
                }

                esp_chip_info_t chip_info;
                esp_chip_info(&chip_info);

                rtc_cpu_freq_config_t freq_cfg;
                rtc_clk_cpu_freq_get_config(&freq_cfg);

                uint32_t flash_size = 0;
                esp_flash_get_size(NULL, &flash_size);

                uint32_t ram_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);

                char chip_desc[64];
                if (chip_info.revision >= 100) {
                    snprintf(chip_desc, sizeof(chip_desc), "ESP32-D0WD (Rev v%d.%d)", chip_info.revision / 100, chip_info.revision % 100);
                } else {
                    snprintf(chip_desc, sizeof(chip_desc), "ESP32-D0WD-V3 (Rev %d)", chip_info.revision);
                }

                char esc_ssid[68];
                char esc_loaded[CONFIG_FATFS_MAX_LFN * 2 + 1];
                json_escape_string((const char *)system::settings.ssid, esc_ssid, sizeof(esc_ssid));
                json_escape_string(storage::loaded_file_path, esc_loaded, sizeof(esc_loaded));

                uint32_t titles_version = 0;
                struct stat t_stat;
                char titles_path[CONFIG_FATFS_MAX_LFN + 16];
                snprintf(titles_path, sizeof(titles_path), "%s/titles.db", storage::base_path);
                if (storage::ready && stat(titles_path, &t_stat) == 0)
                {
                    titles_version = (uint32_t)((uint32_t)t_stat.st_size ^ (uint32_t)t_stat.st_mtime);
                }

                // Versao vem do app descriptor (CMakeLists). O fallback so vale
                // para builds de host, onde o descritor nao existe.
                const esp_app_desc_t *app_desc = esp_ota_get_app_description();
                const char *fw_version = (app_desc != NULL && app_desc->version[0] != '\0')
                                             ? app_desc->version
                                             : "1.5.2";

                char *json_resp = (char *)malloc(2048);
                if (!json_resp)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
                    return ESP_FAIL;
                }

                storage::LegacyProbeResult probe = storage::ProbeLegacyLayout();

                char active_folder[64] = "";
                unsigned active_channel = 0;
                char cards_pfx[CONFIG_FATFS_MAX_LFN + 32];
                snprintf(cards_pfx, sizeof(cards_pfx), "%s/MemoryCards/PS1/", storage::base_path);
                size_t pfx_len = strlen(cards_pfx);
                if (strncmp(storage::loaded_file_path, cards_pfx, pfx_len) == 0)
                {
                    const char *sub = storage::loaded_file_path + pfx_len;
                    const char *slash = strchr(sub, '/');
                    if (slash)
                    {
                        size_t f_len = slash - sub;
                        if (f_len < sizeof(active_folder))
                        {
                            memcpy(active_folder, sub, f_len);
                            active_folder[f_len] = '\0';
                        }
                        const char *dash = strrchr(slash + 1, '-');
                        if (dash)
                        {
                            active_channel = (unsigned)atoi(dash + 1);
                        }
                    }
                }

                char legacy_items[128];
                int l_off = snprintf(legacy_items, sizeof(legacy_items), "[");
                bool first_item = true;
                if (probe.games_dir) { l_off += snprintf(legacy_items + l_off, sizeof(legacy_items) - l_off, "%s\"games/\"", first_item ? "" : ","); first_item = false; }
                if (probe.default_mc) { l_off += snprintf(legacy_items + l_off, sizeof(legacy_items) - l_off, "%s\"default.mc\"", first_item ? "" : ","); first_item = false; }
                if (probe.root_game2folder) { l_off += snprintf(legacy_items + l_off, sizeof(legacy_items) - l_off, "%s\"Game2Folder.ini\"", first_item ? "" : ","); first_item = false; }
                if (probe.multidisc_txt) { l_off += snprintf(legacy_items + l_off, sizeof(legacy_items) - l_off, "%s\"multidisc.txt\"", first_item ? "" : ","); first_item = false; }
                snprintf(legacy_items + l_off, sizeof(legacy_items) - l_off, "]");

                int sn_res = snprintf(json_resp, 2048,
                    "{\"sd_total\":%llu,\"sd_free\":%llu,\"sd_mounted\":%s,\"heap_free\":%u,\"heap_min\":%u,\"ram_total\":%u,"
                    "\"wifi_mode\":%d,\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\","
                    "\"loaded_file\":\"%s\",\"folder\":\"%s\",\"channel\":%u,\"uptime\":%lld,\"mc_count\":%d,\"ps1_count\":%d,\"ps2_count\":%d,"
                    "\"chip\":\"%s\",\"cores\":%d,\"cpu_freq\":%u,\"flash_size\":%u,\"sio_emulation\":%s,\"titles_version\":%u,\"version\":\"%s\","
                    "\"legacy\":{\"detected\":%s,\"items\":%s}}",
                    (unsigned long long)total_bytes, (unsigned long long)free_bytes,
                    storage::ready ? "true" : "false",
                    (unsigned int)esp_get_free_heap_size(), (unsigned int)esp_get_minimum_free_heap_size(),
                    (unsigned int)ram_total,
                    (int)system::settings.wifi_mode, esc_ssid, (int)rssi, ip_str,
                    esc_loaded, active_folder, active_channel,
                    (long long)(esp_timer_get_time() / 1000000ULL),
                    (ps1_count + ps2_count), ps1_count, ps2_count,
                    chip_desc, (int)chip_info.cores, (unsigned int)freq_cfg.freq_mhz, (unsigned int)(flash_size / (1024 * 1024)),
                    sio::memory_card_enabled ? "true" : "false",
                    (unsigned int)titles_version,
                    fw_version,
                    probe.Any() ? "true" : "false", legacy_items);

                if (sn_res < 0 || sn_res >= 2048)
                {
                    snprintf(json_resp, 2048,
                        "{\"sd_mounted\":%s,\"version\":\"%s\",\"legacy\":{\"detected\":%s,\"items\":%s},\"truncated\":true}",
                        storage::ready ? "true" : "false",
                        fw_version,
                        probe.Any() ? "true" : "false", legacy_items);
                }

                httpd_resp_set_type(req, "application/json");
                esp_err_t res = httpd_resp_send(req, json_resp, strlen(json_resp));
                free(json_resp);
                return res;
            }

            /* API Handler: POST /api/config */
            static esp_err_t api_config_handler(httpd_req_t *req)
            {
                char buf[256];
                int remaining = req->content_len;
                if (remaining <= 0 || remaining >= sizeof(buf))
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
                    return ESP_FAIL;
                }

                int received = httpd_req_recv(req, buf, remaining);
                if (received <= 0)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request");
                    return ESP_FAIL;
                }
                buf[received] = '\0';

                char mode_val[16] = "client";
                char ssid_val[128] = "";
                char pass_val[128] = "";

                char *p = strtok(buf, "&");
                while (p != NULL)
                {
                    char *eq = strchr(p, '=');
                    if (eq)
                    {
                        *eq = '\0';
                        char *key = p;
                        char *val = eq + 1;
                        char decoded_val[128];
                        web::PercentDecode(val, decoded_val, sizeof(decoded_val)); // bounded; over-long values fail validation below
                        if (strcmp(key, "mode") == 0)
                        {
                            strlcpy(mode_val, decoded_val, sizeof(mode_val));
                        }
                        else if (strcmp(key, "ssid") == 0)
                        {
                            strlcpy(ssid_val, decoded_val, sizeof(ssid_val));
                        }
                        else if (strcmp(key, "password") == 0)
                        {
                            strlcpy(pass_val, decoded_val, sizeof(pass_val));
                        }
                    }
                    p = strtok(NULL, "&");
                }

                // Validate before touching config.txt: a bad file would only be rejected at the next boot
                const char *config_error = NULL;
                size_t ssid_len = strlen(ssid_val);
                size_t pass_len = strlen(pass_val);
                bool mode_none = (strcasecmp(mode_val, "none") == 0);
                if (strcasecmp(mode_val, "ap") != 0 && strcasecmp(mode_val, "client") != 0 && !mode_none)
                {
                    config_error = "Modo invalido (use ap, client ou none).";
                }
                else if (strpbrk(ssid_val, "\r\n") || strpbrk(pass_val, "\r\n"))
                {
                    config_error = "SSID e senha nao podem conter quebras de linha.";
                }
                else if (!mode_none && (ssid_len < 1 || ssid_len > 32))
                {
                    config_error = "O SSID deve ter de 1 a 32 caracteres.";
                }
                else if (!mode_none && pass_len != 0 && (pass_len < 8 || pass_len > 63))
                {
                    config_error = "A senha deve ficar em branco (rede aberta) ou ter de 8 a 63 caracteres.";
                }

                if (config_error != NULL)
                {
                    char err_json[200];
                    snprintf(err_json, sizeof(err_json), "{\"status\":\"error\",\"message\":\"%s\"}", config_error);
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, "400 Bad Request");
                    return httpd_resp_sendstr(req, err_json);
                }

                if (!storage::ready)
                {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Cartao MicroSD nao inicializado\"}");
                }

                char cfg_path[CONFIG_FATFS_MAX_LFN + 16];
                snprintf(cfg_path, sizeof(cfg_path), "%s/config.txt", storage::base_path);
                FILE *f = fopen(cfg_path, "w");
                if (!f)
                {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Falha ao abrir config.txt para escrita\"}");
                }
                int written = fprintf(f, "# OpenWirelessMC Configuration\nmode=%s\nssid=%s\npassword=%s\n", mode_val, ssid_val, pass_val);
                if (fclose(f) != 0 || written < 0)
                {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Falha ao gravar config.txt\"}");
                }

                storage::config::LoadConfig();

                httpd_resp_set_type(req, "application/json");
                return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
            }

            static void reboot_task(void *pvParameter)
            {
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }

            /* API Handler: POST /api/reboot */
            static esp_err_t api_reboot_handler(httpd_req_t *req)
            {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");

                xTaskCreate(reboot_task, "reboot_task", 2048, NULL, 5, NULL);
                return ESP_OK;
            }

            /* API Handler: GET /api/logs */
            static esp_err_t api_logs_handler(httpd_req_t *req)
            {
                char *log_buf = (char *)malloc(8192);
                if (!log_buf)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory for logs");
                    return ESP_FAIL;
                }

                size_t len = logging_get_logs(log_buf, 8192);
                httpd_resp_set_type(req, "text/plain; charset=utf-8");
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
                httpd_resp_send(req, log_buf, len);
                free(log_buf);
                return ESP_OK;
            }

            /* API Handler: POST /api/ota */
            static esp_err_t api_ota_handler(httpd_req_t *req)
            {
                ESP_LOGI(kLogPrefix, "OTA update initiated via Web API, content length = %d", req->content_len);
                if (req->content_len <= 0)
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
                    return ESP_FAIL;
                }

                const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
                if (!update_partition)
                {
                    ESP_LOGE(kLogPrefix, "OTA: Failed to find update partition!");
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition found");
                    return ESP_FAIL;
                }

                ESP_LOGI(kLogPrefix, "OTA: Target partition: %s (offset: 0x%x, size: 0x%x)",
                         update_partition->label, update_partition->address, update_partition->size);

                esp_ota_handle_t ota_handle = 0;
                esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
                if (err != ESP_OK)
                {
                    ESP_LOGE(kLogPrefix, "OTA: esp_ota_begin failed: 0x%x", err);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
                    return ESP_FAIL;
                }

                char *ota_buf = (char *)malloc(4096);
                if (!ota_buf)
                {
                    esp_ota_end(ota_handle);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA buffer allocation failed");
                    return ESP_FAIL;
                }

                int remaining = req->content_len;
                while (remaining > 0)
                {
                    int to_recv = (remaining < 4096) ? remaining : 4096;
                    int received = httpd_req_recv(req, ota_buf, to_recv);
                    if (received <= 0)
                    {
                        if (received == HTTPD_SOCK_ERR_TIMEOUT)
                        {
                            continue;
                        }
                        ESP_LOGE(kLogPrefix, "OTA: Receive error: %d", received);
                        free(ota_buf);
                        esp_ota_end(ota_handle);
                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA receive interrupted");
                        return ESP_FAIL;
                    }

                    err = esp_ota_write(ota_handle, (const void *)ota_buf, received);
                    if (err != ESP_OK)
                    {
                        ESP_LOGE(kLogPrefix, "OTA: esp_ota_write failed: 0x%x", err);
                        free(ota_buf);
                        esp_ota_end(ota_handle);
                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write flash failed");
                        return ESP_FAIL;
                    }
                    remaining -= received;
                }

                free(ota_buf);

                err = esp_ota_end(ota_handle);
                if (err != ESP_OK)
                {
                    ESP_LOGE(kLogPrefix, "OTA: esp_ota_end failed: 0x%x", err);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
                    return ESP_FAIL;
                }

                esp_app_desc_t new_app_info;
                err = esp_ota_get_partition_description(update_partition, &new_app_info);
                if (err != ESP_OK)
                {
                    ESP_LOGE(kLogPrefix, "OTA: Invalid app image description: 0x%x", err);
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware image header");
                    return ESP_FAIL;
                }
                ESP_LOGI(kLogPrefix, "OTA: New firmware verified -> Project: '%s', Version: '%s'",
                         new_app_info.project_name, new_app_info.version);

                err = esp_ota_set_boot_partition(update_partition);
                if (err != ESP_OK)
                {
                    ESP_LOGE(kLogPrefix, "OTA: esp_ota_set_boot_partition failed: 0x%x", err);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
                    return ESP_FAIL;
                }

                ESP_LOGI(kLogPrefix, "OTA Update SUCCEEDED! Restarting in 2 seconds...");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Atualizado com sucesso! Reiniciando...\"}");

                xTaskCreate(reboot_task, "reboot_task", 2048, NULL, 5, NULL);
                return ESP_OK;
            }

            /* Function to start the file server */
            esp_err_t start_file_server(const char *base_path)
            {
                static struct file_server_data *server_data = NULL;

                if (server_data)
                {
                    ESP_LOGE(kLogPrefix, "File server already started");
                    return ESP_ERR_INVALID_STATE;
                }

                /* Allocate memory for server data */
                server_data = (file_server_data *)calloc(1, sizeof(struct file_server_data));
                if (!server_data)
                {
                    ESP_LOGE(kLogPrefix, "Failed to allocate memory for server data");
                    return ESP_ERR_NO_MEM;
                }
                strlcpy(server_data->base_path, base_path,
                        sizeof(server_data->base_path));

                httpd_handle_t server = NULL;
                httpd_config_t config = HTTPD_DEFAULT_CONFIG();
                config.stack_size = 10240;
                config.max_uri_handlers = 36;

                // Use the URI wildcard matching function in order to
                // allow the same handler to respond to multiple different
                // target URIs which match the wildcard scheme
                config.uri_match_fn = httpd_uri_match_wildcard;

                ESP_LOGI(kLogPrefix, "Starting HTTP Server on port: '%d'", config.server_port);
                if (httpd_start(&server, &config) != ESP_OK)
                {
                    ESP_LOGE(kLogPrefix, "Failed to start file server!");
                    return ESP_FAIL;
                }

                // Register API URI handlers BEFORE wildcard /*
                httpd_uri_t api_status_uri = {
                    .uri = "/api/status",
                    .method = HTTP_GET,
                    .handler = api_status_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_status_uri);

                httpd_uri_t api_config_uri = {
                    .uri = "/api/config",
                    .method = HTTP_POST,
                    .handler = api_config_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_config_uri);

                httpd_uri_t api_card_create_uri = { .uri = "/api/card/create", .method = HTTP_POST, .handler = api_card_create_handler, .user_ctx = NULL };
                httpd_register_uri_handler(server, &api_card_create_uri);

                httpd_uri_t api_create_uri = { .uri = "/api/create", .method = HTTP_POST, .handler = api_card_create_handler, .user_ctx = NULL };
                httpd_register_uri_handler(server, &api_create_uri);

                httpd_uri_t api_card_place_uri = { .uri = "/api/card/place", .method = HTTP_POST, .handler = api_card_place_handler, .user_ctx = NULL };
                httpd_register_uri_handler(server, &api_card_place_uri);

                httpd_uri_t api_card_name_uri = { .uri = "/api/card/name", .method = HTTP_POST, .handler = api_card_name_handler, .user_ctx = NULL };
                httpd_register_uri_handler(server, &api_card_name_uri);

                httpd_uri_t api_channels_uri = { .uri = "/api/channels", .method = HTTP_GET, .handler = api_channels_get_handler, .user_ctx = NULL };
                httpd_register_uri_handler(server, &api_channels_uri);

                httpd_uri_t api_channel_uri = { .uri = "/api/channel", .method = HTTP_POST, .handler = api_channel_post_handler, .user_ctx = NULL };
                httpd_register_uri_handler(server, &api_channel_uri);

                httpd_uri_t api_create_game_uri = { .uri = "/api/create_game", .method = HTTP_POST, .handler = api_create_game_handler, .user_ctx = NULL };
                httpd_register_uri_handler(server, &api_create_game_uri);

                httpd_uri_t api_fs_list_uri = { .uri = "/api/fs/list", .method = HTTP_GET, .handler = api_fs_list_handler, .user_ctx = NULL };
                httpd_register_uri_handler(server, &api_fs_list_uri);

                httpd_uri_t api_fs_put_uri = { .uri = "/api/fs/put", .method = HTTP_POST, .handler = api_fs_put_handler, .user_ctx = NULL };
                httpd_register_uri_handler(server, &api_fs_put_uri);

                httpd_uri_t api_fs_delete_uri = { .uri = "/api/fs/delete", .method = HTTP_POST, .handler = api_fs_delete_handler, .user_ctx = NULL };
                httpd_register_uri_handler(server, &api_fs_delete_uri);

                httpd_uri_t api_rename_uri = {
                    .uri = "/api/rename",
                    .method = HTTP_POST,
                    .handler = api_rename_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_rename_uri);

                httpd_uri_t api_card_duplicate_uri = {
                    .uri = "/api/card/duplicate",
                    .method = HTTP_POST,
                    .handler = api_card_duplicate_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_card_duplicate_uri);

                httpd_uri_t api_duplicate_uri = {
                    .uri = "/api/duplicate",
                    .method = HTTP_POST,
                    .handler = api_card_duplicate_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_duplicate_uri);

                httpd_uri_t api_reboot_uri = {
                    .uri = "/api/reboot",
                    .method = HTTP_POST,
                    .handler = api_reboot_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_reboot_uri);

                httpd_uri_t api_unload_uri = {
                    .uri = "/api/unload",
                    .method = HTTP_POST,
                    .handler = api_unload_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_unload_uri);

                httpd_uri_t api_eject_sd_uri = {
                    .uri = "/api/eject_sd",
                    .method = HTTP_POST,
                    .handler = api_eject_sd_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_eject_sd_uri);

                httpd_uri_t api_mount_sd_uri = {
                    .uri = "/api/mount_sd",
                    .method = HTTP_POST,
                    .handler = api_mount_sd_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_mount_sd_uri);

                httpd_uri_t api_sio_toggle_uri = {
                    .uri = "/api/sio_toggle",
                    .method = HTTP_POST,
                    .handler = api_sio_toggle_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_sio_toggle_uri);

                httpd_uri_t api_logs_uri = {
                    .uri = "/api/logs",
                    .method = HTTP_GET,
                    .handler = api_logs_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_logs_uri);

                httpd_uri_t api_ota_uri = {
                    .uri = "/api/ota",
                    .method = HTTP_POST,
                    .handler = api_ota_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_ota_uri);

                // Specific wildcard handlers MUST be registered BEFORE the generic /* handler
                httpd_uri_t file_upload = {
                    .uri = "/upload/*",
                    .method = HTTP_POST,
                    .handler = upload_post_handler,
                    .user_ctx = server_data
                };
                httpd_register_uri_handler(server, &file_upload);

                httpd_uri_t file_delete = {
                    .uri = "/delete/*",
                    .method = HTTP_POST,
                    .handler = delete_post_handler,
                    .user_ctx = server_data
                };
                httpd_register_uri_handler(server, &file_delete);

                httpd_uri_t file_mount_post = {
                    .uri = "/mount/*",
                    .method = HTTP_POST,
                    .handler = mount_post_handler,
                    .user_ctx = server_data
                };
                httpd_register_uri_handler(server, &file_mount_post);

                httpd_uri_t file_mount_get = {
                    .uri = "/mount/*",
                    .method = HTTP_GET,
                    .handler = mount_post_handler,
                    .user_ctx = server_data
                };
                httpd_register_uri_handler(server, &file_mount_get);

                // Generic URI handler for getting files (match all remaining /* URIs)
                httpd_uri_t file_download = {
                    .uri = "/*",
                    .method = HTTP_GET,
                    .handler = download_get_handler,
                    .user_ctx = server_data
                };
                httpd_register_uri_handler(server, &file_download);

                return ESP_OK;
            }
        } // file_server

    } // web
} // openmc