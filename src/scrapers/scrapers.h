#ifndef SCRAPERS_H
#define SCRAPERS_H

#include "scraper.h"
#include "scraper_manganato.h"
#include "scraper_mgeko.h"
#include "scraper_asura.h"
#include "scraper_flame.h"
#include "scraper_generic.h"

// Returns the best matching scraper for the given URL
const Scraper *find_scraper(const char *url);

// Returns a scraper by its ID ("manganato", "mgeko", "asura", "flame", "generic")
const Scraper *get_scraper_by_id(const char *id);

// Returns the list of all registered scrapers and their count
const Scraper **get_all_scrapers(int *out_count);

#endif // SCRAPERS_H
