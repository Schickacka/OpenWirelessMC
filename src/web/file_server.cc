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

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

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

                // Get handle to embedded file upload script (Head, CSS, Header, Navigation Tabs, Upload Card)
                extern const unsigned char upload_script_start[] asm("_binary_upload_script_html_start");
                extern const unsigned char upload_script_end[] asm("_binary_upload_script_html_end");
                const size_t upload_script_size = (upload_script_end - upload_script_start);
                httpd_resp_send_chunk(req, (const char *)upload_script_start, upload_script_size);

                // Send loaded file badge & current path navigation
                httpd_resp_sendstr_chunk(req, "<div class=\"loaded-card-bar\"><div><div class=\"loaded-label\" data-i18n=\"active_mc_label\">");
                if (strlen(storage::loaded_file_path) > 0) {
                    httpd_resp_sendstr_chunk(req, "Memory Card Ativo no Console</div><div class=\"loaded-filename\">🎮 ");
                    httpd_resp_sendstr_chunk(req, storage::loaded_file_path);
                    httpd_resp_sendstr_chunk(req, "</div></div><div><button type=\"button\" class=\"btn btn-red\" onclick=\"ejectCard()\" style=\"padding:8px 14px; font-size:12px; display:inline-flex; align-items:center; gap:6px;\"><span style=\"font-size:14px;\">⏏</span> <span data-i18n=\"btn_eject\">Desmontar (Liberar Slot)</span></button></div>");
                } else {
                    httpd_resp_sendstr_chunk(req, "Status do Slot no Console</div><div class=\"loaded-filename\"><span style=\"color:var(--text-muted); font-style:italic;\" data-i18n=\"empty_slot_desc\">📭 Nenhum Save Montado (Slot Liberado para Cartão Físico)</span></div></div>");
                }
                httpd_resp_sendstr_chunk(req, "</div>");

                httpd_resp_sendstr_chunk(req, "<div class=\"path-nav\"><span>Pasta Atual: <b>");
                httpd_resp_sendstr_chunk(req, temp_dirpath);
                httpd_resp_sendstr_chunk(req, "</b></span>");
                if (strcmp(temp_dirpath, "/sdcard") != 0 && strcmp(temp_dirpath, "/sdcard/") != 0) {
                    httpd_resp_sendstr_chunk(req, "<a href=\"../\">⬆️ Subir de Pasta (Parent)</a>");
                }
                httpd_resp_sendstr_chunk(req, "</div>");

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
                        // Ignore Windows system folder, internal config.txt, and titles database
                        if ((strcmp(entry->d_name, "System Volume Information") == 0 && entry->d_type == DT_DIR) ||
                            (strcasecmp(entry->d_name, "config.txt") == 0) ||
                            (strcasecmp(entry->d_name, "titles.db") == 0))
                        {
                            continue;
                        }

                        if (entry->d_type == DT_DIR)
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

                        bool is_dir = (entry->d_type == DT_DIR);
                        bool is_mc = (!is_dir && (IS_FILE_EXT(entry->d_name, ".mc") || IS_FILE_EXT(entry->d_name, ".mcd") || IS_FILE_EXT(entry->d_name, ".mcr")));

                        bool is_mounted = (strlen(storage::loaded_file_path) > 0 && strcmp(entrypath, storage::loaded_file_path) == 0);

                        httpd_resp_sendstr_chunk(req, "<tr><td class=\"game-col\" data-name=\"");
                        httpd_resp_sendstr_chunk(req, entry->d_name);
                        httpd_resp_sendstr_chunk(req, "\"><span class=\"game-title-text\">-</span></td><td><a href=\"");
                        httpd_resp_sendstr_chunk(req, req->uri);
                        httpd_resp_sendstr_chunk(req, entry->d_name);
                        if (is_dir)
                        {
                            httpd_resp_sendstr_chunk(req, "/\">📁 ");
                            httpd_resp_sendstr_chunk(req, entry->d_name);
                            httpd_resp_sendstr_chunk(req, "/</a></td><td><span class=\"tag tag-dir\">Pasta</span></td><td>-</td><td><a href=\"");
                            httpd_resp_sendstr_chunk(req, req->uri);
                            httpd_resp_sendstr_chunk(req, entry->d_name);
                            httpd_resp_sendstr_chunk(req, "/\" class=\"btn btn-green\">Abrir 📂</a></td></tr>\n");
                        }
                        else
                        {
                            httpd_resp_sendstr_chunk(req, "\">");
                            if (is_mc) {
                                httpd_resp_sendstr_chunk(req, "🎮 ");
                            } else {
                                httpd_resp_sendstr_chunk(req, "📄 ");
                            }
                            httpd_resp_sendstr_chunk(req, entry->d_name);
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
                            httpd_resp_sendstr_chunk(req, req->uri);
                            httpd_resp_sendstr_chunk(req, entry->d_name);
                            httpd_resp_sendstr_chunk(req, "\" download class=\"btn btn-download\"><span>⬇️</span> <span data-i18n=\"btn_download\">Baixar</span></a>");

                            if (is_mc) {
                                if (!is_mounted) {
                                    httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/mount");
                                    httpd_resp_sendstr_chunk(req, req->uri);
                                    httpd_resp_sendstr_chunk(req, entry->d_name);
                                    httpd_resp_sendstr_chunk(req, "\"><button type=\"submit\" class=\"btn btn-green\">▲ Montar</button></form>");

                                    httpd_resp_sendstr_chunk(req, "<button type=\"button\" class=\"btn btn-grey\" onclick=\"renameCard('");
                                    httpd_resp_sendstr_chunk(req, entrypath);
                                    httpd_resp_sendstr_chunk(req, "', '");
                                    httpd_resp_sendstr_chunk(req, entry->d_name);
                                    httpd_resp_sendstr_chunk(req, "')\" style=\"padding:5px 9px; font-size:11px;\">✏️ Renomear</button>");
                                }
                            }

                            if (!is_mounted) {
                                httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/delete");
                                httpd_resp_sendstr_chunk(req, req->uri);
                                httpd_resp_sendstr_chunk(req, entry->d_name);
                                httpd_resp_sendstr_chunk(req, "\"><button type=\"submit\" class=\"btn btn-red\" onclick=\"return confirm('Deseja excluir este save permanentemente?')\">● Excluir</button></form>");
                            } else {
                                httpd_resp_sendstr_chunk(req, "<span style=\"font-size:11px; color:var(--ps-green); font-weight:600;\">🔒 Em Uso</span>");
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
                /* This is a limited set only */
                /* For any other type always set as plain text */
                return httpd_resp_set_type(req, "text/plain");
            }

            /* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
            static const char *get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
            {
                char decoded_uri[513];
                web::PercentDecode(uri, decoded_uri);
                web::PercentDecode(base_path, dest);
                const size_t base_pathlen = strlen(dest);
                size_t pathlen = strlen(decoded_uri);

                const char *quest = strchr(decoded_uri, '?');
                if (quest)
                {
                    pathlen = MIN(pathlen, quest - decoded_uri);
                }
                const char *hash = strchr(decoded_uri, '#');
                if (hash)
                {
                    pathlen = MIN(pathlen, hash - decoded_uri);
                }

                if (base_pathlen + pathlen + 1 > destsize)
                {
                    /* Full path string won't fit into destination buffer */
                    return NULL;
                }

                /* Construct full path (base + path) */
                //strcpy(dest, base_path);
                strlcpy(dest + base_pathlen, decoded_uri, pathlen + 1);

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

                /* If name has trailing '/', respond with directory contents */
                if (filename[strlen(filename) - 1] == '/')
                {
                    return http_resp_dir_html(req, filepath);
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

                fd = fopen(filepath, "r");
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

            /* Handler to upload a file onto the server */
            static esp_err_t upload_post_handler(httpd_req_t *req)
            {
                char filepath[FILE_PATH_MAX];
                FILE *fd = NULL;
                struct stat file_stat;

                char redirect_path[FILE_PATH_MAX];
                char *end_of_redirect_path;

                /* Skip leading "/upload" from URI to get filename */
                /* Note sizeof() counts NULL termination hence the -1 */
                const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                                         req->uri + sizeof("/upload") - 1, sizeof(filepath));
                //URLDecode(filepath);
                ets_printf("Upload filename: %s, filepath = %s\n", filename, filepath);
                if (!filename)
                {
                    /* Respond with 500 Internal Server Error */
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
                    return ESP_FAIL;
                }

                /* Filename cannot have a trailing '/' */
                if (filename[strlen(filename) - 1] == '/')
                {
                    ESP_LOGE(kLogPrefix, "Invalid filename : %s", filename);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
                    return ESP_FAIL;
                }

                if (stat(filepath, &file_stat) == 0)
                {
                    ESP_LOGE(kLogPrefix, "File already exists : %s", filepath);
                    /* Respond with 400 Bad Request */
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File already exists");
                    return ESP_FAIL;
                }

                /* File cannot be larger than a limit */
                if (req->content_len > MAX_FILE_SIZE)
                {
                    ESP_LOGE(kLogPrefix, "File too large : %d bytes", req->content_len);
                    /* Respond with 400 Bad Request */
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                        "File size must be less than " MAX_FILE_SIZE_STR "!");
                    /* Return failure to close underlying connection else the
         * incoming file content will keep the socket busy */
                    return ESP_FAIL;
                }

                fd = fopen(filepath, "w");
                if (!fd)
                {
                    ESP_LOGE(kLogPrefix, "Failed to create file : %s", filepath);
                    /* Respond with 500 Internal Server Error */
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
                    return ESP_FAIL;
                }

                //ESP_LOGI(kLogPrefix, "Receiving file : %s...", filename);

                /* Retrieve the pointer to scratch buffer for temporary storage */
                char *buf = ((struct file_server_data *)req->user_ctx)->scratch;
                int received;

                /* Content length of the request gives
     * the size of the file being uploaded */
                int remaining = req->content_len;

                while (remaining > 0)
                {

                    //ESP_LOGI(kLogPrefix, "Remaining size : %d", remaining);
                    /* Receive the file part by part into a buffer */
                    if ((received = httpd_req_recv(req, buf, MIN(remaining, SCRATCH_BUFSIZE))) <= 0)
                    {
                        if (received == HTTPD_SOCK_ERR_TIMEOUT)
                        {
                            /* Retry if timeout occurred */
                            continue;
                        }

                        /* In case of unrecoverable error,
             * close and delete the unfinished file*/
                        fclose(fd);
                        unlink(filepath);

                        ESP_LOGE(kLogPrefix, "File reception failed!");
                        /* Respond with 500 Internal Server Error */
                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
                        return ESP_FAIL;
                    }

                    /* Write buffer content to file on storage */
                    if (received && (received != fwrite(buf, 1, received, fd)))
                    {
                        /* Couldn't write everything to file!
             * Storage may be full? */
                        fclose(fd);
                        unlink(filepath);

                        ESP_LOGE(kLogPrefix, "File write failed!");
                        /* Respond with 500 Internal Server Error */
                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to storage");
                        return ESP_FAIL;
                    }

                    /* Keep track of remaining size of
         * the file left to be uploaded */
                    remaining -= received;
                }

                /* Close file upon upload completion */
                fclose(fd);
                //ESP_LOGI(kLogPrefix, "File reception complete");

                strlcpy(redirect_path, filename, sizeof(redirect_path));
                end_of_redirect_path = strrchr(redirect_path, '/');
                if (end_of_redirect_path)
                {
                    end_of_redirect_path[1] = '\0';
                }

                // Redirect onto root to see the updated file list
                httpd_resp_set_status(req, "303 See Other");
                httpd_resp_set_hdr(req, "Location", redirect_path);
                httpd_resp_sendstr(req, "File uploaded successfully");
                return ESP_OK;
            }

            /* Handler to delete a file from the server */
            static esp_err_t delete_post_handler(httpd_req_t *req)
            {
                char filepath[FILE_PATH_MAX];
                char redirect_path[FILE_PATH_MAX];
                char *end_of_redirect_path;
                struct stat file_stat;

                /* Skip leading "/delete" from URI to get filename */
                /* Note sizeof() counts NULL termination hence the -1 */
                const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                                         req->uri + sizeof("/delete") - 1, sizeof(filepath));
                //URLDecode(filepath);
                if (!filename)
                {
                    /* Respond with 500 Internal Server Error */
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
                    return ESP_FAIL;
                }

                /* Filename cannot have a trailing '/' */
                if (filename[strlen(filename) - 1] == '/')
                {
                    ESP_LOGE(kLogPrefix, "Invalid filename : %s", filename);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
                    return ESP_FAIL;
                }

                if (stat(filepath, &file_stat) == -1)
                {
                    ESP_LOGE(kLogPrefix, "File does not exist : %s", filename);
                    // Respond with 400 Bad Request
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File does not exist");
                    return ESP_FAIL;
                }

                //ESP_LOGI(kLogPrefix, "Deleting file : %s", filename);
                // Delete file
                unlink(filepath);

                strlcpy(redirect_path, filename, sizeof(redirect_path));
                end_of_redirect_path = strrchr(redirect_path, '/');
                if (end_of_redirect_path)
                {
                    end_of_redirect_path[1] = '\0';
                }

                // Redirect to directory to see the updated file list
                httpd_resp_set_status(req, "303 See Other");
                httpd_resp_set_hdr(req, "Location", redirect_path);
                httpd_resp_sendstr(req, "File deleted successfully");
                return ESP_OK;
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
                    gui::WakeDisplay();
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

            /* API Handler: POST /api/create */
            static esp_err_t api_create_handler(httpd_req_t *req)
            {
                char buf[128];
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

                char filename[64] = "";
                char *eq = strchr(buf, '=');
                if (eq)
                {
                    char decoded[64];
                    web::PercentDecode(eq + 1, decoded);
                    strlcpy(filename, decoded, sizeof(filename));
                }
                else
                {
                    strlcpy(filename, buf, sizeof(filename));
                }

                // Check for invalid characters
                if (strlen(filename) == 0 || strpbrk(filename, "/\\:*?\"<>| "))
                {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, "400 Bad Request");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Nome invalido! Nao use espacos ou caracteres especiais (/ \\ : * ? \\\" < > |)\"}");
                }

                if (!strchr(filename, '.'))
                {
                    strlcat(filename, ".mc", sizeof(filename));
                }

                char target_path[FILE_PATH_MAX + 1];
                snprintf(target_path, sizeof(target_path), "/sdcard/%s", filename);

                if (storage::CreateBlankFormattedCard(target_path))
                {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
                    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
                }
                else
                {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Falha ao criar Memory Card virgem\"}");
                }
            }

            /* API Handler: POST /api/unload (Eject Memory Card) */
            static esp_err_t api_unload_handler(httpd_req_t *req)
            {
                storage::UnloadCard();
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
                return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"quote\":\"I can read your mind!\"}");
            }

            /* API Handler: POST /api/eject_sd (Safely Eject SD Card) */
            static esp_err_t api_eject_sd_handler(httpd_req_t *req)
            {
                storage::SafelyUnmountSD();
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
                return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Cartao MicroSD ejetado com seguranca! Pronto para remocao fisica.\"}");
            }

            /* API Handler: POST /api/mount_sd (Remount SD Card) */
            static esp_err_t api_mount_sd_handler(httpd_req_t *req)
            {
                bool ok = storage::RemountSD();
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
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
                sio::SetMemoryCardEmulation(new_state);

                ESP_LOGI(kLogPrefix, "SIO Mode toggled: %s", new_state ? "EMULATION ACTIVE" : "PASSTHROUGH / PHYSICAL CARD");

                char resp[128];
                snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"emulation_active\":%s}",
                         new_state ? "true" : "false");

                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
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

                char old_path[FILE_PATH_MAX + 1] = "";
                char new_name[64] = "";

                char *p = strtok(buf, "&");
                while (p != NULL)
                {
                    char *eq = strchr(p, '=');
                    if (eq)
                    {
                        *eq = '\0';
                        char *key = p;
                        char *val = eq + 1;
                        char decoded_val[FILE_PATH_MAX + 1];
                        web::PercentDecode(val, decoded_val);
                        if (strcmp(key, "old_path") == 0)
                        {
                            strlcpy(old_path, decoded_val, sizeof(old_path));
                        }
                        else if (strcmp(key, "new_name") == 0)
                        {
                            strlcpy(new_name, decoded_val, sizeof(new_name));
                        }
                    }
                    p = strtok(NULL, "&");
                }

                if (strlen(old_path) == 0 || strlen(new_name) == 0)
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing parameters");
                    return ESP_FAIL;
                }

                if (strpbrk(new_name, "/\\:*?\"<>| "))
                {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, "400 Bad Request");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Novo nome invalido! Nao use espacos ou caracteres especiais (/ \\ : * ? \\\" < > |)\"}");
                }

                if (strcmp(old_path, storage::loaded_file_path) == 0)
                {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, "400 Bad Request");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Nao e possivel renomear o Memory Card montado no console!\"}");
                }

                char new_path[FILE_PATH_MAX + 1];
                char *slash = strrchr(old_path, '/');
                if (slash)
                {
                    int dir_len = slash - old_path + 1;
                    strncpy(new_path, old_path, dir_len);
                    new_path[dir_len] = '\0';
                    strlcat(new_path, new_name, sizeof(new_path));
                }
                else
                {
                    snprintf(new_path, sizeof(new_path), "/sdcard/%s", new_name);
                }

                if (rename(old_path, new_path) == 0)
                {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
                    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
                }
                else
                {
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Falha ao renomear arquivo\"}");
                }
            }

            /* API Handler: GET /api/status */
            static esp_err_t api_status_handler(httpd_req_t *req)
            {
                uint64_t total_bytes = 0;
                uint64_t free_bytes = 0;
                FATFS *fs = NULL;
                DWORD fre_clust = 0;
                if (f_getfree("0:", &fre_clust, &fs) == FR_OK && fs != NULL)
                {
                    uint64_t tot_sect = (uint64_t)(fs->n_fatent - 2) * fs->csize;
                    uint64_t fre_sect = (uint64_t)fre_clust * fs->csize;
#if FF_MAX_SS != FF_MIN_SS
                    total_bytes = tot_sect * fs->ssize;
                    free_bytes = fre_sect * fs->ssize;
#else
                    total_bytes = tot_sect * FF_SS;
                    free_bytes = fre_sect * FF_SS;
#endif
                }

                int8_t rssi = 0;
                wifi_ap_record_t ap_info;
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
                {
                    rssi = ap_info.rssi;
                }

                int ps1_count = 0;
                int ps2_count = 0;
                DIR *d = opendir("/sdcard");
                if (d)
                {
                    struct dirent *de;
                    while ((de = readdir(d)) != NULL)
                    {
                        const char *dot = strrchr(de->d_name, '.');
                        if (dot && (strcasecmp(dot, ".mc") == 0 || strcasecmp(dot, ".mcd") == 0 || strcasecmp(dot, ".mcr") == 0))
                        {
                            char fpath[FILE_PATH_MAX + 32];
                            strlcpy(fpath, "/sdcard/", sizeof(fpath));
                            strlcat(fpath, de->d_name, sizeof(fpath));
                            struct stat st;
                            if (stat(fpath, &st) == 0 && st.st_size > (128 * 1024))
                            {
                                ps2_count++;
                            }
                            else
                            {
                                ps1_count++;
                            }
                        }
                    }
                    closedir(d);
                }

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

                char json_resp[768];
                snprintf(json_resp, sizeof(json_resp),
                    "{\"sd_total\":%llu,\"sd_free\":%llu,\"sd_mounted\":%s,\"heap_free\":%u,\"heap_min\":%u,\"ram_total\":%u,"
                    "\"wifi_mode\":%d,\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\","
                    "\"loaded_file\":\"%s\",\"uptime\":%lld,\"mc_count\":%d,\"ps1_count\":%d,\"ps2_count\":%d,"
                    "\"chip\":\"%s\",\"cores\":%d,\"cpu_freq\":%u,\"flash_size\":%u,\"sio_emulation\":%s}",
                    (unsigned long long)total_bytes, (unsigned long long)free_bytes,
                    storage::ready ? "true" : "false",
                    (unsigned int)esp_get_free_heap_size(), (unsigned int)esp_get_minimum_free_heap_size(),
                    (unsigned int)ram_total,
                    (int)system::settings.wifi_mode, system::settings.ssid, (int)rssi, ip_str,
                    storage::loaded_file_path,
                    (long long)(esp_timer_get_time() / 1000000ULL),
                    (ps1_count + ps2_count), ps1_count, ps2_count,
                    chip_desc, (int)chip_info.cores, (unsigned int)freq_cfg.freq_mhz, (unsigned int)(flash_size / (1024 * 1024)),
                    sio::memory_card_enabled ? "true" : "false");

                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
                return httpd_resp_send(req, json_resp, strlen(json_resp));
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
                char ssid_val[64] = "";
                char pass_val[64] = "";

                char *p = strtok(buf, "&");
                while (p != NULL)
                {
                    char *eq = strchr(p, '=');
                    if (eq)
                    {
                        *eq = '\0';
                        char *key = p;
                        char *val = eq + 1;
                        char decoded_val[64];
                        web::PercentDecode(val, decoded_val);
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

                FILE *f = fopen("/sdcard/config.txt", "w");
                if (!f)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write config.txt");
                    return ESP_FAIL;
                }
                fprintf(f, "# OpenWirelessMC Configuration\nmode=%s\nssid=%s\npassword=%s\n", mode_val, ssid_val, pass_val);
                fclose(f);

                storage::config::LoadConfig();

                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
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
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
                httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");

                xTaskCreate(reboot_task, "reboot_task", 2048, NULL, 5, NULL);
                return ESP_OK;
            }

            /* API Handler: GET /api/logs */
            static esp_err_t api_logs_handler(httpd_req_t *req)
            {
                char *log_buf = (char *)malloc(16384);
                if (!log_buf)
                {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory for logs");
                    return ESP_FAIL;
                }

                size_t len = logging_get_logs(log_buf, 16384);
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

                err = esp_ota_set_boot_partition(update_partition);
                if (err != ESP_OK)
                {
                    ESP_LOGE(kLogPrefix, "OTA: esp_ota_set_boot_partition failed: 0x%x", err);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
                    return ESP_FAIL;
                }

                ESP_LOGI(kLogPrefix, "OTA Update SUCCEEDED! Restarting in 2 seconds...");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
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
                config.max_uri_handlers = 20;

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

                httpd_uri_t api_create_uri = {
                    .uri = "/api/create",
                    .method = HTTP_POST,
                    .handler = api_create_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_create_uri);

                httpd_uri_t api_rename_uri = {
                    .uri = "/api/rename",
                    .method = HTTP_POST,
                    .handler = api_rename_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_rename_uri);

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

                // URI handler for getting uploaded files
                httpd_uri_t file_download = {
                    .uri = "/*", // Match all URIs of type /path/to/file
                    .method = HTTP_GET,
                    .handler = download_get_handler,
                    .user_ctx = server_data // Pass server data as context
                };
                httpd_register_uri_handler(server, &file_download);

                // URI handler for uploading files to server
                httpd_uri_t file_upload = {
                    .uri = "/upload/*", // Match all URIs of type /upload/path/to/file
                    .method = HTTP_POST,
                    .handler = upload_post_handler,
                    .user_ctx = server_data // Pass server data as context
                };
                httpd_register_uri_handler(server, &file_upload);

                // URI handler for deleting files from server
                httpd_uri_t file_delete = {
                    .uri = "/delete/*", // Match all URIs of type /delete/path/to/file
                    .method = HTTP_POST,
                    .handler = delete_post_handler,
                    .user_ctx = server_data // Pass server data as context
                };
                httpd_register_uri_handler(server, &file_delete);

                httpd_uri_t file_mount = {
                    .uri = "/mount/*", // Match all URIs of type /delete/path/to/file
                    .method = HTTP_POST,
                    .handler = mount_post_handler,
                    .user_ctx = server_data // Pass server data as context
                };
                httpd_register_uri_handler(server, &file_mount);

                return ESP_OK;
            }
        } // file_server

    } // web
} // openmc