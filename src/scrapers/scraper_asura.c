#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "scraper_asura.h"

static bool asura_match(const char *url) {
    if (!url) return false;
    char low[1024];
    size_t len = strlen(url);
    if (len >= sizeof(low)) len = sizeof(low) - 1;
    for (size_t i = 0; i < len; i++) low[i] = (char)tolower((unsigned char)url[i]);
    low[len] = '\0';

    return (strstr(low, "asura") != NULL);
}

static int asura_scan_chapters(const char *base_url, const char *html, ChapterItem *chapters, int max_chapters) {
    if (!html || !chapters || max_chapters <= 0) return 0;

    int count = 0;
    char base_host[256];
    get_base_host(base_url, base_host, sizeof(base_host));

    // 1. Locate chapter list container if available (modern Astro SSR or legacy themes)
    const char *p = strstr(html, "class=\"divide-y");
    if (!p) p = strstr(html, "max-h-[500px]");
    if (!p) p = strstr(html, "id=\"chapterlist\"");
    if (!p) p = strstr(html, "class=\"chapterlist\"");
    if (!p) p = strstr(html, "class=\"eplister\"");
    if (!p) p = html;

    const char *end_bound = NULL;
    if (p != html) {
        end_bound = strstr(p, "<!--astro:end--></astro-island>");
        if (!end_bound) end_bound = strstr(p, "<!-- Comments");
        if (!end_bound) end_bound = strstr(p, "class=\"footer\"");
        if (!end_bound) end_bound = strstr(p, "<footer");
    }

    while ((p = strstr(p, "<a ")) != NULL && count < max_chapters) {
        if (end_bound && p >= end_bound) break;

        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;
        if (end_bound && tag_end >= end_bound) break;

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

                // Look ahead for closing </a> to check button text if not in dedicated container
                const char *close_a = strstr(tag_end, "</a>");
                bool is_btn = false;
                if (close_a && (close_a - tag_end) < 2000) {
                    char a_text[2048];
                    size_t t_len = close_a - tag_end;
                    if (t_len >= sizeof(a_text)) t_len = sizeof(a_text) - 1;
                    strncpy(a_text, tag_end, t_len);
                    a_text[t_len] = '\0';

                    if (strstr(a_text, "First Chapter") != NULL ||
                        strstr(a_text, "Read First") != NULL ||
                        strstr(a_text, "Last Chapter") != NULL ||
                        strstr(a_text, "Bookmark") != NULL) {
                        is_btn = true;
                    }
                }

                bool is_chapter = (strstr(raw_url, "/chapter/") != NULL ||
                                   strstr(raw_url, "-chapter-") != NULL);

                if (is_chapter && !is_btn &&
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

                        // Try extracting chapter number directly from URL path segment:
                        // e.g. /comics/solo-leveling-bd5bdaf8/chapter/200 -> 200
                        int ch_num = -1;
                        const char *c_seg = strstr(raw_url, "/chapter/");
                        if (c_seg) {
                            c_seg += 9;
                            if (*c_seg && isdigit((unsigned char)*c_seg)) {
                                ch_num = atoi(c_seg);
                            }
                        }

                        // Fallback: check inner text of <a>
                        if (ch_num < 0 && close_a && (close_a - tag_end) < 1000) {
                            char inner[1024];
                            size_t in_len = close_a - tag_end;
                            if (in_len >= sizeof(inner)) in_len = sizeof(inner) - 1;
                            strncpy(inner, tag_end, in_len);
                            inner[in_len] = '\0';
                            ch_num = extract_chapter_number_from_string(inner);
                        }

                        if (ch_num < 0) ch_num = count + 1;
                        chapters[count].chapter_num = ch_num;

                        if (ch_num >= 1000) {
                            snprintf(chapters[count].name, sizeof(chapters[count].name), "Chapter %04d", ch_num);
                        } else {
                            snprintf(chapters[count].name, sizeof(chapters[count].name), "Chapter %03d", ch_num);
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

static int asura_scan_images(const char *chapter_url, const char *html, char images[][1024], int max_images) {
    if (!html || !images || max_images <= 0) return 0;

    int count = 0;
    char base_host[256];
    get_base_host(chapter_url, base_host, sizeof(base_host));

    // 1. Scan <img> tags
    const char *p = html;
    while ((p = strstr(p, "<img ")) != NULL && count < max_images) {
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;

        const char *src_pos = NULL;
        const char *dsrc = strstr(p, "data-src=\"");
        if (!dsrc || dsrc > tag_end) dsrc = strstr(p, "data-original=\"");
        if (!dsrc || dsrc > tag_end) dsrc = strstr(p, "data-lazy-src=\"");
        if (!dsrc || dsrc > tag_end) dsrc = strstr(p, "data-src='");

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
                                         strstr(low_url, "loading") != NULL ||
                                         strstr(low_url, "avatar") != NULL ||
                                         strstr(low_url, "banner") != NULL ||
                                         strstr(low_url, "favicon") != NULL ||
                                         strstr(low_url, "/covers/") != NULL ||
                                         strstr(low_url, "discord") != NULL ||
                                         strstr(low_url, ".svg") != NULL);

                    bool is_image_ext = (strstr(low_url, ".jpg") != NULL ||
                                         strstr(low_url, ".jpeg") != NULL ||
                                         strstr(low_url, ".png") != NULL ||
                                         strstr(low_url, ".webp") != NULL ||
                                         strstr(low_url, ".avif") != NULL);

                    // Check if this tag is a reader image:
                    // Either has /chapters/ in URL, or tag has data-page-index, or alt="Page
                    bool is_reader_tag = (strstr(low_url, "/chapters/") != NULL ||
                                          strstr(p, "data-page-index") != NULL ||
                                          strstr(p, "alt=\"page") != NULL ||
                                          strstr(p, "alt=\"Page") != NULL);

                    if (!is_ad_or_logo && is_image_ext && raw_url[0] != '\0') {
                        if (is_reader_tag || strstr(low_url, "/chapters/") != NULL) {
                            char full_img_url[1024];
                            resolve_url(base_host, raw_url, full_img_url, sizeof(full_img_url));

                            bool exists = false;
                            for (int i = 0; i < count; i++) {
                                if (strcmp(images[i], full_img_url) == 0) {
                                    exists = true;
                                    break;
                                }
                            }
                            if (!exists) {
                                snprintf(images[count], sizeof(images[count]), "%s", full_img_url);
                                count++;
                            }
                        }
                    }
                }
            }
        }
        p = tag_end + 1;
    }

    // 2. Fallback: Parse Astro/Next JSON props if <img> scan found nothing
    if (count == 0) {
        const char *pages_p = strstr(html, "&quot;pages&quot;:[1,[");
        if (!pages_p) pages_p = strstr(html, "\"pages\":[1,[");
        if (!pages_p) pages_p = strstr(html, "\"pages\":[");

        if (pages_p) {
            const char *end_pages = strstr(pages_p, "]]");
            if (!end_pages) end_pages = strstr(pages_p, "]");
            if (!end_pages) end_pages = pages_p + strlen(pages_p);

            const char *u_search = pages_p;
            while (u_search < end_pages && count < max_images) {
                const char *http = strstr(u_search, "http://");
                if (!http || http > end_pages) http = strstr(u_search, "https://");
                if (!http || http > end_pages) break;

                const char *u_end = http;
                while (u_end < end_pages && *u_end != '\"' && *u_end != '\'' &&
                       *u_end != '&' && *u_end != '\\' && !isspace((unsigned char)*u_end)) {
                    u_end++;
                }

                char raw_url[1024];
                size_t ulen = u_end - http;
                if (ulen < sizeof(raw_url)) {
                    strncpy(raw_url, http, ulen);
                    raw_url[ulen] = '\0';

                    char low_url[1024];
                    for (size_t k = 0; k <= ulen && k < sizeof(low_url) - 1; k++) {
                        low_url[k] = (char)tolower((unsigned char)raw_url[k]);
                    }
                    low_url[ulen] = '\0';

                    bool is_img = (strstr(low_url, ".webp") != NULL ||
                                   strstr(low_url, ".jpg") != NULL ||
                                   strstr(low_url, ".jpeg") != NULL ||
                                   strstr(low_url, ".png") != NULL);

                    if (is_img && strstr(low_url, "/covers/") == NULL) {
                        bool exists = false;
                        for (int i = 0; i < count; i++) {
                            if (strcmp(images[i], raw_url) == 0) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            snprintf(images[count], sizeof(images[count]), "%s", raw_url);
                            count++;
                        }
                    }
                }
                u_search = u_end + 1;
            }
        }
    }

    // 3. Fallback for legacy WordPress MangaReader themes (readerarea / rd-article)
    if (count == 0) {
        const char *start_p = strstr(html, "id=\"readerarea\"");
        if (!start_p) start_p = strstr(html, "class=\"rd-article\"");
        if (start_p) {
            const char *p_leg = start_p;
            while ((p_leg = strstr(p_leg, "<img ")) != NULL && count < max_images) {
                const char *tag_end = strchr(p_leg, '>');
                if (!tag_end) break;

                const char *src_pos = NULL;
                const char *dsrc = strstr(p_leg, "data-src=\"");
                if (!dsrc || dsrc > tag_end) dsrc = strstr(p_leg, "data-original=\"");
                if (!dsrc || dsrc > tag_end) dsrc = strstr(p_leg, "data-lazy-src=\"");

                if (dsrc && dsrc < tag_end) {
                    src_pos = dsrc;
                    src_pos = strchr(src_pos, '=') + 1;
                } else {
                    const char *src = strstr(p_leg, "src=\"");
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

                            char full_img_url[1024];
                            resolve_url(base_host, raw_url, full_img_url, sizeof(full_img_url));

                            bool exists = false;
                            for (int i = 0; i < count; i++) {
                                if (strcmp(images[i], full_img_url) == 0) {
                                    exists = true;
                                    break;
                                }
                            }
                            if (!exists) {
                                snprintf(images[count], sizeof(images[count]), "%s", full_img_url);
                                count++;
                            }
                        }
                    }
                }
                p_leg = tag_end + 1;
            }
        }
    }

    return count;
}

const Scraper g_scraper_asura = {
    .id = "asura",
    .display_name = "Asura Scans",
    .match = asura_match,
    .scan_chapters = asura_scan_chapters,
    .scan_images = asura_scan_images
};
