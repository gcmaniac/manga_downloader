#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "scraper_flame.h"

static bool flame_match(const char *url) {
    if (!url) return false;
    char low[1024];
    size_t len = strlen(url);
    if (len >= sizeof(low)) len = sizeof(low) - 1;
    for (size_t i = 0; i < len; i++) low[i] = (char)tolower((unsigned char)url[i]);
    low[len] = '\0';

    return (strstr(low, "flamecomics.xyz") != NULL ||
            strstr(low, "flamecomics.me") != NULL);
}

static int flame_scan_chapters(const char *base_url, const char *html, ChapterItem *chapters, int max_chapters) {
    if (!html || !chapters || max_chapters <= 0) return 0;

    int count = 0;
    char base_host[256];
    get_base_host(base_url, base_host, sizeof(base_host));

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

                bool is_chapter = (strstr(raw_url, "-chapter-") != NULL ||
                                   strstr(raw_url, "/chapter-") != NULL ||
                                   strstr(raw_url, "/chapter/") != NULL);

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

                        int ch_num = extract_chapter_number_from_string(ch_title);
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

static int flame_scan_images(const char *chapter_url, const char *html, char images[][1024], int max_images) {
    if (!html || !images || max_images <= 0) return 0;

    int count = 0;
    char base_host[256];
    get_base_host(chapter_url, base_host, sizeof(base_host));

    const char *start_p = strstr(html, "id=\"readerarea\"");
    if (!start_p) start_p = strstr(html, "class=\"reading-content\"");
    if (!start_p) start_p = html;

    const char *p = start_p;
    while ((p = strstr(p, "<img ")) != NULL && count < max_images) {
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;

        const char *src_pos = NULL;
        const char *dsrc = strstr(p, "data-src=\"");
        if (!dsrc || dsrc > tag_end) dsrc = strstr(p, "data-original=\"");
        if (!dsrc || dsrc > tag_end) dsrc = strstr(p, "data-lazy-src=\"");

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
                                         strstr(low_url, ".svg") != NULL);

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

const Scraper g_scraper_flame = {
    .id = "flame",
    .display_name = "Flame Comics",
    .match = flame_match,
    .scan_chapters = flame_scan_chapters,
    .scan_images = flame_scan_images
};
