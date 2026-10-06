#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <cjson/cJSON.h>
#include "scraper_manganato.h"

static bool manganato_match(const char *url) {
    if (!url) return false;
    char low[1024];
    size_t len = strlen(url);
    if (len >= sizeof(low)) len = sizeof(low) - 1;
    for (size_t i = 0; i < len; i++) low[i] = (char)tolower((unsigned char)url[i]);
    low[len] = '\0';

    return (strstr(low, "manganato.gg") != NULL ||
            strstr(low, "manganato.com") != NULL ||
            strstr(low, "chapmanganato.to") != NULL ||
            strstr(low, "mangakakalot.com") != NULL ||
            strstr(low, "manganelo.com") != NULL);
}

// Extract slug from URL e.g. https://www.manganato.gg/manga/ruler-of-the-land -> ruler-of-the-land
static bool extract_slug_from_url(const char *url, char *slug_out, size_t max_len) {
    const char *p = strstr(url, "/manga/");
    if (!p) return false;
    p += 7; // skip "/manga/"
    while (*p == '/') p++;
    if (*p == '\0') return false;

    const char *end = strchr(p, '/');
    if (!end) end = strchr(p, '?');
    if (!end) end = p + strlen(p);

    size_t slen = (size_t)(end - p);
    if (slen == 0 || slen >= max_len) return false;

    strncpy(slug_out, p, slen);
    slug_out[slen] = '\0';
    trim_whitespace(slug_out);
    return (slug_out[0] != '\0');
}

// Scans chapters using MangaNato's JSON API (for manganato.gg dynamic SPA architecture)
static int manganato_scan_api(const char *base_url, const char *html, ChapterItem *chapters, int max_chapters) {
    char slug[256] = "";

    // 1. Try finding data-comic-slug in HTML
    const char *s_slug = strstr(html, "data-comic-slug=\"");
    if (s_slug) {
        s_slug += 17;
        const char *end = strchr(s_slug, '\"');
        if (end && (end - s_slug) < sizeof(slug)) {
            strncpy(slug, s_slug, end - s_slug);
            slug[end - s_slug] = '\0';
        }
    }

    // Fallback: extract slug from base_url
    if (slug[0] == '\0') {
        extract_slug_from_url(base_url, slug, sizeof(slug));
    }

    if (slug[0] == '\0') {
        return 0; // cannot determine slug for API
    }

    char url_template[512] = "";
    const char *s_tpl = strstr(html, "data-chapter-url-template=\"");
    if (s_tpl) {
        s_tpl += 27;
        const char *end = strchr(s_tpl, '\"');
        if (end && (end - s_tpl) < sizeof(url_template)) {
            strncpy(url_template, s_tpl, end - s_tpl);
            url_template[end - s_tpl] = '\0';
        }
    }

    char base_host[256] = "https://www.manganato.gg";
    get_base_host(base_url, base_host, sizeof(base_host));

    int count = 0;
    int offset = 0;
    const int page_limit = 1000;

    while (count < max_chapters) {
        char api_url[1024];
        snprintf(api_url, sizeof(api_url), "%s/api/manga/%s/chapters?limit=%d&offset=%d",
                 base_host, slug, page_limit, offset);

        char *json_resp = fetch_url(api_url, base_url);
        if (!json_resp) break;

        cJSON *root = cJSON_Parse(json_resp);
        if (!root) {
            free(json_resp);
            break;
        }

        cJSON *success = cJSON_GetObjectItem(root, "success");
        if (!cJSON_IsTrue(success)) {
            cJSON_Delete(root);
            free(json_resp);
            break;
        }

        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (!data) {
            cJSON_Delete(root);
            free(json_resp);
            break;
        }

        cJSON *ch_array = cJSON_GetObjectItem(data, "chapters");
        if (!cJSON_IsArray(ch_array)) {
            cJSON_Delete(root);
            free(json_resp);
            break;
        }

        int arr_size = cJSON_GetArraySize(ch_array);
        if (arr_size == 0) {
            cJSON_Delete(root);
            free(json_resp);
            break;
        }

        for (int i = 0; i < arr_size && count < max_chapters; i++) {
            cJSON *item = cJSON_GetArrayItem(ch_array, i);
            if (!item) continue;

            cJSON *c_name = cJSON_GetObjectItem(item, "chapter_name");
            cJSON *c_slug = cJSON_GetObjectItem(item, "chapter_slug");
            cJSON *c_num  = cJSON_GetObjectItem(item, "chapter_num");

            const char *name_str = c_name ? c_name->valuestring : "";
            const char *slug_str = c_slug ? c_slug->valuestring : "";

            if (!slug_str || slug_str[0] == '\0') continue;

            char ch_url[1024];
            if (url_template[0] != '\0') {
                // Replace __MANGA__ and __CHAPTER__
                char temp[1024];
                const char *m_pos = strstr(url_template, "__MANGA__");
                if (m_pos) {
                    size_t pre_len = m_pos - url_template;
                    snprintf(temp, sizeof(temp), "%.*s%s%s", (int)pre_len, url_template, slug, m_pos + 9);
                } else {
                    strncpy(temp, url_template, sizeof(temp) - 1);
                    temp[sizeof(temp) - 1] = '\0';
                }

                const char *c_pos = strstr(temp, "__CHAPTER__");
                if (c_pos) {
                    size_t pre_len = c_pos - temp;
                    snprintf(ch_url, sizeof(ch_url), "%.*s%s%s", (int)pre_len, temp, slug_str, c_pos + 11);
                } else {
                    strncpy(ch_url, temp, sizeof(ch_url) - 1);
                    ch_url[sizeof(ch_url) - 1] = '\0';
                }
            } else {
                snprintf(ch_url, sizeof(ch_url), "%s/manga/%s/%s", base_host, slug, slug_str);
            }

            // Deduplication check
            bool exists = false;
            for (int k = 0; k < count; k++) {
                if (strcmp(chapters[k].url, ch_url) == 0) {
                    exists = true;
                    break;
                }
            }
            if (exists) continue;

            snprintf(chapters[count].url, sizeof(chapters[count].url), "%s", ch_url);

            int ch_num = 0;
            if (c_num && c_num->type == cJSON_Number) {
                ch_num = c_num->valueint;
            } else {
                ch_num = extract_chapter_number_from_string(name_str ? name_str : slug_str);
            }
            if (ch_num <= 0 && c_num && c_num->type == cJSON_Number && c_num->valueint == 0) {
                ch_num = 0; // chapter 0
            } else if (ch_num < 0) {
                ch_num = count + 1;
            }

            chapters[count].chapter_num = ch_num;

            // Normalized format: "Chapter 001", "Chapter 728", or "Chapter 0001"
            if (ch_num >= 1000) {
                snprintf(chapters[count].name, sizeof(chapters[count].name), "Chapter %04d", ch_num);
            } else {
                snprintf(chapters[count].name, sizeof(chapters[count].name), "Chapter %03d", ch_num);
            }

            count++;
        }

        // Check pagination has_more
        bool has_more = false;
        cJSON *pagination = cJSON_GetObjectItem(data, "pagination");
        if (pagination) {
            cJSON *hm = cJSON_GetObjectItem(pagination, "has_more");
            if (cJSON_IsTrue(hm)) has_more = true;
        }

        cJSON_Delete(root);
        free(json_resp);

        if (!has_more) break;
        offset += page_limit;
    }

    return count;
}

// Scans chapters using static HTML parsing (for legacy manganato.com, chapmanganato.to, etc.)
static int manganato_scan_html(const char *base_url, const char *html, ChapterItem *chapters, int max_chapters) {
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

                bool is_chapter = (strstr(raw_url, "/chapter-") != NULL ||
                                   strstr(p, "chapter-name") != NULL ||
                                   strstr(p, "chapter-title") != NULL);

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
                        const char *a_end = strstr(tag_end, "</a>");
                        if (a_end) {
                            const char *ct = strstr(tag_end, "class=\"chapter-name\"");
                            if (!ct || ct > a_end) ct = strstr(tag_end, "class='chapter-name'");
                            if (!ct || ct > a_end) ct = strstr(tag_end, "class=\"chapter-title\"");
                            if (!ct || ct > a_end) ct = strstr(tag_end, "class='chapter-title'");
                            if (ct && ct < a_end) {
                                const char *ct_val = strchr(ct, '>');
                                if (ct_val && ct_val < a_end) {
                                    ct_val++;
                                    const char *ct_end = strchr(ct_val, '<');
                                    if (ct_end && ct_end <= a_end) {
                                        size_t tlen = ct_end - ct_val;
                                        if (tlen >= sizeof(ch_title)) tlen = sizeof(ch_title) - 1;
                                        strncpy(ch_title, ct_val, tlen);
                                        ch_title[tlen] = '\0';
                                        trim_whitespace(ch_title);
                                    }
                                }
                            }
                        }

                        if (ch_title[0] == '\0') {
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

static int manganato_scan_chapters(const char *base_url, const char *html, ChapterItem *chapters, int max_chapters) {
    if (!html || !chapters || max_chapters <= 0) return 0;

    // Check if site uses the dynamic API
    if (strstr(html, "chapter-list-container") != NULL ||
        strstr(html, "data-api-url") != NULL ||
        strstr(base_url, "manganato.gg") != NULL) {

        int api_count = manganato_scan_api(base_url, html, chapters, max_chapters);
        if (api_count > 0) {
            return api_count;
        }
    }

    // Fallback to static HTML scanning
    return manganato_scan_html(base_url, html, chapters, max_chapters);
}

static int manganato_scan_images(const char *chapter_url, const char *html, char images[][1024], int max_images) {
    if (!html || !images || max_images <= 0) return 0;

    int count = 0;
    char base_host[256];
    get_base_host(chapter_url, base_host, sizeof(base_host));

    // MangaNato specific reader container
    const char *start_p = strstr(html, "container-chapter-reader");
    if (!start_p) start_p = strstr(html, "class=\"container-chapter-reader\"");
    if (!start_p) start_p = strstr(html, "id=\"vungdoc\"");
    if (!start_p) start_p = strstr(html, "class=\"vungdoc\"");
    if (!start_p) start_p = html;

    // Find end boundary of the reader area to avoid scanning comment section or recommended widgets
    const char *end_p = strstr(start_p, "class=\"comment-info\"");
    if (!end_p) end_p = strstr(start_p, "<div class=\"fb-comments\"");
    if (!end_p) end_p = strstr(start_p, "id=\"comment\"");
    if (!end_p) end_p = strstr(start_p, "data-zone=\"bottom_content\"");
    if (!end_p) end_p = strstr(start_p, "class=\"footer-content\"");

    const char *p = start_p;
    while ((p = strstr(p, "<img ")) != NULL && count < max_images) {
        if (end_p && p >= end_p) break;

        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;

        const char *src_pos = NULL;
        const char *dsrc = strstr(p, "data-src=\"");
        if (!dsrc || dsrc > tag_end) dsrc = strstr(p, "data-original=\"");
        if (!dsrc || dsrc > tag_end) dsrc = strstr(p, "data-src='");
        if (!dsrc || dsrc > tag_end) dsrc = strstr(p, "data-original='");

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
                                         strstr(low_url, ".svg") != NULL ||
                                         strstr(low_url, ".gif") != NULL);

                    bool is_image_ext = (strstr(low_url, ".webp") != NULL ||
                                         strstr(low_url, ".jpg") != NULL ||
                                         strstr(low_url, ".jpeg") != NULL ||
                                         strstr(low_url, ".png") != NULL);

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

const Scraper g_scraper_manganato = {
    .id = "manganato",
    .display_name = "MangaNato (manganato.gg)",
    .match = manganato_match,
    .scan_chapters = manganato_scan_chapters,
    .scan_images = manganato_scan_images
};
