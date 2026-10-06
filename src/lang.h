// src/lang.h
#ifndef LANG_H
#define LANG_H

#include <stdbool.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize localization module with language code (e.g., "id", "en")
bool lang_init(const char *lang_code);

// Get localized string in UTF-8 (char*)
const char *lang_str(const char *key);

// Get localized string in UTF-16 (wchar_t*) for Win32 GUI APIs
const wchar_t *lang_str_w(const char *key);

// Free all loaded language resources
void lang_free(void);

// Shorthand convenience macros
#define _T(k)  lang_str(k)
#define _TW(k) lang_str_w(k)

#ifdef __cplusplus
}
#endif

#endif // LANG_H
