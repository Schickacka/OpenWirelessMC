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
#include "ff.h"

/* Max length a file path can have on storage */
#define FILE_PATH_MAX CONFIG_FATFS_MAX_LFN

/* Max size of an individual file. Make sure this
 * value is same as that set in upload_script.html */
#define MAX_FILE_SIZE (128 * 1024) // 128 KB
#define MAX_FILE_SIZE_STR "128KB"

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

                if (!dir)
                {
                    ESP_LOGE(kLogPrefix, "Failed to stat dir : %s", temp_dirpath);
                    /* Respond with 404 Not Found */
                    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory does not exist");
                    ESP_LOGE(kLogPrefix, "Failed to check dir %s\n", temp_dirpath);
                    return ESP_FAIL;
                }

                // Get handle to embedded file upload script (Head, CSS, Header, Navigation Tabs, Upload Card)
                extern const unsigned char upload_script_start[] asm("_binary_upload_script_html_start");
                extern const unsigned char upload_script_end[] asm("_binary_upload_script_html_end");
                const size_t upload_script_size = (upload_script_end - upload_script_start);
                httpd_resp_send_chunk(req, (const char *)upload_script_start, upload_script_size);

                // Send loaded file badge & current path navigation
                httpd_resp_sendstr_chunk(req, "<div class=\"loaded-card-bar\"><div><div class=\"loaded-label\">Memory Card Ativo no Console</div><div class=\"loaded-filename\">🎮 ");
                httpd_resp_sendstr_chunk(req, storage::loaded_file_path);
                httpd_resp_sendstr_chunk(req, "</div></div></div>");

                httpd_resp_sendstr_chunk(req, "<div class=\"path-nav\"><span>Pasta Atual: <b>");
                httpd_resp_sendstr_chunk(req, temp_dirpath);
                httpd_resp_sendstr_chunk(req, "</b></span>");
                if (strcmp(temp_dirpath, "/sdcard") != 0 && strcmp(temp_dirpath, "/sdcard/") != 0) {
                    httpd_resp_sendstr_chunk(req, "<a href=\"../\">⬆️ Subir de Pasta (Parent)</a>");
                }
                httpd_resp_sendstr_chunk(req, "</div>");

                httpd_resp_sendstr_chunk(req, "<div class=\"table-wrapper\"><table class=\"ps-table\"><thead><tr><th>Nome</th><th>Tipo</th><th>Tamanho</th><th>Ações</th></tr></thead><tbody>");

                /* Iterate over all files / folders and fetch their names and sizes */
                while ((entry = readdir(dir)) != NULL)
                {
                    // Ignore these stupid folders created by Windows
                    if (strcmp(entry->d_name, "System Volume Information") == 0 && entry->d_type == DT_DIR)
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
                    //ESP_LOGI(kLogPrefix, "entrypath = %s\n", entrypath);
                    if (stat(entrypath, &entry_stat) == -1)
                    {
                        ESP_LOGE(kLogPrefix, "Failed to stat %s : %s", entrytype, entry->d_name);
                        continue;
                    }
                    sprintf(entrysize, "%ld", entry_stat.st_size);
                    //ESP_LOGI(kLogPrefix, "Found %s : %s (%s bytes)", entrytype, entry->d_name, entrysize);

                    bool is_dir = (entry->d_type == DT_DIR);
                    bool is_mc = (!is_dir && (IS_FILE_EXT(entry->d_name, ".mc") || IS_FILE_EXT(entry->d_name, ".mcd") || IS_FILE_EXT(entry->d_name, ".mcr")));

                    httpd_resp_sendstr_chunk(req, "<tr><td><a href=\"");
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
                            httpd_resp_sendstr_chunk(req, "<span class=\"tag tag-mc\">Save PS1</span>");
                        } else {
                            httpd_resp_sendstr_chunk(req, "<span class=\"tag tag-file\">Arquivo</span>");
                        }
                        httpd_resp_sendstr_chunk(req, "</td><td>");
                        httpd_resp_sendstr_chunk(req, entrysize);
                        httpd_resp_sendstr_chunk(req, " B</td><td><div class=\"actions-cell\">");
                        
                        if (is_mc) {
                            httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/mount");
                            httpd_resp_sendstr_chunk(req, req->uri);
                            httpd_resp_sendstr_chunk(req, entry->d_name);
                            httpd_resp_sendstr_chunk(req, "\"><button type=\"submit\" class=\"btn btn-green\">▲ Montar</button></form>");
                        }

                        httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/delete");
                        httpd_resp_sendstr_chunk(req, req->uri);
                        httpd_resp_sendstr_chunk(req, entry->d_name);
                        httpd_resp_sendstr_chunk(req, "\"><button type=\"submit\" class=\"btn btn-red\" onclick=\"return confirm('Deseja excluir este save permanentemente?')\">● Excluir</button></form>");
                        httpd_resp_sendstr_chunk(req, "</div></td></tr>\n");
                    }
                }
                closedir(dir);

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

                int mc_count = 0;
                DIR *d = opendir("/sdcard");
                if (d)
                {
                    struct dirent *de;
                    while ((de = readdir(d)) != NULL)
                    {
                        const char *dot = strrchr(de->d_name, '.');
                        if (dot && (strcasecmp(dot, ".mc") == 0 || strcasecmp(dot, ".mcd") == 0 || strcasecmp(dot, ".mcr") == 0))
                        {
                            mc_count++;
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

                char json_resp[512];
                snprintf(json_resp, sizeof(json_resp),
                    "{\"sd_total\":%llu,\"sd_free\":%llu,\"heap_free\":%u,\"heap_min\":%u,"
                    "\"wifi_mode\":%d,\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\","
                    "\"loaded_file\":\"%s\",\"uptime\":%lld,\"mc_count\":%d}",
                    (unsigned long long)total_bytes, (unsigned long long)free_bytes,
                    (unsigned int)esp_get_free_heap_size(), (unsigned int)esp_get_minimum_free_heap_size(),
                    (int)system::settings.wifi_mode, system::settings.ssid, (int)rssi, ip_str,
                    storage::loaded_file_path,
                    (long long)(esp_timer_get_time() / 1000000ULL),
                    mc_count);

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
                fprintf(f, "# OpenMC Configuration\nmode=%s\nssid=%s\npassword=%s\n", mode_val, ssid_val, pass_val);
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
                config.max_uri_handlers = 12;

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

                httpd_uri_t api_reboot_uri = {
                    .uri = "/api/reboot",
                    .method = HTTP_POST,
                    .handler = api_reboot_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &api_reboot_uri);

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