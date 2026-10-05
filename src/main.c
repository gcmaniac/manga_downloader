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
#define ID_MAIN_TAB             150

#define MAX_CHAPTERS            5000
#define MAX_PAGES               1000

#define DL_SUCCESS              0
#define DL_SKIPPED              10
#define DL_ABORTED              4
#define DL_ERR_INIT             1
#define DL_ERR_FILE             2
#define DL_ERR_PERFORM          3

typedef enum {
    SITE_UNKNOWN = 0,
    SITE_MGEKO,         // mgeko.cc, mangageko.com
    SITE_MANGANATO,     // manganato.com, chapmanganato.to, mangakakalot.com
    SITE_ASURA,         // asuracomic.net, asurascans.com, asura.gg
    SITE_FLAME,         // flamecomics.xyz, flamecomics.me
    SITE_GENERIC        // Universal fallback
} SiteType;

typedef struct {
    char url[1024];
    char name[256];
    int chapter_num;
} ChapterItem;

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

static HWND hMainTab = NULL;
static HWND g_home_controls[20];
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

static void get_base_host(const char *url, char *host_out, size_t max_len) {
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

static SiteType detect_site_from_url(const char *url, char *site_name_out, size_t max_len) {
    char low[1024];
    size_t ulen = strlen(url);
    if (ulen >= sizeof(low)) ulen = sizeof(low) - 1;
    for (size_t i = 0; i < ulen; i++) low[i] = (char)tolower((unsigned char)url[i]);
    low[ulen] = '\0';

    if (strstr(low, "mgeko.cc") != NULL || strstr(low, "mangageko.com") != NULL) {
        if (site_name_out) snprintf(site_name_out, max_len, "MangaGeko (mgeko.cc)");
        return SITE_MGEKO;
    }
    if (strstr(low, "manganato.com") != NULL || strstr(low, "chapmanganato.to") != NULL ||
        strstr(low, "mangakakalot.com") != NULL || strstr(low, "manganelo.com") != NULL) {
        if (site_name_out) snprintf(site_name_out, max_len, "MangaNato / MangaKakalot");
        return SITE_MANGANATO;
    }
    if (strstr(low, "asuracomic.net") != NULL || strstr(low, "asurascans.com") != NULL ||
        strstr(low, "asura.gg") != NULL) {
        if (site_name_out) snprintf(site_name_out, max_len, "Asura Scans");
        return SITE_ASURA;
    }
    if (strstr(low, "flamecomics.xyz") != NULL || strstr(low, "flamecomics.me") != NULL) {
        if (site_name_out) snprintf(site_name_out, max_len, "Flame Comics");
        return SITE_FLAME;
    }

    char host[256] = "";
    get_base_host(url, host, sizeof(host));
    if (host[0] != '\0') {
        const char *hname = strstr(host, "://");
        hname = hname ? (hname + 3) : host;
        if (site_name_out) snprintf(site_name_out, max_len, "%s", hname);
        return SITE_GENERIC;
    }

    if (site_name_out) snprintf(site_name_out, max_len, "Belum terdeteksi");
    return SITE_UNKNOWN;
}

static void update_site_status_ui(void) {
    if (!hUrlEdit || !hSiteStatus) return;
    wchar_t url_w[1024];
    GetWindowTextW(hUrlEdit, url_w, 1024);
    if (wcslen(url_w) == 0) {
        SetWindowTextW(hSiteStatus, L"Status Web: (Silakan tempelkan link URL manga di atas)");
        return;
    }

    char url_a[1024];
    WideCharToMultiByte(CP_UTF8, 0, url_w, -1, url_a, 1024, NULL, NULL);

    char site_name[256] = "";
    SiteType type = detect_site_from_url(url_a, site_name, sizeof(site_name));

    wchar_t status_msg[512];
    if (type == SITE_MGEKO) {
        _snwprintf(status_msg, 512, L"Status Web: MangaGeko (mgeko.cc)  |  Scraper: MangaGeko Engine [Didukung]");
    } else if (type == SITE_MANGANATO) {
        _snwprintf(status_msg, 512, L"Status Web: MangaNato / MangaKakalot  |  Scraper: MangaNato Engine [Didukung]");
    } else if (type == SITE_ASURA) {
        _snwprintf(status_msg, 512, L"Status Web: Asura Scans  |  Scraper: Asura Scans Engine [Didukung]");
    } else if (type == SITE_FLAME) {
        _snwprintf(status_msg, 512, L"Status Web: Flame Comics  |  Scraper: Flame Comics Engine [Didukung]");
    } else if (type == SITE_GENERIC) {
        wchar_t sname_w[256];
        MultiByteToWideChar(CP_UTF8, 0, site_name, -1, sname_w, 256);
        _snwprintf(status_msg, 512, L"Status Web: %s  |  Scraper: Universal Heuristic Engine", sname_w);
    } else {
        _snwprintf(status_msg, 512, L"Status Web: Format URL belum dikenali");
    }

    SetWindowTextW(hSiteStatus, status_msg);
}

static char *fetch_url(const char *url, const char *referer) {
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

    if (referer && referer[0] != '\0') {
        curl_easy_setopt(curl, CURLOPT_REFERER, referer);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(chunk.memory);
        return NULL;
    }
    return chunk.memory;
}

static void resolve_url(const char *base_host, const char *raw_url, char *out_url, size_t max_len) {
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

static void sanitize_filename(char *name) {
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

static void trim_whitespace(char *str) {
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

// Checks if a chapter already has a folder on disk containing images
static bool is_chapter_already_downloaded(const char *target_root, const char *chapter_name, int chapter_num) {
    char chapter_dir[1024];
    snprintf(chapter_dir, sizeof(chapter_dir), "%s\\%s", target_root, chapter_name);

    DWORD attr = GetFileAttributesA(chapter_dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // Also check if alternative folder named "Chapter <num>" exists
        if (chapter_num > 0) {
            char alt_dir[1024];
            snprintf(alt_dir, sizeof(alt_dir), "%s\\Chapter %d", target_root, chapter_num);
            attr = GetFileAttributesA(alt_dir);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                snprintf(chapter_dir, sizeof(chapter_dir), "%s", alt_dir);
            } else {
                return false;
            }
        } else {
            return false;
        }
    }

    // Check if directory contains at least one image file
    char search_pattern[2048];
    snprintf(search_pattern, sizeof(search_pattern), "%s\\*.*", chapter_dir);

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
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

    if (referer && referer[0] != '\0') {
        curl_easy_setopt(curl, CURLOPT_REFERER, referer);
    }

    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    curl_easy_cleanup(curl);

    if (stop_requested) {
        DeleteFileA(outpath);
        return DL_ABORTED;
    }
    return (res == CURLE_OK) ? DL_SUCCESS : DL_ERR_PERFORM;
}

static int scan_chapters(const char *html, const char *base_host, ChapterItem *chapters, int max_chapters, SiteType site) {
    int count = 0;
    const char *p = html;

    while ((p = strstr(p, "<a ")) != NULL && count < max_chapters) {
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;

        const char *href_pos = strstr(p, "href=\"");
        if (!href_pos || href_pos > tag_end) {
            href_pos = strstr(p, "href='");
        }
        if (href_pos && href_pos < tag_end) {
            char quote = href_pos[5];
            const char *val_start = href_pos + 6;
            const char *val_end = strchr(val_start, quote);
            if (val_end && (val_end - val_start) < 1000) {
                char raw_url[1024];
                size_t ulen = val_end - val_start;
                strncpy(raw_url, val_start, ulen);
                raw_url[ulen] = '\0';

                bool is_chapter = false;

                if (site == SITE_MGEKO) {
                    is_chapter = (strstr(raw_url, "/reader/") != NULL);
                } else if (site == SITE_MANGANATO) {
                    is_chapter = (strstr(raw_url, "/chapter-") != NULL || strstr(p, "chapter-name") != NULL);
                } else if (site == SITE_ASURA) {
                    is_chapter = (strstr(raw_url, "/chapter/") != NULL || strstr(raw_url, "-chapter-") != NULL);
                } else {
                    is_chapter = (strstr(raw_url, "/reader/") != NULL ||
                                  strstr(raw_url, "-chapter-") != NULL ||
                                  strstr(raw_url, "/chapter-") != NULL ||
                                  strstr(raw_url, "/chapter/") != NULL ||
                                  strstr(raw_url, "/ch-") != NULL);
                }

                if (is_chapter &&
                    strstr(raw_url, "login") == NULL &&
                    strstr(raw_url, "javascript:") == NULL &&
                    strstr(raw_url, "#") == NULL) {

                    char full_url[1024];
                    resolve_url(base_host, raw_url, full_url, sizeof(full_url));

                    bool exists = false;
                    for (int i = 0; i < count; i++) {
                        if (strcmp(chapters[i].url, full_url) == 0) {
                            exists = true;
                            break;
                        }
                    }

                    if (!exists) {
                        snprintf(chapters[count].url, sizeof(chapters[count].url), "%s", full_url);

                        char ch_title[256] = "";
                        const char *a_end = strstr(tag_end, "</a>");
                        if (a_end) {
                            const char *ct = strstr(tag_end, "class=\"chapter-title\"");
                            if (!ct || ct > a_end) ct = strstr(tag_end, "class='chapter-title'");
                            if (!ct || ct > a_end) ct = strstr(tag_end, "class=\"chapter-name\"");
                            if (!ct || ct > a_end) ct = strstr(tag_end, "class='chapter-name'");
                            if (ct && ct < a_end) {
                                const char *ct_val = strchr(ct, '>');
                                if (ct_val && ct_val < a_end) {
                                    ct_val++;
                                    const char *ct_end = strchr(ct_val, '<');
                                    if (ct_end && ct_end <= a_end) {
                                        size_t tlen = ct_end - ct_val;
                                        if (tlen >= sizeof(ch_title)) tlen = sizeof(ch_title) - 1;
                                        strncpy(ch_title, ct_val, tlen);
                                        ch_title[tlen] = '\0';
                                        trim_whitespace(ch_title);
                                    }
                                }
                            }
                        }

                        if (ch_title[0] == '\0') {
                            const char *tattr = strstr(p, "title=\"");
                            if (tattr && tattr < tag_end) {
                                const char *tval = tattr + 7;
                                const char *tend = strchr(tval, '\"');
                                if (tend && tend <= tag_end) {
                                    size_t tlen = tend - tval;
                                    if (tlen >= sizeof(ch_title)) tlen = sizeof(ch_title) - 1;
                                    strncpy(ch_title, tval, tlen);
                                    ch_title[tlen] = '\0';
                                    trim_whitespace(ch_title);
                                }
                            }
                        }

                        if (ch_title[0] == '\0') {
                            char temp_u[1024];
                            strncpy(temp_u, raw_url, sizeof(temp_u) - 1);
                            temp_u[sizeof(temp_u) - 1] = '\0';
                            size_t tlen = strlen(temp_u);
                            while (tlen > 0 && temp_u[tlen - 1] == '/') temp_u[--tlen] = '\0';
                            char *last_s = strrchr(temp_u, '/');
                            if (last_s) {
                                snprintf(ch_title, sizeof(ch_title), "%s", last_s + 1);
                            } else {
                                snprintf(ch_title, sizeof(ch_title), "Chapter_%03d", count + 1);
                            }
                        }

                        sanitize_filename(ch_title);
                        if (ch_title[0] == '\0') snprintf(ch_title, sizeof(ch_title), "Chapter_%03d", count + 1);

                        if (isdigit((unsigned char)ch_title[0])) {
                            snprintf(chapters[count].name, sizeof(chapters[count].name), "Chapter %s", ch_title);
                        } else {
                            snprintf(chapters[count].name, sizeof(chapters[count].name), "%s", ch_title);
                        }

                        chapters[count].chapter_num = count + 1;
                        for (size_t s = 0; ch_title[s] != '\0'; s++) {
                            if (isdigit((unsigned char)ch_title[s])) {
                                chapters[count].chapter_num = atoi(&ch_title[s]);
                                break;
                            }
                        }

                        count++;
                    }
                }
            }
        }
        p = tag_end + 1;
    }
    return count;
}

static int scan_chapter_images(const char *html, const char *base_host, char images[][1024], int max_images, SiteType site) {
    int count = 0;

    const char *start_p = html;
    const char *reader_sec = NULL;

    if (site == SITE_MGEKO) {
        reader_sec = strstr(html, "id=\"chapter-reader\"");
        if (!reader_sec) reader_sec = strstr(html, "id='chapter-reader'");
    } else if (site == SITE_MANGANATO) {
        reader_sec = strstr(html, "container-chapter-reader");
    } else if (site == SITE_ASURA) {
        reader_sec = strstr(html, "id=\"readerarea\"");
        if (!reader_sec) reader_sec = strstr(html, "class=\"rd-article\"");
    }

    if (!reader_sec) {
        const char *candidates[] = {
            "id=\"chapter-reader\"", "class=\"chapter-reader\"",
            "id=\"readerarea\"", "class=\"reading-content\"",
            "container-chapter-reader", "id=\"reader\"", "class=\"page-in\""
        };
        for (size_t c = 0; c < sizeof(candidates)/sizeof(candidates[0]); c++) {
            reader_sec = strstr(html, candidates[c]);
            if (reader_sec) break;
        }
    }

    if (reader_sec) {
        start_p = reader_sec;
    }

    const char *p = start_p;
    while ((p = strstr(p, "<img ")) != NULL && count < max_images) {
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;

        const char *src_pos = NULL;
        const char *dsrc = strstr(p, "data-src=\"");
        if (!dsrc || dsrc > tag_end) dsrc = strstr(p, "data-original=\"");
        if (!dsrc || dsrc > tag_end) dsrc = strstr(p, "data-lazy-src=\"");
        if (!dsrc || dsrc > tag_end) dsrc = strstr(p, "data-src='");
        if (!dsrc || dsrc > tag_end) dsrc = strstr(p, "data-original='");

        if (dsrc && dsrc < tag_end) {
            src_pos = dsrc;
            src_pos = strchr(src_pos, '=') + 1;
        } else {
            const char *src = strstr(p, "src=\"");
            if (!src || src > tag_end) src = strstr(p, "src='");
            if (src && src < tag_end) {
                src_pos = src + 4;
            }
        }

        if (src_pos && src_pos < tag_end) {
            char quote = *src_pos;
            if (quote == '\"' || quote == '\'') {
                src_pos++;
                const char *val_end = strchr(src_pos, quote);
                if (val_end && (val_end - src_pos) < 1000) {
                    char raw_url[1024];
                    size_t ulen = val_end - src_pos;
                    strncpy(raw_url, src_pos, ulen);
                    raw_url[ulen] = '\0';
                    trim_whitespace(raw_url);

                    char low_url[1024];
                    for (size_t k = 0; k <= ulen && k < sizeof(low_url) - 1; k++) {
                        low_url[k] = (char)tolower((unsigned char)raw_url[k]);
                    }
                    low_url[ulen] = '\0';

                    bool is_ad_or_logo = (strstr(low_url, "logo") != NULL ||
                                         strstr(low_url, "/static/img/") != NULL ||
                                         strstr(low_url, "loading") != NULL ||
                                         strstr(low_url, "avatar") != NULL ||
                                         strstr(low_url, "banner") != NULL ||
                                         strstr(low_url, "favicon") != NULL ||
                                         strstr(low_url, ".svg") != NULL ||
                                         strstr(low_url, "radioads") != NULL);

                    bool is_image_ext = (strstr(low_url, ".jpg") != NULL ||
                                         strstr(low_url, ".jpeg") != NULL ||
                                         strstr(low_url, ".png") != NULL ||
                                         strstr(low_url, ".webp") != NULL);

                    if (!is_ad_or_logo && is_image_ext && raw_url[0] != '\0') {
                        char full_img_url[1024];
                        resolve_url(base_host, raw_url, full_img_url, sizeof(full_img_url));

                        snprintf(images[count], sizeof(images[count]), "%s", full_img_url);
                        count++;
                    }
                }
            }
        }
        p = tag_end + 1;
    }
    return count;
}

static void extract_extension(const char *url, char *ext, size_t max_ext) {
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

    char site_name[256] = "";
    SiteType site_type = detect_site_from_url(base_url, site_name, sizeof(site_name));

    char logmsg[2048];
    snprintf(logmsg, sizeof(logmsg), "=== Memulai Proses Manga Downloader ===");
    append_log(logmsg);
    snprintf(logmsg, sizeof(logmsg), "Link Utama: %s", base_url);
    append_log(logmsg);
    snprintf(logmsg, sizeof(logmsg), "Website Terdeteksi: %s", site_name);
    append_log(logmsg);

    if (only_missing) {
        append_log("Mode Seleksi: HANYA CHAPTER YANG BELUM ADA DI FOLDER");
    } else {
        snprintf(logmsg, sizeof(logmsg), "Filter Chapter: %s", chapter_filter[0] ? chapter_filter : "(Semua Chapter)");
        append_log(logmsg);
    }

    snprintf(logmsg, sizeof(logmsg), "Folder Target: %s", target_root);
    append_log(logmsg);
    snprintf(logmsg, sizeof(logmsg), "Mode Duplikasi: %s", overwrite ? "Timpa (Overwrite)" : "Lewati jika sudah ada (Skip)");
    append_log(logmsg);

    // 1. Fetch main page HTML
    append_log("Mengambil halaman utama untuk memindai daftar chapter...");
    char *main_html = fetch_url(base_url, NULL);
    if (!main_html) {
        append_log("[Error] Gagal mengakses link utama manga. Periksa koneksi internet atau URL.");
        is_downloading = false;
        EnableWindow(hStartBtn, TRUE);
        EnableWindow(hStopBtn, FALSE);
        return 1;
    }

    // 2. Scan chapters
    ChapterItem *chapters = (ChapterItem *)calloc(MAX_CHAPTERS, sizeof(ChapterItem));
    if (!chapters) {
        append_log("[Error] Gagal mengalokasikan memori untuk daftar chapter.");
        free(main_html);
        is_downloading = false;
        EnableWindow(hStartBtn, TRUE);
        EnableWindow(hStopBtn, FALSE);
        return 1;
    }

    int total_chapters = scan_chapters(main_html, base_host, chapters, MAX_CHAPTERS, site_type);
    free(main_html);

    if (total_chapters == 0) {
        if (strstr(base_url, "/reader/") != NULL || strstr(base_url, "-chapter-") != NULL || strstr(base_url, "/chapter/") != NULL) {
            append_log("Link yang dimasukkan terdeteksi sebagai single chapter.");
            snprintf(chapters[0].url, sizeof(chapters[0].url), "%s", base_url);
            snprintf(chapters[0].name, sizeof(chapters[0].name), "%s", "Chapter_Single");
            total_chapters = 1;
        } else {
            append_log("[Info] Tidak ditemukan daftar chapter pada halaman tersebut.");
            free(chapters);
            is_downloading = false;
            EnableWindow(hStartBtn, TRUE);
            EnableWindow(hStopBtn, FALSE);
            return 0;
        }
    } else {
        snprintf(logmsg, sizeof(logmsg), "Berhasil memindai: Ditemukan total %d chapter di situs.", total_chapters);
        append_log(logmsg);

        // Sort chronologically from lowest chapter number to highest
        if (total_chapters > 1 && chapters[0].chapter_num > chapters[total_chapters - 1].chapter_num) {
            append_log("Menyesuaikan urutan: Mengurutkan mulai dari chapter paling awal...");
            for (int i = 0; i < total_chapters / 2; i++) {
                ChapterItem temp = chapters[i];
                chapters[i] = chapters[total_chapters - 1 - i];
                chapters[total_chapters - 1 - i] = temp;
            }
        }
    }

    // Count how many chapters will be downloaded
    int to_download_count = 0;
    if (only_missing) {
        for (int i = 0; i < total_chapters; i++) {
            if (!is_chapter_already_downloaded(target_root, chapters[i].name, chapters[i].chapter_num)) {
                to_download_count++;
            }
        }
        snprintf(logmsg, sizeof(logmsg), "Mode Khusus: Terdeteksi %d chapter baru/belum ada (dari total %d chapter di situs).",
                 to_download_count, total_chapters);
        append_log(logmsg);
    } else {
        for (int i = 0; i < total_chapters; i++) {
            if (is_chapter_selected(chapters[i].chapter_num, i + 1, chapter_filter)) {
                to_download_count++;
            }
        }
        if (chapter_filter[0] != '\0') {
            snprintf(logmsg, sizeof(logmsg), "Filter aktif ['%s']: %d dari %d chapter akan diunduh.",
                     chapter_filter, to_download_count, total_chapters);
            append_log(logmsg);
        }
    }

    if (to_download_count == 0) {
        if (only_missing) {
            append_log("[Info] Semua chapter di situs sudah lengkap di folder target! Tidak ada chapter baru.");
        } else {
            append_log("[Peringatan] Tidak ada chapter yang cocok dengan filter yang ditentukan.");
        }
        free(chapters);
        is_downloading = false;
        EnableWindow(hStartBtn, TRUE);
        EnableWindow(hStopBtn, FALSE);
        return 0;
    }

    // 3. Loop through each selected chapter
    static char page_urls[MAX_PAGES][1024];
    int current_processed = 0;

    for (int ch_idx = 0; ch_idx < total_chapters; ch_idx++) {
        if (stop_requested) {
            append_log(">>> Pengunduhan dihentikan oleh pengguna.");
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
        snprintf(logmsg, sizeof(logmsg), "\n--- [%d/%d] Memproses: %s (No: %d) ---",
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
            snprintf(logmsg, sizeof(logmsg), "[Warning] Gagal membuka reader chapter: %s", chapters[ch_idx].url);
            append_log(logmsg);
            continue;
        }

        // Scan images from chapter reader
        int total_pages = scan_chapter_images(ch_html, base_host, page_urls, MAX_PAGES, site_type);
        free(ch_html);

        if (total_pages == 0) {
            append_log("-> Tidak ada gambar yang ditemukan pada chapter ini.");
            continue;
        }

        snprintf(logmsg, sizeof(logmsg), "-> Ditemukan %d gambar halaman.", total_pages);
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
                append_log("-> Download dibatalkan saat sedang berlangsung.");
                break;
            } else {
                snprintf(logmsg, sizeof(logmsg), "-> [Gagal] Gambar %03d.%s (%s)", p_idx + 1, ext, page_urls[p_idx]);
                append_log(logmsg);
            }
        }

        if (downloaded_count > 0 && skipped_count > 0) {
            snprintf(logmsg, sizeof(logmsg), "-> Selesai: %d gambar baru diunduh, %d dilewati (sudah ada).", downloaded_count, skipped_count);
        } else if (downloaded_count > 0) {
            snprintf(logmsg, sizeof(logmsg), "-> Selesai: Semua %d gambar berhasil diunduh.", downloaded_count);
        } else {
            snprintf(logmsg, sizeof(logmsg), "-> Dilewati: Semua %d gambar sudah ada di folder.", skipped_count);
        }
        append_log(logmsg);
    }

    free(chapters);

    if (stop_requested) {
        append_log("\n=========================================");
        append_log("STATUS: Pengunduhan DIBATALKAN oleh pengguna.");
        append_log("=========================================");
    } else {
        append_log("\n=========================================");
        append_log("STATUS: SEMUA PENGUNDUHAN BERHASIL SELESAI!");
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
        append_log("Unduhan sedang berjalan.");
        return;
    }

    wchar_t url_w[1024];
    GetWindowTextW(hUrlEdit, url_w, 1024);
    if (wcslen(url_w) == 0) {
        MessageBoxW(hwnd, L"Link utama manga belum diisi!", L"Peringatan", MB_OK | MB_ICONWARNING);
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, url_w, -1, base_url, 1024, NULL, NULL);

    GetWindowTextW(hFolderEdit, folder_path, MAX_PATH);
    if (wcslen(folder_path) == 0) {
        MessageBoxW(hwnd, L"Folder tujuan belum dipilih!", L"Peringatan", MB_OK | MB_ICONWARNING);
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
        tie.pszText = L"Download Manga (Home)";
        TabCtrl_InsertItem(hMainTab, 0, &tie);
        tie.pszText = L"Pengaturan & Uji AI";
        TabCtrl_InsertItem(hMainTab, 1, &tie);
        tie.pszText = L"Katalog Model Teruji";
        TabCtrl_InsertItem(hMainTab, 2, &tie);
        tie.pszText = L"Model AI Digunakan";
        TabCtrl_InsertItem(hMainTab, 3, &tie);

        // Label: Link Utama Manga
        HWND hLblUrl = CreateWindowW(L"STATIC", L"Link Utama Manga:", WS_CHILD | WS_VISIBLE,
                                     25, 38, 200, 18, hwnd, NULL, hInst, NULL);
        apply_gui_font(hLblUrl);
        g_home_controls[g_home_ctrl_count++] = hLblUrl;

        // Edit: URL
        hUrlEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                  25, 58, 675, 24, hwnd, (HMENU)ID_URL_EDIT, hInst, NULL);
        apply_gui_font(hUrlEdit);
        g_home_controls[g_home_ctrl_count++] = hUrlEdit;

        // Status Label: Detected Website & Engine
        hSiteStatus = CreateWindowW(L"STATIC", L"Status Web: (Silakan tempelkan link URL manga di atas)",
                                   WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                   25, 86, 675, 18, hwnd, (HMENU)ID_SITE_STATUS, hInst, NULL);
        apply_gui_font(hSiteStatus);
        g_home_controls[g_home_ctrl_count++] = hSiteStatus;

        // Label: Folder Penyimpanan
        HWND hLblFolder = CreateWindowW(L"STATIC", L"Folder Penyimpanan:", WS_CHILD | WS_VISIBLE,
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
        HWND hBrowseBtn = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       508, 129, 90, 26, hwnd, (HMENU)ID_BROWSE_BTN, hInst, NULL);
        apply_gui_font(hBrowseBtn);
        g_home_controls[g_home_ctrl_count++] = hBrowseBtn;

        // Button: Default
        HWND hDefBtn = CreateWindowW(L"BUTTON", L"Default", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    605, 129, 95, 26, hwnd, (HMENU)ID_DEFAULT_BTN, hInst, NULL);
        apply_gui_font(hDefBtn);
        g_home_controls[g_home_ctrl_count++] = hDefBtn;

        // Label: Pilih Chapter
        HWND hLblFilter = CreateWindowW(L"STATIC", L"Pilih Chapter (Kosongkan = Semua, contoh: 1, 2-5, 116-end):",
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
        hOnlyMissingChk = CreateWindowW(L"BUTTON", L"Hanya cari & download chapter yang belum ada di folder (Otomatis lewati yang sudah ada)",
                                       WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       25, 210, 675, 22, hwnd, (HMENU)ID_ONLY_MISSING_CHK, hInst, NULL);
        apply_gui_font(hOnlyMissingChk);
        SendMessageW(hOnlyMissingChk, BM_SETCHECK, BST_UNCHECKED, 0);
        g_home_controls[g_home_ctrl_count++] = hOnlyMissingChk;

        // Checkbox: Overwrite / Skip
        hOverwriteChk = CreateWindowW(L"BUTTON", L"Timpa file jika sudah ada (Default: Skip/Lewati jika tidak dicentang)",
                                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     25, 234, 675, 22, hwnd, (HMENU)ID_OVERWRITE_CHK, hInst, NULL);
        apply_gui_font(hOverwriteChk);
        SendMessageW(hOverwriteChk, BM_SETCHECK, BST_UNCHECKED, 0);
        g_home_controls[g_home_ctrl_count++] = hOverwriteChk;

        // Button: Start
        hStartBtn = CreateWindowW(L"BUTTON", L"Start Download", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                  25, 264, 140, 30, hwnd, (HMENU)ID_START_BTN, hInst, NULL);
        apply_gui_font(hStartBtn);
        g_home_controls[g_home_ctrl_count++] = hStartBtn;

        // Button: Stop
        hStopBtn = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 175, 264, 100, 30, hwnd, (HMENU)ID_STOP_BTN, hInst, NULL);
        apply_gui_font(hStopBtn);
        EnableWindow(hStopBtn, FALSE);
        g_home_controls[g_home_ctrl_count++] = hStopBtn;

        // Label: Log Aktivitas
        HWND hLblLog = CreateWindowW(L"STATIC", L"Log Aktivitas:", WS_CHILD | WS_VISIBLE,
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

        // Initialize AI Agent settings controls & database
        ai_agent_init(hwnd, hInst);

        // Show Home tab by default
        switch_main_tab(0);
        break;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id >= 300 && id < 400) {
            ai_agent_on_command(hwnd, wParam, lParam);
            break;
        }

        switch (id) {
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
                append_log("Permintaan stop dikirim. Menunggu proses saat ini selesai...");
            }
            break;

        case ID_BROWSE_BTN: {
            BROWSEINFOW bi = { 0 };
            bi.hwndOwner = hwnd;
            bi.lpszTitle = L"Pilih Folder Penyimpanan Manga:";
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
                MessageBoxW(hwnd, L"Silakan isi Link Utama Manga terlebih dahulu!", L"Info", MB_OK | MB_ICONINFORMATION);
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
                append_log("Folder default disetel ke:");
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

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"Manga Downloader",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 740, 675,
                                NULL, NULL, hInstance, NULL);
    if (hwnd == NULL) return 0;

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
    return (int)msg.wParam;
}
