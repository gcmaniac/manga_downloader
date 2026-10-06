// src/lang.c
#include "lang.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#define MAX_ENTRIES 512

typedef struct {
    char *key;
    char *val;
    wchar_t *val_w;
} LangEntry;

static LangEntry g_entries[MAX_ENTRIES];
static int g_entry_count = 0;
static wchar_t g_fallback_w[256];

static void clear_entries(void) {
    for (int i = 0; i < g_entry_count; i++) {
        if (g_entries[i].key) free(g_entries[i].key);
        if (g_entries[i].val) free(g_entries[i].val);
        if (g_entries[i].val_w) free(g_entries[i].val_w);
    }
    g_entry_count = 0;
}

static char *read_file_to_memory(const wchar_t *filepath) {
    FILE *fp = _wfopen(filepath, L"rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        return NULL;
    }

    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t read_bytes = fread(buf, 1, size, fp);
    buf[read_bytes] = '\0';
    fclose(fp);
    return buf;
}

static bool try_load_json(const wchar_t *path) {
    char *json_text = read_file_to_memory(path);
    if (!json_text) return false;

    cJSON *root = cJSON_Parse(json_text);
    free(json_text);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return false;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (g_entry_count >= MAX_ENTRIES) break;
        if (cJSON_IsString(item) && item->string && item->valuestring) {
            LangEntry *e = &g_entries[g_entry_count];
            e->key = strdup(item->string);
            e->val = strdup(item->valuestring);

            int wlen = MultiByteToWideChar(CP_UTF8, 0, item->valuestring, -1, NULL, 0);
            if (wlen > 0) {
                e->val_w = (wchar_t *)malloc(wlen * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, item->valuestring, -1, e->val_w, wlen);
            } else {
                e->val_w = NULL;
            }
            g_entry_count++;
        }
    }

    cJSON_Delete(root);
    return g_entry_count > 0;
}

bool lang_init(const char *lang_code) {
    clear_entries();

    if (!lang_code || lang_code[0] == '\0') {
        lang_code = "id";
    }
    if (_stricmp(lang_code, "us") == 0) {
        lang_code = "en";
    }

    wchar_t wcode[32];
    MultiByteToWideChar(CP_UTF8, 0, lang_code, -1, wcode, 32);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t *pSlash = wcsrchr(exePath, L'\\');
    if (pSlash) *(pSlash + 1) = L'\0';

    wchar_t candidate[MAX_PATH];

    // Candidate 1: <exe_dir>\lang\<code].json
    _snwprintf(candidate, MAX_PATH, L"%slang\\%s.json", exePath, wcode);
    if (try_load_json(candidate)) return true;

    // Candidate 2: <exe_dir>..\src\lang\<code].json
    _snwprintf(candidate, MAX_PATH, L"%s..\\src\\lang\\%s.json", exePath, wcode);
    if (try_load_json(candidate)) return true;

    // Candidate 3: src\lang\<code].json
    _snwprintf(candidate, MAX_PATH, L"src\\lang\\%s.json", wcode);
    if (try_load_json(candidate)) return true;

    // Candidate 4: lang\<code].json
    _snwprintf(candidate, MAX_PATH, L"lang\\%s.json", wcode);
    if (try_load_json(candidate)) return true;

    // If requested language failed and wasn't "id", try fallback to "id"
    if (_stricmp(lang_code, "id") != 0) {
        _snwprintf(candidate, MAX_PATH, L"%slang\\id.json", exePath);
        if (try_load_json(candidate)) return true;

        _snwprintf(candidate, MAX_PATH, L"%s..\\src\\lang\\id.json", exePath);
        if (try_load_json(candidate)) return true;

        _snwprintf(candidate, MAX_PATH, L"src\\lang\\id.json");
        if (try_load_json(candidate)) return true;
    }

    return false;
}

const char *lang_str(const char *key) {
    if (!key) return "";
    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].key, key) == 0) {
            return g_entries[i].val;
        }
    }
    return key;
}

const wchar_t *lang_str_w(const char *key) {
    if (!key) return L"";
    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].key, key) == 0) {
            return g_entries[i].val_w ? g_entries[i].val_w : L"";
        }
    }
    MultiByteToWideChar(CP_UTF8, 0, key, -1, g_fallback_w, 256);
    return g_fallback_w;
}

void lang_free(void) {
    clear_entries();
}
