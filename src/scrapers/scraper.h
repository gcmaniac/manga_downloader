#ifndef SCRAPER_H
#define SCRAPER_H

#include <stdbool.h>
#include <stddef.h>

#define MAX_CHAPTERS 5000
#define MAX_PAGES 1000

typedef struct {
    char url[1024];
    char name[256];
    int chapter_num;
} ChapterItem;

typedef struct Scraper {
    const char *id;
    const char *display_name;
    bool (*match)(const char *url);
    int (*scan_chapters)(const char *base_url, const char *html, ChapterItem *chapters, int max_chapters);
    int (*scan_images)(const char *chapter_url, const char *html, char images[][1024], int max_images);
} Scraper;

// Shared utilities provided by the application engine
char *fetch_url(const char *url, const char *referer);
void resolve_url(const char *base_host, const char *raw_url, char *out_url, size_t max_len);
void sanitize_filename(char *name);
void trim_whitespace(char *str);
void get_base_host(const char *url, char *host_out, size_t max_len);
void extract_extension(const char *url, char *ext, size_t max_ext);
int extract_chapter_number_from_string(const char *str);

#endif // SCRAPER_H
