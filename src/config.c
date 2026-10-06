// src/config.c
#include "config.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

#define SECTION "Settings"
#define KEY "Language"

static const char* lang_codes[] = { "en", "id" };

static void get_config_path(char *out_path, size_t max_len) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t *pSlash = wcsrchr(exePath, L'\\');
    if (pSlash) *(pSlash + 1) = L'\0';
    wcscat(exePath, L"config.ini");
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, out_path, (int)max_len, NULL, NULL);
}

static Language parse_language(const char* code) {
    if (code == NULL) return LANG_ID;
    if (_stricmp(code, "us") == 0) return LANG_EN;
    for (int i = 0; i < (int)(sizeof(lang_codes)/sizeof(lang_codes[0])); ++i) {
        if (strcmp(code, lang_codes[i]) == 0) {
            return (Language)i;
        }
    }
    return LANG_ID; // default to Indonesian as requested
}

Language load_language(void) {
    char cfg_path[MAX_PATH];
    get_config_path(cfg_path, sizeof(cfg_path));

    char buf[16] = {0};
    DWORD chars = GetPrivateProfileStringA(SECTION, KEY, "id", buf, sizeof(buf), cfg_path);
    if (chars == 0) {
        save_language(LANG_ID);
        return LANG_ID;
    }
    return parse_language(buf);
}

void save_language(Language lang) {
    char cfg_path[MAX_PATH];
    get_config_path(cfg_path, sizeof(cfg_path));

    const char* code = language_code(lang);
    WritePrivateProfileStringA(SECTION, KEY, code, cfg_path);
}

const char* language_code(Language lang) {
    if (lang < 0 || lang >= (Language)(sizeof(lang_codes)/sizeof(lang_codes[0]))) {
        return "id";
    }
    return lang_codes[lang];
}
