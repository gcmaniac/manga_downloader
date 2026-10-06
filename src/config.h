// src/config.h
#ifndef CONFIG_H
#define CONFIG_H

// Language enumeration
typedef enum {
    LANG_EN, // English (default)
    LANG_ID  // Bahasa Indonesia
    // Add more languages here as needed
} Language;

// Load language setting (default is English if config missing)
Language load_language(void);

// Save language setting to config.ini
void save_language(Language lang);

// Helper: get language code string (e.g., "en", "id")
const char* language_code(Language lang);

#endif // CONFIG_H
