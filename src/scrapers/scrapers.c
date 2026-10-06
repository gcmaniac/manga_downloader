#include <string.h>
#include "scrapers.h"

static const Scraper *g_registered_scrapers[] = {
    &g_scraper_manganato,
    &g_scraper_mgeko,
    &g_scraper_asura,
    &g_scraper_flame,
    &g_scraper_generic, // Fallback must be last
    NULL
};

const Scraper *find_scraper(const char *url) {
    if (!url || url[0] == '\0') return NULL;

    for (int i = 0; g_registered_scrapers[i] != NULL; i++) {
        if (g_registered_scrapers[i]->match(url)) {
            return g_registered_scrapers[i];
        }
    }
    return &g_scraper_generic;
}

const Scraper *get_scraper_by_id(const char *id) {
    if (!id) return NULL;
    for (int i = 0; g_registered_scrapers[i] != NULL; i++) {
        if (strcmp(g_registered_scrapers[i]->id, id) == 0) {
            return g_registered_scrapers[i];
        }
    }
    return NULL;
}

const Scraper **get_all_scrapers(int *out_count) {
    static const Scraper *list[16];
    int count = 0;
    for (int i = 0; g_registered_scrapers[i] != NULL && count < 15; i++) {
        list[count++] = g_registered_scrapers[i];
    }
    list[count] = NULL;
    if (out_count) *out_count = count;
    return list;
}
