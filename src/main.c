#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <curl/curl.h>
#include <shlobj.h>
#include "resource.h"
#include "ai_agent.h"
#include "db_migration.h"
#include "config.h"
#include "lang.h"

#define LOG_BUFFER_SIZE         131072
#define ID_URL_EDIT             101
#define ID_FOLDER_EDIT          102
#define ID_BROWSE_BTN           103
#define ID_DEFAULT_BTN          104
#define ID_OVERWRITE_CHK        105
#define ID_START_BTN            106
#define ID_STOP_BTN             107
#define ID_LOG_EDIT             108
#define ID_SITE_STATUS          109
#define ID_CHAPTER_FILTER_EDIT  110
#define ID_ONLY_MISSING_CHK     111
#define ID_LANG_COMBO           115
#define ID_MAIN_TAB             150

#include "scrapers/scrapers.h"

#define DL_SUCCESS              0
#define DL_SKIPPED              10
#define DL_ABORTED              4
#define DL_ERR_INIT             1
#define DL_ERR_FILE             2
#define DL_ERR_PERFORM          3

typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

static wchar_t log_buffer[LOG_BUFFER_SIZE];
static HWND hUrlEdit;
static HWND hSiteStatus;
static HWND hFolderEdit;
static HWND hChapterFilterEdit;
static HWND hOnlyMissingChk;
static HWND hOverwriteChk;
static HWND hStartBtn;
static HWND hStopBtn;
static HWND hLogEdit;
static HWND hLblUrl = NULL;
static HWND hLblFolder = NULL;
static HWND hBrowseBtn = NULL;
static HWND hDefBtn = NULL;
static HWND hLblFilter = NULL;
static HWND hLblLog = NULL;
static HWND hLblLang = NULL;
static HWND hLangCombo = NULL;

static HWND hMainTab = NULL;
static HWND g_home_controls[32];
static int g_home_ctrl_count = 0;

static void show_home_controls(bool show) {
    int cmd = show ? SW_SHOW : SW_HIDE;
    for (int i = 0; i < g_home_ctrl_count; i++) {
        if (g_home_controls[i]) ShowWindow(g_home_controls[i], cmd);
    }
}

static void switch_main_tab(int tab_index) {
    if (tab_index == 0) {
        ai_agent_switch_tab(0);
        show_home_controls(true);
    } else {
        show_home_controls(false);
        ai_agent_switch_tab(tab_index);
    }
}

static volatile bool is_downloading = false;
static volatile bool stop_requested = false;
static bool overwrite = false;
static bool only_missing = false;
static wchar_t folder_path[MAX_PATH];
static char base_url[1024];
static char chapter_filter[512];
static HANDLE hDownloadThread = NULL;

static size_t write_memory_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->memory = ptr;
    memcpy(mem->memory + mem->size, contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = '\0';
    return realsize;
}

static size_t write_file_cb(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

static void append_log(const char *msg) {
    wchar_t wmsg[2048];
    MultiByteToWideChar(CP_UTF8, 0, msg, -1, wmsg, 2048);

    size_t cur_len = wcslen(log_buffer);
    size_t add_len = wcslen(wmsg) + 2;
    if (cur_len + add_len >= LOG_BUFFER_SIZE - 2) {
        memmove(log_buffer, log_buffer + (LOG_BUFFER_SIZE / 2), (LOG_BUFFER_SIZE / 2) * sizeof(wchar_t));
        log_buffer[LOG_BUFFER_SIZE / 2] = L'\0';
    }
    wcscat(log_buffer, wmsg);
    wcscat(log_buffer, L"\r\n");
    SetWindowTextW(hLogEdit, log_buffer);
    SendMessageW(hLogEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(hLogEdit, EM_SCROLLCARET, 0, 0);
}

void get_base_host(const char *url, char *host_out, size_t max_len) {
    host_out[0] = '\0';
    const char *p = strstr(url, "://");
    if (!p) return;
    p += 3;
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - url) : strlen(url);
    if (len >= max_len) len = max_len - 1;
    memcpy(host_out, url, len);
    host_out[len] = '\0';
}

static void update_site_status_ui(void) {
    if (!hUrlEdit || !hSiteStatus) return;
    wchar_t url_w[1024];
    GetWindowTextW(hUrlEdit, url_w, 1024);
    if (wcslen(url_w) == 0) {
        SetWindowTextW(hSiteStatus, _TW("str_site_status_placeholder"));
        return;
    }

    char url_a[1024];
    WideCharToMultiByte(CP_UTF8, 0, url_w, -1, url_a, 1024, NULL, NULL);

    const Scraper *scraper = find_scraper(url_a);
    wchar_t status_msg[512];
    if (scraper) {
        wchar_t sname_w[256];
        MultiByteToWideChar(CP_UTF8, 0, scraper->display_name, -1, sname_w, 256);
        _snwprintf(status_msg, 512, _TW("str_site_status_supported"), sname_w, sname_w);
    } else {
        _snwprintf(status_msg, 512, L"%s", _TW("str_site_status_unknown"));
    }

    SetWindowTextW(hSiteStatus, status_msg);
}

char *fetch_url(const char *url, const char *referer) {
    if (!url || url[0] == '\0') return NULL;

    char cookie_path[MAX_PATH];
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t *p_slash = wcsrchr(exePath, L'\\');
    if (p_slash) *(p_slash + 1) = L'\0';
    wcscat(exePath, L"cookies.txt");
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, cookie_path, MAX_PATH, NULL, NULL);

    int max_retries = 2;
    for (int attempt = 1; attempt <= max_retries; attempt++) {
        CURL *curl = curl_easy_init();
        if (!curl) return NULL;

        MemoryStruct chunk = { malloc(1), 0 };
        if (!chunk.memory) {
            curl_easy_cleanup(curl);
            return NULL;
        }
        chunk.memory[0] = '\0';

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // Auto decompress gzip/deflate/br/zstd

        // Persistent cookie support (critical for Cloudflare session preservation)
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_path);
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookie_path);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
        headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8");
        headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9,id;q=0.8");
        headers = curl_slist_append(headers, "sec-ch-ua: \"Chromium\";v=\"124\", \"Google Chrome\";v=\"124\", \"Not-A.Brand\";v=\"99\"");
        headers = curl_slist_append(headers, "sec-ch-ua-mobile: ?0");
        headers = curl_slist_append(headers, "sec-ch-ua-platform: \"Windows\"");
        headers = curl_slist_append(headers, "sec-fetch-dest: document");
        headers = curl_slist_append(headers, "sec-fetch-mode: navigate");
        headers = curl_slist_append(headers, "sec-fetch-site: same-origin");
        headers = curl_slist_append(headers, "upgrade-insecure-requests: 1");

        if (referer && referer[0] != '\0') {
            curl_easy_setopt(curl, CURLOPT_REFERER, referer);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK && chunk.memory) {
            bool is_cf_challenge = false;
            if (http_code == 403 || http_code == 503) {
                if (strstr(chunk.memory, "cf-turnstile") != NULL ||
                    strstr(chunk.memory, "Just a moment...") != NULL ||
                    strstr(chunk.memory, "challenge-platform") != NULL ||
                    strstr(chunk.memory, "Cloudflare") != NULL) {
                    is_cf_challenge = true;
                }
            }

            if (is_cf_challenge) {
                char cf_log[512];
                snprintf(cf_log, sizeof(cf_log),
                         _T("str_log_cf_challenge"),
                         http_code, attempt, max_retries);
                append_log(cf_log);

                if (attempt < max_retries) {
                    free(chunk.memory);
                    Sleep(1500);
                    continue;
                }
            } else if (http_code == 200) {
                if (strstr(chunk.memory, "cf-ray") != NULL || strstr(chunk.memory, "cloudflare") != NULL) {
                    append_log(_T("str_log_cf_bypass"));
                }
            }

            return chunk.memory;
        }

        free(chunk.memory);
        if (attempt < max_retries) Sleep(1000);
    }

    return NULL;
}


void resolve_url(const char *base_host, const char *raw_url, char *out_url, size_t max_len) {
    if (strncmp(raw_url, "http://", 7) == 0 || strncmp(raw_url, "https://", 8) == 0) {
        snprintf(out_url, max_len, "%s", raw_url);
    } else if (strncmp(raw_url, "//", 2) == 0) {
        snprintf(out_url, max_len, "https:%s", raw_url);
    } else if (raw_url[0] == '/') {
        snprintf(out_url, max_len, "%s%s", base_host, raw_url);
    } else {
        snprintf(out_url, max_len, "%s/%s", base_host, raw_url);
    }
}

void sanitize_filename(char *name) {
    for (size_t i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\r' || c == '\n') {
            name[i] = '_';
        }
    }
    size_t len = strlen(name);
    while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '.' || name[len - 1] == '_')) {
        name[--len] = '\0';
    }
}

void trim_whitespace(char *str) {
    char *p = str;
    while (isspace((unsigned char)*p)) p++;
    if (p != str) memmove(str, p, strlen(p) + 1);
    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) {
        str[--len] = '\0';
    }
}

// Parses chapter selection string, supporting single values and ranges e.g. "1, 2-5, 116-end"
static bool is_chapter_selected(int chapter_num, int chapter_index_1based, const char *filter_str) {
    if (!filter_str) return true;

    bool has_content = false;
    for (const char *s = filter_str; *s; s++) {
        if (!isspace((unsigned char)*s)) {
            has_content = true;
            break;
        }
    }
    // Empty filter means download all chapters
    if (!has_content) return true;

    char buf[1024];
    strncpy(buf, filter_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = strtok(buf, ",;");
    while (token) {
        while (isspace((unsigned char)*token)) token++;
        char *end = token + strlen(token) - 1;
        while (end >= token && isspace((unsigned char)*end)) {
            *end = '\0';
            end--;
        }

        if (*token != '\0') {
            char *dash = strchr(token, '-');
            if (dash) {
                // Range, e.g. "2-5" or "116-end" or "116-"
                *dash = '\0';
                char *left = token;
                char *right = dash + 1;
                trim_whitespace(left);
                trim_whitespace(right);

                int r_start = 1;
                if (*left != '\0') {
                    r_start = atoi(left);
                }

                int r_end = 9999999;
                char low_right[64] = "";
                size_t rlen = strlen(right);
                if (rlen >= sizeof(low_right)) rlen = sizeof(low_right) - 1;
                for (size_t k = 0; k < rlen; k++) low_right[k] = (char)tolower((unsigned char)right[k]);
                low_right[rlen] = '\0';

                if (strcmp(low_right, "end") == 0 ||
                    strcmp(low_right, "last") == 0 ||
                    strcmp(low_right, "akhir") == 0 ||
                    *right == '\0') {
                    r_end = 9999999;
                } else {
                    r_end = atoi(right);
                }

                if (r_start > r_end && r_end != 9999999) {
                    int tmp = r_start;
                    r_start = r_end;
                    r_end = tmp;
                }

                if ((chapter_num >= r_start && chapter_num <= r_end) ||
                    (chapter_index_1based >= r_start && chapter_index_1based <= r_end)) {
                    return true;
                }
            } else {
                // Single chapter, e.g. "24"
                int target = atoi(token);
                if (chapter_num == target || chapter_index_1based == target) {
                    return true;
                }
            }
        }
        token = strtok(NULL, ",;");
    }

    return false;
}

int extract_chapter_number_from_string(const char *str) {
    if (!str || str[0] == '\0') return -1;

    char low[512];
    size_t len = strlen(str);
    if (len >= sizeof(low)) len = sizeof(low) - 1;
    for (size_t i = 0; i < len; i++) low[i] = (char)tolower((unsigned char)str[i]);
    low[len] = '\0';

    // 1. Look for "chapter", "chap", "ch.", "ch-", "ch ", "ch_"
    const char *p = strstr(low, "chapter");
    if (!p) p = strstr(low, "chap");
    if (!p) p = strstr(low, "ch.");
    if (!p) p = strstr(low, "ch ");
    if (!p) p = strstr(low, "ch-");
    if (!p) p = strstr(low, "ch_");

    if (p) {
        while (*p && !isdigit((unsigned char)*p)) p++;
        if (*p && isdigit((unsigned char)*p)) {
            return atoi(p);
        }
    }

    // 2. Check if string starts with digits or separator + digits (e.g. "001", "1", "0001", "1-eng-li")
    const char *s = low;
    while (*s && (isspace((unsigned char)*s) || *s == '_' || *s == '-')) s++;
    if (*s && isdigit((unsigned char)*s)) {
        return atoi(s);
    }

    // 3. Fallback: find any digit in the string
    for (size_t i = 0; low[i] != '\0'; i++) {
        if (isdigit((unsigned char)low[i])) {
            return atoi(&low[i]);
        }
    }

    return -1;
}

static bool directory_has_images(const char *dir_path) {
    DWORD attr = GetFileAttributesA(dir_path);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    char search_pattern[MAX_PATH + 32];
    snprintf(search_pattern, sizeof(search_pattern), "%s\\*.*", dir_path);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    bool has_images = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const char *dot = strrchr(fd.cFileName, '.');
            if (dot) {
                if (_stricmp(dot, ".jpg") == 0 || _stricmp(dot, ".jpeg") == 0 ||
                    _stricmp(dot, ".png") == 0 || _stricmp(dot, ".webp") == 0) {
                    has_images = true;
                    break;
                }
            }
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return has_images;
}

// Checks if a chapter already has a folder on disk containing images.
// Matches exact folder names, padded numbers (Chapter 001, 001, 1), and arbitrary suffixes (e.g. "Chapter 1-eng-li").
static bool is_chapter_already_downloaded(const char *target_root, const char *chapter_name, int chapter_num) {
    if (!target_root || target_root[0] == '\0') return false;

    // 1. Direct exact name match
    char chapter_dir[1024];
    snprintf(chapter_dir, sizeof(chapter_dir), "%s\\%s", target_root, chapter_name);
    if (directory_has_images(chapter_dir)) return true;

    if (chapter_num > 0) {
        // 2. Standardized numbering variations
        char alt[1024];
        snprintf(alt, sizeof(alt), "%s\\Chapter %d", target_root, chapter_num);
        if (directory_has_images(alt)) return true;

        snprintf(alt, sizeof(alt), "%s\\Chapter %02d", target_root, chapter_num);
        if (directory_has_images(alt)) return true;

        snprintf(alt, sizeof(alt), "%s\\Chapter %03d", target_root, chapter_num);
        if (directory_has_images(alt)) return true;

        snprintf(alt, sizeof(alt), "%s\\Chapter %04d", target_root, chapter_num);
        if (directory_has_images(alt)) return true;

        snprintf(alt, sizeof(alt), "%s\\%03d", target_root, chapter_num);
        if (directory_has_images(alt)) return true;

        snprintf(alt, sizeof(alt), "%s\\%04d", target_root, chapter_num);
        if (directory_has_images(alt)) return true;

        snprintf(alt, sizeof(alt), "%s\\%d", target_root, chapter_num);
        if (directory_has_images(alt)) return true;

        // 3. Scan existing folders on disk by numeric extraction (e.g. "Chapter 1-eng-li", "Ch. 1 - Vol 1")
        char search_pattern[1024];
        snprintf(search_pattern, sizeof(search_pattern), "%s\\*.*", target_root);

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search_pattern, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0) {

                    int folder_num = extract_chapter_number_from_string(fd.cFileName);
                    if (folder_num == chapter_num) {
                        char matched_dir[1024];
                        snprintf(matched_dir, sizeof(matched_dir), "%s\\%s", target_root, fd.cFileName);
                        if (directory_has_images(matched_dir)) {
                            FindClose(hFind);
                            return true;
                        }
                    }
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }

    return false;
}

static int download_file(const char *url, const char *outpath, const char *referer) {
    if (!overwrite) {
        DWORD attr = GetFileAttributesA(outpath);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return DL_SKIPPED; // File already exists, skip it!
        }
    }

    CURL *curl = curl_easy_init();
    if (!curl) return DL_ERR_INIT;
    FILE *fp = fopen(outpath, "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        return DL_ERR_FILE;
    }

    char cookie_path[MAX_PATH];
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t *p_slash = wcsrchr(exePath, L'\\');
    if (p_slash) *(p_slash + 1) = L'\0';
    wcscat(exePath, L"cookies.txt");
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, cookie_path, MAX_PATH, NULL, NULL);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_path);
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookie_path);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
    headers = curl_slist_append(headers, "Accept: image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9,id;q=0.8");
    headers = curl_slist_append(headers, "sec-ch-ua: \"Chromium\";v=\"124\", \"Google Chrome\";v=\"124\", \"Not-A.Brand\";v=\"99\"");
    headers = curl_slist_append(headers, "sec-ch-ua-mobile: ?0");
    headers = curl_slist_append(headers, "sec-ch-ua-platform: \"Windows\"");
    headers = curl_slist_append(headers, "sec-fetch-dest: image");
    headers = curl_slist_append(headers, "sec-fetch-mode: no-cors");
    headers = curl_slist_append(headers, "sec-fetch-site: cross-site");

    if (referer && referer[0] != '\0') {
        curl_easy_setopt(curl, CURLOPT_REFERER, referer);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    fclose(fp);
    curl_easy_cleanup(curl);

    if (stop_requested) {
        DeleteFileA(outpath);
        return DL_ABORTED;
    }
    return (res == CURLE_OK) ? DL_SUCCESS : DL_ERR_PERFORM;
}
void extract_extension(const char *url, char *ext, size_t max_ext) {
    strncpy(ext, "jpg", max_ext);
    const char *q = strchr(url, '?');
    size_t url_len = q ? (size_t)(q - url) : strlen(url);

    for (size_t i = url_len; i > 0; i--) {
        if (url[i - 1] == '.') {
            size_t elen = url_len - i;
            if (elen > 0 && elen < max_ext) {
                strncpy(ext, url + i, elen);
                ext[elen] = '\0';
                for (size_t k = 0; ext[k] != '\0'; k++) ext[k] = (char)tolower((unsigned char)ext[k]);
            }
            break;
        }
        if (url[i - 1] == '/') break;
    }
}

static DWORD WINAPI DownloadThreadProc(LPVOID lpParam) {
    char target_root[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, folder_path, -1, target_root, MAX_PATH, NULL, NULL);

    char base_host[256];
    get_base_host(base_url, base_host, sizeof(base_host));

    const Scraper *scraper = find_scraper(base_url);
    if (!scraper) scraper = get_scraper_by_id("generic");

    char logmsg[2048];
    snprintf(logmsg, sizeof(logmsg), "=======================================================");
    append_log(logmsg);
    snprintf(logmsg, sizeof(logmsg), "%s", _T("str_log_process_started"));
    append_log(logmsg);
    snprintf(logmsg, sizeof(logmsg), "=======================================================");
    append_log(logmsg);
    snprintf(logmsg, sizeof(logmsg), _T("str_log_step_1"), scraper->display_name);
    append_log(logmsg);
    snprintf(logmsg, sizeof(logmsg), _T("str_log_main_link"), base_url);
    append_log(logmsg);
    snprintf(logmsg, sizeof(logmsg), _T("str_log_target_folder"), target_root);
    append_log(logmsg);

    if (only_missing) {
        append_log(_T("str_log_filter_mode_missing"));
    } else {
        snprintf(logmsg, sizeof(logmsg), _T("str_log_filter_mode"), chapter_filter[0] ? chapter_filter : _T("str_log_all_chapters"));
        append_log(logmsg);
    }

    snprintf(logmsg, sizeof(logmsg), _T("str_log_file_mode"), overwrite ? _T("str_log_file_mode_overwrite") : _T("str_log_file_mode_skip"));
    append_log(logmsg);
    append_log(_T("str_log_cf_setup"));

    // 1. Fetch main page HTML
    append_log(_T("str_log_step_2"));
    char *main_html = fetch_url(base_url, NULL);
    if (!main_html) {
        append_log(_T("str_log_err_main_page"));
        is_downloading = false;
        EnableWindow(hStartBtn, TRUE);
        EnableWindow(hStopBtn, FALSE);
        return 1;
    }

    // 2. Scan chapters
    append_log(_T("str_log_step_3"));
    ChapterItem *chapters = (ChapterItem *)calloc(MAX_CHAPTERS, sizeof(ChapterItem));
    if (!chapters) {
        append_log(_T("str_log_err_mem_chapters"));
        free(main_html);
        is_downloading = false;
        EnableWindow(hStartBtn, TRUE);
        EnableWindow(hStopBtn, FALSE);
        return 1;
    }

    int total_chapters = scraper->scan_chapters(base_url, main_html, chapters, MAX_CHAPTERS);
    free(main_html);

    if (total_chapters == 0) {
        if (strstr(base_url, "/reader/") != NULL || strstr(base_url, "-chapter-") != NULL || strstr(base_url, "/chapter/") != NULL) {
            append_log(_T("str_log_single_chapter"));
            snprintf(chapters[0].url, sizeof(chapters[0].url), "%s", base_url);
            snprintf(chapters[0].name, sizeof(chapters[0].name), "%s", "Chapter_Single");
            total_chapters = 1;
        } else {
            append_log(_T("str_log_no_chapters_found"));
            free(chapters);
            is_downloading = false;
            EnableWindow(hStartBtn, TRUE);
            EnableWindow(hStopBtn, FALSE);
            return 0;
        }
    } else {
        snprintf(logmsg, sizeof(logmsg), _T("str_log_scan_success"), total_chapters);
        append_log(logmsg);

        // Sort chronologically from lowest chapter number to highest
        if (total_chapters > 1 && chapters[0].chapter_num > chapters[total_chapters - 1].chapter_num) {
            append_log(_T("str_log_adjust_order"));
            for (int i = 0; i < total_chapters / 2; i++) {
                ChapterItem temp = chapters[i];
                chapters[i] = chapters[total_chapters - 1 - i];
                chapters[total_chapters - 1 - i] = temp;
            }
        }
    }

    // Count how many chapters will be downloaded
    append_log(_T("str_log_step_4"));
    int to_download_count = 0;
    if (only_missing) {
        for (int i = 0; i < total_chapters; i++) {
            if (!is_chapter_already_downloaded(target_root, chapters[i].name, chapters[i].chapter_num)) {
                to_download_count++;
            }
        }
        snprintf(logmsg, sizeof(logmsg), _T("str_log_scan_missing_result"),
                 to_download_count, total_chapters);
        append_log(logmsg);
    } else {
        for (int i = 0; i < total_chapters; i++) {
            if (is_chapter_selected(chapters[i].chapter_num, i + 1, chapter_filter)) {
                to_download_count++;
            }
        }
        if (chapter_filter[0] != '\0') {
            snprintf(logmsg, sizeof(logmsg), _T("str_log_active_filter"),
                     chapter_filter, to_download_count, total_chapters);
            append_log(logmsg);
        }
    }


    if (to_download_count == 0) {
        if (only_missing) {
            append_log(_T("str_log_all_complete"));
        } else {
            append_log(_T("str_log_no_filter_match"));
        }
        free(chapters);
        is_downloading = false;
        EnableWindow(hStartBtn, TRUE);
        EnableWindow(hStopBtn, FALSE);
        return 0;
    }

    // 3. Loop through each selected chapter
    append_log(_T("str_log_step_5"));
    static char page_urls[MAX_PAGES][1024];
    int current_processed = 0;

    for (int ch_idx = 0; ch_idx < total_chapters; ch_idx++) {
        if (stop_requested) {
            append_log(_T("str_log_user_stopped"));
            break;
        }

        if (only_missing) {
            if (is_chapter_already_downloaded(target_root, chapters[ch_idx].name, chapters[ch_idx].chapter_num)) {
                // Chapter is already complete on disk, skip network requests completely!
                continue;
            }
        } else {
            if (!is_chapter_selected(chapters[ch_idx].chapter_num, ch_idx + 1, chapter_filter)) {
                continue;
            }
        }

        current_processed++;
        snprintf(logmsg, sizeof(logmsg), _T("str_log_processing_chapter"),
                 current_processed, to_download_count, chapters[ch_idx].name, chapters[ch_idx].chapter_num);
        append_log(logmsg);

        // Create chapter subfolder
        char chapter_dir[1024];
        snprintf(chapter_dir, sizeof(chapter_dir), "%s\\%s", target_root, chapters[ch_idx].name);
        wchar_t chapter_dir_w[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, chapter_dir, -1, chapter_dir_w, MAX_PATH);
        CreateDirectoryW(chapter_dir_w, NULL);

        // Fetch chapter reader HTML with referer
        char *ch_html = fetch_url(chapters[ch_idx].url, base_url);
        if (!ch_html) {
            snprintf(logmsg, sizeof(logmsg), _T("str_log_warn_open_reader"), chapters[ch_idx].url);
            append_log(logmsg);
            continue;
        }

        // Scan images from chapter reader
        int total_pages = scraper->scan_images(chapters[ch_idx].url, ch_html, page_urls, MAX_PAGES);
        free(ch_html);

        if (total_pages == 0) {
            append_log(_T("str_log_no_images_in_chapter"));
            continue;
        }

        snprintf(logmsg, sizeof(logmsg), _T("str_log_images_found"), total_pages);
        append_log(logmsg);

        int downloaded_count = 0;
        int skipped_count = 0;

        for (int p_idx = 0; p_idx < total_pages; p_idx++) {
            if (stop_requested) break;

            char ext[16] = "jpg";
            extract_extension(page_urls[p_idx], ext, sizeof(ext));

            // Sequential renaming: 001.jpg, 002.jpg...
            char out_img_path[2048];
            snprintf(out_img_path, sizeof(out_img_path), "%s\\%03d.%s", chapter_dir, p_idx + 1, ext);

            int rc = download_file(page_urls[p_idx], out_img_path, chapters[ch_idx].url);
            if (rc == DL_SUCCESS) {
                downloaded_count++;
            } else if (rc == DL_SKIPPED) {
                skipped_count++;
            } else if (rc == DL_ABORTED) {
                append_log(_T("str_log_download_aborted"));
                break;
            } else {
                snprintf(logmsg, sizeof(logmsg), _T("str_log_image_fail"), p_idx + 1, ext, page_urls[p_idx]);
                append_log(logmsg);
            }
        }

        if (downloaded_count > 0 && skipped_count > 0) {
            snprintf(logmsg, sizeof(logmsg), _T("str_log_chapter_finish_mixed"), downloaded_count, skipped_count);
        } else if (downloaded_count > 0) {
            snprintf(logmsg, sizeof(logmsg), _T("str_log_chapter_finish_all"), downloaded_count);
        } else {
            snprintf(logmsg, sizeof(logmsg), _T("str_log_chapter_finish_skip"), skipped_count);
        }
        append_log(logmsg);
    }

    free(chapters);

    if (stop_requested) {
        append_log("\n=========================================");
        append_log(_T("str_log_status_cancelled"));
        append_log("=========================================");
    } else {
        append_log("\n=========================================");
        append_log(_T("str_log_status_success"));
        append_log("=========================================");
    }

    is_downloading = false;
    EnableWindow(hStartBtn, TRUE);
    EnableWindow(hStopBtn, FALSE);
    return 0;
}

static void extract_manga_title_from_url(const wchar_t *url, wchar_t *title_out, size_t max_len) {
    wchar_t temp[1024];
    wcsncpy(temp, url, 1023);
    temp[1023] = L'\0';

    size_t len = wcslen(temp);
    while (len > 0 && (temp[len - 1] == L'/' || temp[len - 1] == L'\\')) {
        temp[--len] = L'\0';
    }

    const wchar_t *patterns[] = { L"/all-chapters", L"/all-chapter", L"/chapters" };
    for (int p = 0; p < 3; p++) {
        size_t plen = wcslen(patterns[p]);
        if (len >= plen && _wcsicmp(temp + len - plen, patterns[p]) == 0) {
            temp[len - plen] = L'\0';
            len -= plen;
            break;
        }
    }

    wchar_t *lastSlash = wcsrchr(temp, L'/');
    if (!lastSlash) lastSlash = wcsrchr(temp, L'\\');
    if (lastSlash && *(lastSlash + 1) != L'\0') {
        wcsncpy(title_out, lastSlash + 1, max_len - 1);
        title_out[max_len - 1] = L'\0';
    } else {
        wcsncpy(title_out, L"Manga_Output", max_len - 1);
        title_out[max_len - 1] = L'\0';
    }

    wchar_t *query = wcschr(title_out, L'?');
    if (query) *query = L'\0';

    for (size_t i = 0; title_out[i] != L'\0'; i++) {
        wchar_t c = title_out[i];
        if (c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'/' || c == L'\\') {
            title_out[i] = L'_';
        }
    }
}

static void start_download(HWND hwnd) {
    if (is_downloading) {
        append_log(_T("str_log_download_in_progress"));
        return;
    }

    wchar_t url_w[1024];
    GetWindowTextW(hUrlEdit, url_w, 1024);
    if (wcslen(url_w) == 0) {
        MessageBoxW(hwnd, _TW("str_alert_url_empty"), _TW("str_alert_warning"), MB_OK | MB_ICONWARNING);
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, url_w, -1, base_url, 1024, NULL, NULL);

    GetWindowTextW(hFolderEdit, folder_path, MAX_PATH);
    if (wcslen(folder_path) == 0) {
        MessageBoxW(hwnd, _TW("str_alert_folder_empty"), _TW("str_alert_warning"), MB_OK | MB_ICONWARNING);
        return;
    }
    CreateDirectoryW(folder_path, NULL);

    only_missing = (SendMessageW(hOnlyMissingChk, BM_GETCHECK, 0, 0) == BST_CHECKED);

    wchar_t filter_w[512];
    GetWindowTextW(hChapterFilterEdit, filter_w, 512);
    WideCharToMultiByte(CP_UTF8, 0, filter_w, -1, chapter_filter, sizeof(chapter_filter), NULL, NULL);
    trim_whitespace(chapter_filter);

    overwrite = (SendMessageW(hOverwriteChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    stop_requested = false;
    is_downloading = true;

    EnableWindow(hStartBtn, FALSE);
    EnableWindow(hStopBtn, TRUE);

    if (hDownloadThread) {
        CloseHandle(hDownloadThread);
        hDownloadThread = NULL;
    }
    hDownloadThread = CreateThread(NULL, 0, DownloadThreadProc, NULL, 0, NULL);
}

static void apply_gui_font(HWND hwndCtrl) {
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(hwndCtrl, WM_SETFONT, (WPARAM)hFont, TRUE);
}

static void draw_lang_combo_item(LPDRAWITEMSTRUCT lpdis) {
    if (lpdis->itemID == (UINT)-1) return;

    bool is_selected = (lpdis->itemState & ODS_SELECTED);
    COLORREF bg_color = is_selected ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW);
    COLORREF text_color = is_selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);

    HBRUSH hBgBrush = CreateSolidBrush(bg_color);
    FillRect(lpdis->hDC, &lpdis->rcItem, hBgBrush);
    DeleteObject(hBgBrush);

    // Flag dimensions: 20x14 px, vertically centered
    int flag_w = 20;
    int flag_h = 14;
    int flag_x = lpdis->rcItem.left + 6;
    int flag_y = lpdis->rcItem.top + (lpdis->rcItem.bottom - lpdis->rcItem.top - flag_h) / 2;

    int item_type = (int)lpdis->itemData; // 0 = LANG_ID, 1 = LANG_EN

    if (item_type == (int)LANG_ID) {
        // --- Indonesian Flag (Merah Putih) ---
        RECT rcTop = { flag_x, flag_y, flag_x + flag_w, flag_y + (flag_h / 2) };
        HBRUSH hRedBrush = CreateSolidBrush(RGB(220, 20, 40));
        FillRect(lpdis->hDC, &rcTop, hRedBrush);
        DeleteObject(hRedBrush);

        RECT rcBottom = { flag_x, flag_y + (flag_h / 2), flag_x + flag_w, flag_y + flag_h };
        HBRUSH hWhiteBrush = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(lpdis->hDC, &rcBottom, hWhiteBrush);
        DeleteObject(hWhiteBrush);
    } else {
        // --- USA Flag (Stars and Stripes) ---
        HBRUSH hUsRed = CreateSolidBrush(RGB(179, 25, 66));
        HBRUSH hUsWhite = CreateSolidBrush(RGB(255, 255, 255));
        for (int s = 0; s < 7; s++) {
            RECT rcStripe = { flag_x, flag_y + s * 2, flag_x + flag_w, flag_y + (s + 1) * 2 };
            FillRect(lpdis->hDC, &rcStripe, (s % 2 == 0) ? hUsRed : hUsWhite);
        }
        DeleteObject(hUsRed);
        DeleteObject(hUsWhite);

        // Blue Canton (top-left 9x8)
        RECT rcCanton = { flag_x, flag_y, flag_x + 9, flag_y + 8 };
        HBRUSH hCanton = CreateSolidBrush(RGB(10, 49, 97));
        FillRect(lpdis->hDC, &rcCanton, hCanton);
        DeleteObject(hCanton);

        // Star dots (white pixels inside canton)
        SetPixel(lpdis->hDC, flag_x + 2, flag_y + 2, RGB(255, 255, 255));
        SetPixel(lpdis->hDC, flag_x + 6, flag_y + 2, RGB(255, 255, 255));
        SetPixel(lpdis->hDC, flag_x + 4, flag_y + 4, RGB(255, 255, 255));
        SetPixel(lpdis->hDC, flag_x + 2, flag_y + 6, RGB(255, 255, 255));
        SetPixel(lpdis->hDC, flag_x + 6, flag_y + 6, RGB(255, 255, 255));
    }

    // Border around the flag
    RECT rcFlag = { flag_x, flag_y, flag_x + flag_w, flag_y + flag_h };
    HBRUSH hBorder = CreateSolidBrush(RGB(160, 160, 160));
    FrameRect(lpdis->hDC, &rcFlag, hBorder);
    DeleteObject(hBorder);

    // Get item text
    wchar_t text[128] = {0};
    SendMessageW(lpdis->hwndItem, CB_GETLBTEXT, lpdis->itemID, (LPARAM)text);

    // Draw text
    SetTextColor(lpdis->hDC, text_color);
    SetBkMode(lpdis->hDC, TRANSPARENT);
    HFONT hFont = (HFONT)SendMessageW(lpdis->hwndItem, WM_GETFONT, 0, 0);
    HFONT hOldFont = NULL;
    if (hFont) hOldFont = (HFONT)SelectObject(lpdis->hDC, hFont);

    RECT rcText = lpdis->rcItem;
    rcText.left = flag_x + flag_w + 8;
    DrawTextW(lpdis->hDC, text, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (hOldFont) SelectObject(lpdis->hDC, hOldFont);

    if (lpdis->itemState & ODS_FOCUS) {
        DrawFocusRect(lpdis->hDC, &lpdis->rcItem);
    }
}

static void apply_language_change(HWND hwnd) {
    SetWindowTextW(hwnd, _TW("str_app_title"));

    if (hMainTab) {
        TCITEMW tie = {0};
        tie.mask = TCIF_TEXT;
        tie.pszText = (LPWSTR)_TW("str_tab_home");
        TabCtrl_SetItem(hMainTab, 0, &tie);
        tie.pszText = (LPWSTR)_TW("str_tab_ai_test");
        TabCtrl_SetItem(hMainTab, 1, &tie);
        tie.pszText = (LPWSTR)_TW("str_tab_ai_catalog");
        TabCtrl_SetItem(hMainTab, 2, &tie);
        tie.pszText = (LPWSTR)_TW("str_tab_ai_active");
        TabCtrl_SetItem(hMainTab, 3, &tie);
    }

    if (hLblUrl) SetWindowTextW(hLblUrl, _TW("str_main_link"));
    if (hLblFolder) SetWindowTextW(hLblFolder, _TW("str_folder_label"));
    if (hBrowseBtn) SetWindowTextW(hBrowseBtn, _TW("str_btn_browse"));
    if (hDefBtn) SetWindowTextW(hDefBtn, _TW("str_btn_default"));
    if (hLblFilter) SetWindowTextW(hLblFilter, _TW("str_filter_label"));
    if (hOnlyMissingChk) SetWindowTextW(hOnlyMissingChk, _TW("str_only_missing_chk"));
    if (hOverwriteChk) SetWindowTextW(hOverwriteChk, _TW("str_overwrite_chk"));
    if (hStartBtn) SetWindowTextW(hStartBtn, _TW("str_btn_start"));
    if (hStopBtn) SetWindowTextW(hStopBtn, _TW("str_btn_stop"));
    if (hLblLog) SetWindowTextW(hLblLog, _TW("str_log_label"));
    if (hLblLang) SetWindowTextW(hLblLang, _TW("str_lang_label"));

    update_site_status_ui();
    ai_agent_refresh_lang();

    InvalidateRect(hwnd, NULL, TRUE);
    UpdateWindow(hwnd);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;
        g_home_ctrl_count = 0;

        // Main Tab Control
        hMainTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                   WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
                                   10, 8, 705, 610, hwnd, (HMENU)ID_MAIN_TAB, hInst, NULL);
        apply_gui_font(hMainTab);

        TCITEMW tie = {0};
        tie.mask = TCIF_TEXT;
        tie.pszText = (LPWSTR)_TW("str_tab_home");
        TabCtrl_InsertItem(hMainTab, 0, &tie);
        tie.pszText = (LPWSTR)_TW("str_tab_ai_test");
        TabCtrl_InsertItem(hMainTab, 1, &tie);
        tie.pszText = (LPWSTR)_TW("str_tab_ai_catalog");
        TabCtrl_InsertItem(hMainTab, 2, &tie);
        tie.pszText = (LPWSTR)_TW("str_tab_ai_active");
        TabCtrl_InsertItem(hMainTab, 3, &tie);

        // Label: Link Utama Manga
        hLblUrl = CreateWindowW(L"STATIC", _TW("str_main_link"), WS_CHILD | WS_VISIBLE,
                                25, 38, 200, 18, hwnd, NULL, hInst, NULL);
        apply_gui_font(hLblUrl);
        g_home_controls[g_home_ctrl_count++] = hLblUrl;

        // Label: Bahasa / Language
        hLblLang = CreateWindowW(L"STATIC", _TW("str_lang_label"), WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                 410, 36, 65, 18, hwnd, NULL, hInst, NULL);
        apply_gui_font(hLblLang);
        g_home_controls[g_home_ctrl_count++] = hLblLang;

        // Input Select: Language ComboBox with Flags
        hLangCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                     480, 31, 220, 150, hwnd, (HMENU)ID_LANG_COMBO, hInst, NULL);
        apply_gui_font(hLangCombo);
        g_home_controls[g_home_ctrl_count++] = hLangCombo;

        SendMessageW(hLangCombo, CB_ADDSTRING, 0, (LPARAM)L"Indonesia (ID) - id");
        SendMessageW(hLangCombo, CB_SETITEMDATA, 0, (LPARAM)LANG_ID);

        SendMessageW(hLangCombo, CB_ADDSTRING, 0, (LPARAM)L"English (USA) - us");
        SendMessageW(hLangCombo, CB_SETITEMDATA, 1, (LPARAM)LANG_EN);

        Language cur_lang = load_language();
        SendMessageW(hLangCombo, CB_SETCURSEL, (cur_lang == LANG_EN) ? 1 : 0, 0);

        // Edit: URL
        hUrlEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                  25, 58, 675, 24, hwnd, (HMENU)ID_URL_EDIT, hInst, NULL);
        apply_gui_font(hUrlEdit);
        g_home_controls[g_home_ctrl_count++] = hUrlEdit;

        // Status Label: Detected Website & Engine
        hSiteStatus = CreateWindowW(L"STATIC", _TW("str_site_status_placeholder"),
                                   WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                   25, 86, 675, 18, hwnd, (HMENU)ID_SITE_STATUS, hInst, NULL);
        apply_gui_font(hSiteStatus);
        g_home_controls[g_home_ctrl_count++] = hSiteStatus;

        // Label: Folder Penyimpanan
        hLblFolder = CreateWindowW(L"STATIC", _TW("str_folder_label"), WS_CHILD | WS_VISIBLE,
                                   25, 110, 200, 18, hwnd, NULL, hInst, NULL);
        apply_gui_font(hLblFolder);
        g_home_controls[g_home_ctrl_count++] = hLblFolder;

        // Edit: Folder
        hFolderEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                     25, 130, 475, 24, hwnd, (HMENU)ID_FOLDER_EDIT, hInst, NULL);
        apply_gui_font(hFolderEdit);
        g_home_controls[g_home_ctrl_count++] = hFolderEdit;

        // Button: Browse...
        hBrowseBtn = CreateWindowW(L"BUTTON", _TW("str_btn_browse"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   508, 129, 90, 26, hwnd, (HMENU)ID_BROWSE_BTN, hInst, NULL);
        apply_gui_font(hBrowseBtn);
        g_home_controls[g_home_ctrl_count++] = hBrowseBtn;

        // Button: Default
        hDefBtn = CreateWindowW(L"BUTTON", _TW("str_btn_default"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                605, 129, 95, 26, hwnd, (HMENU)ID_DEFAULT_BTN, hInst, NULL);
        apply_gui_font(hDefBtn);
        g_home_controls[g_home_ctrl_count++] = hDefBtn;

        // Label: Pilih Chapter
        hLblFilter = CreateWindowW(L"STATIC", _TW("str_filter_label"),
                                   WS_CHILD | WS_VISIBLE,
                                   25, 160, 500, 18, hwnd, NULL, hInst, NULL);
        apply_gui_font(hLblFilter);
        g_home_controls[g_home_ctrl_count++] = hLblFilter;

        // Edit: Chapter Filter
        hChapterFilterEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                            25, 180, 675, 24, hwnd, (HMENU)ID_CHAPTER_FILTER_EDIT, hInst, NULL);
        apply_gui_font(hChapterFilterEdit);
        g_home_controls[g_home_ctrl_count++] = hChapterFilterEdit;

        // Checkbox: Hanya Download Chapter Belum Ada
        hOnlyMissingChk = CreateWindowW(L"BUTTON", _TW("str_only_missing_chk"),
                                       WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       25, 210, 675, 22, hwnd, (HMENU)ID_ONLY_MISSING_CHK, hInst, NULL);
        apply_gui_font(hOnlyMissingChk);
        SendMessageW(hOnlyMissingChk, BM_SETCHECK, BST_UNCHECKED, 0);
        g_home_controls[g_home_ctrl_count++] = hOnlyMissingChk;

        // Checkbox: Overwrite / Skip
        hOverwriteChk = CreateWindowW(L"BUTTON", _TW("str_overwrite_chk"),
                                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     25, 234, 675, 22, hwnd, (HMENU)ID_OVERWRITE_CHK, hInst, NULL);
        apply_gui_font(hOverwriteChk);
        SendMessageW(hOverwriteChk, BM_SETCHECK, BST_UNCHECKED, 0);
        g_home_controls[g_home_ctrl_count++] = hOverwriteChk;

        // Button: Start
        hStartBtn = CreateWindowW(L"BUTTON", _TW("str_btn_start"), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                  25, 264, 140, 30, hwnd, (HMENU)ID_START_BTN, hInst, NULL);
        apply_gui_font(hStartBtn);
        g_home_controls[g_home_ctrl_count++] = hStartBtn;

        // Button: Stop
        hStopBtn = CreateWindowW(L"BUTTON", _TW("str_btn_stop"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 175, 264, 100, 30, hwnd, (HMENU)ID_STOP_BTN, hInst, NULL);
        apply_gui_font(hStopBtn);
        EnableWindow(hStopBtn, FALSE);
        g_home_controls[g_home_ctrl_count++] = hStopBtn;

        // Label: Log Aktivitas
        hLblLog = CreateWindowW(L"STATIC", _TW("str_log_label"), WS_CHILD | WS_VISIBLE,
                                25, 302, 200, 18, hwnd, NULL, hInst, NULL);
        apply_gui_font(hLblLog);
        g_home_controls[g_home_ctrl_count++] = hLblLog;

        // Edit: Log Box
        hLogEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                   25, 322, 675, 278, hwnd, (HMENU)ID_LOG_EDIT, hInst, NULL);
        apply_gui_font(hLogEdit);
        g_home_controls[g_home_ctrl_count++] = hLogEdit;

        update_site_status_ui();

        // Kunci aplikasi dan jalankan migrasi database SQLite sampai semua tabel & file siap
        char mig_err[512] = "";
        if (!db_migration_run_all(hwnd, append_log, mig_err, sizeof(mig_err))) {
            wchar_t werr[512];
            MultiByteToWideChar(CP_UTF8, 0, mig_err, -1, werr, 512);
            MessageBoxW(hwnd, werr, _TW("str_alert_db_migration_error"), MB_ICONWARNING | MB_OK);
        }

        // Initialize AI Agent settings controls & database
        ai_agent_init(hwnd, hInst);

        // Show Home tab by default
        switch_main_tab(0);
        break;
    }
    case WM_MEASUREITEM: {
        LPMEASUREITEMSTRUCT lpmis = (LPMEASUREITEMSTRUCT)lParam;
        if (lpmis && lpmis->CtlID == ID_LANG_COMBO) {
            lpmis->itemHeight = 22;
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT lpdis = (LPDRAWITEMSTRUCT)lParam;
        if (lpdis && lpdis->CtlID == ID_LANG_COMBO) {
            draw_lang_combo_item(lpdis);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id >= 300 && id < 400) {
            ai_agent_on_command(hwnd, wParam, lParam);
            break;
        }

        switch (id) {
        case ID_LANG_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(hLangCombo, CB_GETCURSEL, 0, 0);
                Language new_lang = (sel == 1) ? LANG_EN : LANG_ID;
                save_language(new_lang);
                lang_init(language_code(new_lang));
                apply_language_change(hwnd);
            }
            break;

        case ID_URL_EDIT:
            if (HIWORD(wParam) == EN_CHANGE) {
                update_site_status_ui();
            }
            break;

        case ID_ONLY_MISSING_CHK: {
            bool is_checked = (SendMessageW(hOnlyMissingChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(hChapterFilterEdit, !is_checked);
            break;
        }

        case ID_START_BTN:
            start_download(hwnd);
            break;

        case ID_STOP_BTN:
            if (is_downloading) {
                stop_requested = true;
                append_log(_T("str_log_stop_requested"));
            }
            break;

        case ID_BROWSE_BTN: {
            BROWSEINFOW bi = { 0 };
            bi.hwndOwner = hwnd;
            bi.lpszTitle = _TW("str_browse_title");
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl != NULL) {
                wchar_t chosenPath[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, chosenPath)) {
                    SetWindowTextW(hFolderEdit, chosenPath);
                }
                CoTaskMemFree(pidl);
            }
            break;
        }

        case ID_DEFAULT_BTN: {
            wchar_t url_buf[1024];
            GetWindowTextW(hUrlEdit, url_buf, 1024);
            if (wcslen(url_buf) == 0) {
                MessageBoxW(hwnd, _TW("str_alert_fill_url_first"), _TW("str_alert_info"), MB_OK | MB_ICONINFORMATION);
                break;
            }

            wchar_t title[256];
            extract_manga_title_from_url(url_buf, title, 256);

            wchar_t docsPath[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docsPath))) {
                wchar_t mangaDir[MAX_PATH];
                _snwprintf(mangaDir, MAX_PATH, L"%s\\Manga", docsPath);
                CreateDirectoryW(mangaDir, NULL);

                wchar_t defaultPath[MAX_PATH];
                _snwprintf(defaultPath, MAX_PATH, L"%s\\%s", mangaDir, title);
                CreateDirectoryW(defaultPath, NULL);

                SetWindowTextW(hFolderEdit, defaultPath);
                append_log(_T("str_log_default_folder_set"));
                char defaultPathA[MAX_PATH];
                WideCharToMultiByte(CP_UTF8, 0, defaultPath, -1, defaultPathA, MAX_PATH, NULL, NULL);
                append_log(defaultPathA);
            }
            break;
        }
        }
        break;
    }
    case WM_NOTIFY: {
        LPNMHDR pnm = (LPNMHDR)lParam;
        if (pnm && pnm->idFrom == ID_MAIN_TAB && pnm->code == TCN_SELCHANGE) {
            int cur_tab = (int)SendMessageW(hMainTab, TCM_GETCURSEL, 0, 0);
            switch_main_tab(cur_tab);
        }
        break;
    }
    case WM_APP_SCAN_DONE:
        ai_agent_on_scan_done(hwnd, (int)wParam);
        break;
    case WM_DESTROY:
        stop_requested = true;
        if (hDownloadThread) {
            WaitForSingleObject(hDownloadThread, 1000);
            CloseHandle(hDownloadThread);
            hDownloadThread = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInst, PWSTR pCmdLine, int nCmdShow) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    Language user_lang = load_language();
    lang_init(language_code(user_lang));

    const wchar_t CLASS_NAME[] = L"MangaDownloaderWndClass";
    HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    HICON hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = hIcon;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, _TW("str_app_title"),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 740, 675,
                                NULL, NULL, hInstance, NULL);
    if (hwnd == NULL) {
        lang_free();
        return 0;
    }

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    log_buffer[0] = L'\0';
    curl_global_init(CURL_GLOBAL_DEFAULT);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    curl_global_cleanup();
    lang_free();
    return (int)msg.wParam;
}
