// src/config.c
#include "config.h"
#include <windows.h>
#include <string.h>

#define CONFIG_FILE "config.ini"
#define SECTION "Settings"
#define KEY "Language"

static const char* lang_codes[] = { "en", "id" };

static Language parse_language(const char* code) {
    if (code == NULL) return LANG_EN;
    for (int i = 0; i < (int)(sizeof(lang_codes)/sizeof(lang_codes[0])); ++i) {
        if (strcmp(code, lang_codes[i]) == 0) {
            return (Language)i;
        }
    }
    return LANG_EN; // default
}

Language load_language(void) {
    char buf[16] = {0};
    DWORD chars = GetPrivateProfileStringA(SECTION, KEY, "en", buf, sizeof(buf), CONFIG_FILE);
    if (chars == 0) {
        // No config file or key missing, create default
        save_language(LANG_EN);
        return LANG_EN;
    }
    return parse_language(buf);
}

void save_language(Language lang) {
    const char* code = language_code(lang);
    WritePrivateProfileStringA(SECTION, KEY, code, CONFIG_FILE);
}

const char* language_code(Language lang) {
    if (lang < 0 || lang >= (Language)(sizeof(lang_codes)/sizeof(lang_codes[0]))) {
        return "en";
    }
    return lang_codes[lang];
}
