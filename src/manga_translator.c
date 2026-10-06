#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <cjson/cJSON.h>

#include "manga_translator.h"
#include "pdf_converter.h"
#include "db_migration.h"
#include "lang.h"
#include "scrapers/scraper.h"

#define MAX_PATH_PAGES 2000

typedef struct {
    float ymin, xmin, ymax, xmax;
    char translation[1024];
} DetectedBubble;

typedef struct {
    wchar_t folder_name[MAX_PATH];
    wchar_t full_path[MAX_PATH];
    int chapter_num;
    int image_count;
} TransChapterInfo;

// UI Controls
static HWND hParentWnd = NULL;
static HINSTANCE hTransInst = NULL;
static HWND hTransFolderLbl = NULL;
static HWND hTransFolderEdit = NULL;
static HWND hTransBrowseBtn = NULL;
static HWND hTransOpenBtn = NULL;
static HWND hTransLangLbl = NULL;
static HWND hTransLangCombo = NULL;
static HWND hTransFilterLbl = NULL;
static HWND hTransFilterEdit = NULL;
static HWND hTransSaveImgChk = NULL;
static HWND hTransAutoPdfChk = NULL;
static HWND hTransStartBtn = NULL;
static HWND hTransStopBtn = NULL;
static HWND hTransStatusLbl = NULL;
static HWND hTransLogLbl = NULL;
static HWND hTransLogEdit = NULL;

static HWND g_trans_controls[20];
static int g_trans_ctrl_count = 0;

static volatile bool is_translating = false;
static volatile bool stop_trans_requested = false;
static HANDLE hTransThread = NULL;

static void apply_gui_font(HWND hwndCtrl) {
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(hwndCtrl, WM_SETFONT, (WPARAM)hFont, TRUE);
}

static void append_trans_log(const wchar_t *text) {
    if (!hTransLogEdit) return;
    int len = GetWindowTextLengthW(hTransLogEdit);
    SendMessageW(hTransLogEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(hTransLogEdit, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(hTransLogEdit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(hTransLogEdit, EM_SCROLLCARET, 0, 0);
}

// Deepest existing folder helper
static void find_deepest_existing_folder(const wchar_t *input_path, wchar_t *out_path, size_t max_len) {
    if (!input_path || input_path[0] == L'\0' || !out_path || max_len == 0) {
        if (out_path && max_len > 0) out_path[0] = L'\0';
        return;
    }

    wcsncpy(out_path, input_path, max_len - 1);
    out_path[max_len - 1] = L'\0';

    size_t len = wcslen(out_path);
    while (len > 0 && iswspace(out_path[0])) {
        wmemmove(out_path, out_path + 1, len);
        len--;
    }
    while (len > 0 && iswspace(out_path[len - 1])) {
        out_path[--len] = L'\0';
    }

    if (len >= 2 && out_path[0] == L'\"') {
        wmemmove(out_path, out_path + 1, len);
        len = wcslen(out_path);
        if (len > 0 && out_path[len - 1] == L'\"') out_path[--len] = L'\0';
    }

    while (len > 3 && (out_path[len - 1] == L'\\' || out_path[len - 1] == L'/')) {
        out_path[--len] = L'\0';
    }

    while (len > 0) {
        DWORD attr = GetFileAttributesW(out_path);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return;
        }

        wchar_t *p_slash = wcsrchr(out_path, L'\\');
        wchar_t *p_fwd = wcsrchr(out_path, L'/');
        if (p_fwd && (!p_slash || p_fwd > p_slash)) p_slash = p_fwd;

        if (!p_slash) {
            out_path[0] = L'\0';
            return;
        }

        if (p_slash == out_path + 2 && out_path[1] == L':') {
            *(p_slash + 1) = L'\0';
            DWORD root_attr = GetFileAttributesW(out_path);
            if (root_attr != INVALID_FILE_ATTRIBUTES && (root_attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return;
            }
            out_path[0] = L'\0';
            return;
        }

        *p_slash = L'\0';
        len = wcslen(out_path);
    }

    out_path[0] = L'\0';
}

static int CALLBACK BrowseFolderCallback(HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData) {
    if (uMsg == BFFM_INITIALIZED) {
        if (lpData && ((const wchar_t *)lpData)[0] != L'\0') {
            SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, lpData);
        }
    }
    return 0;
}

// Base64 encoder for vision image payloads
static char *base64_encode(const unsigned char *data, size_t input_length) {
    static const char encoding_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t output_length = 4 * ((input_length + 2) / 3);
    char *encoded_data = malloc(output_length + 1);
    if (!encoded_data) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        encoded_data[j++] = encoding_table[(triple >> 18) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 12) & 0x3F];
        encoded_data[j++] = (i > input_length + 1) ? '=' : encoding_table[(triple >> 6) & 0x3F];
        encoded_data[j++] = (i > input_length) ? '=' : encoding_table[triple & 0x3F];
    }
    encoded_data[output_length] = '\0';
    return encoded_data;
}

typedef struct {
    char *data;
    size_t size;
} MemChunk;

static size_t curl_mem_cb(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    MemChunk *mem = (MemChunk *)userp;
    char *p = realloc(mem->data, mem->size + total + 1);
    if (!p) return 0;
    mem->data = p;
    memcpy(mem->data + mem->size, ptr, total);
    mem->size += total;
    mem->data[mem->size] = '\0';
    return total;
}

// Check chapter filter e.g. "1-5", "10", empty = all
static bool is_chapter_in_filter(int ch_num, const wchar_t *filter_w) {
    if (!filter_w || filter_w[0] == L'\0') return true;

    char filter[256];
    WideCharToMultiByte(CP_UTF8, 0, filter_w, -1, filter, sizeof(filter), NULL, NULL);

    const char *p = filter;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        int start_ch = -1, end_ch = -1;
        start_ch = atoi(p);
        while (*p && isdigit((unsigned char)*p)) p++;

        while (*p == ' ') p++;
        if (*p == '-') {
            p++;
            while (*p == ' ') p++;
            if (_strnicmp(p, "end", 3) == 0) {
                end_ch = 999999;
                p += 3;
            } else if (isdigit((unsigned char)*p)) {
                end_ch = atoi(p);
                while (*p && isdigit((unsigned char)*p)) p++;
            }
        } else {
            end_ch = start_ch;
        }

        if (ch_num >= start_ch && ch_num <= end_ch) return true;
        while (*p && *p != ',') p++;
    }
    return false;
}

// Convert image file to compressed JPEG buffer for Vision API transmission
static bool image_to_jpeg_buffer(const wchar_t *img_path, BYTE **out_buf, DWORD *out_size, UINT *out_w, UINT *out_h) {
    *out_buf = NULL;
    *out_size = 0;
    *out_w = 0;
    *out_h = 0;

    IWICImagingFactory *pFactory = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&pFactory);
    if (FAILED(hr)) return false;

    IWICBitmapDecoder *pDecoder = NULL;
    hr = pFactory->lpVtbl->CreateDecoderFromFilename(
        pFactory, img_path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr)) {
        pFactory->lpVtbl->Release(pFactory);
        return false;
    }

    IWICBitmapFrameDecode *pFrame = NULL;
    hr = pDecoder->lpVtbl->GetFrame(pDecoder, 0, &pFrame);
    if (FAILED(hr)) {
        pDecoder->lpVtbl->Release(pDecoder);
        pFactory->lpVtbl->Release(pFactory);
        return false;
    }

    UINT w = 0, h = 0;
    pFrame->lpVtbl->GetSize(pFrame, &w, &h);
    *out_w = w;
    *out_h = h;

    IStream *pStream = NULL;
    CreateStreamOnHGlobal(NULL, TRUE, &pStream);

    IWICBitmapEncoder *pEncoder = NULL;
    pFactory->lpVtbl->CreateEncoder(pFactory, &GUID_ContainerFormatJpeg, NULL, &pEncoder);
    pEncoder->lpVtbl->Initialize(pEncoder, pStream, WICBitmapEncoderNoCache);

    IWICBitmapFrameEncode *pFrameEncode = NULL;
    IPropertyBag2 *pProp = NULL;
    pEncoder->lpVtbl->CreateNewFrame(pEncoder, &pFrameEncode, &pProp);

    PROPBAG2 opt = { 0 };
    opt.pstrName = L"ImageQuality";
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_R4;
    var.fltVal = 0.75f; // 75% quality for lightweight API transmission
    if (pProp) pProp->lpVtbl->Write(pProp, 1, &opt, &var);

    pFrameEncode->lpVtbl->Initialize(pFrameEncode, pProp);
    pFrameEncode->lpVtbl->SetSize(pFrameEncode, w, h);
    pFrameEncode->lpVtbl->WriteSource(pFrameEncode, (IWICBitmapSource *)pFrame, NULL);
    pFrameEncode->lpVtbl->Commit(pFrameEncode);
    pEncoder->lpVtbl->Commit(pEncoder);

    HGLOBAL hMem = NULL;
    GetHGlobalFromStream(pStream, &hMem);
    if (hMem) {
        SIZE_T s = GlobalSize(hMem);
        void *ptr = GlobalLock(hMem);
        if (ptr && s > 0) {
            *out_buf = (BYTE *)malloc(s);
            if (*out_buf) {
                memcpy(*out_buf, ptr, s);
                *out_size = (DWORD)s;
            }
            GlobalUnlock(hMem);
        }
    }

    if (pProp) pProp->lpVtbl->Release(pProp);
    pFrameEncode->lpVtbl->Release(pFrameEncode);
    pEncoder->lpVtbl->Release(pEncoder);
    pStream->lpVtbl->Release(pStream);
    pFrame->lpVtbl->Release(pFrame);
    pDecoder->lpVtbl->Release(pDecoder);
    pFactory->lpVtbl->Release(pFactory);

    return (*out_buf != NULL && *out_size > 0);
}

// Request AI Vision to detect text bubbles and translate to target language
static int call_vision_translate(const BYTE *jpeg_buf, DWORD jpeg_sz, const char *target_lang,
                                 DetectedBubble *bubbles, int max_bubbles) {
    if (!jpeg_buf || jpeg_sz == 0 || !bubbles || max_bubbles <= 0) return 0;

    wchar_t db_path_w[MAX_PATH];
    db_migration_get_db_path(db_path_w, MAX_PATH);
    char db_path[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, db_path_w, -1, db_path, MAX_PATH, NULL, NULL);

    sqlite3 *db = NULL;
    char api_key[512] = "";
    char model_id[256] = "";
    char api_url[512] = "https://openrouter.ai/api/v1";

    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, "SELECT api_key, url FROM server_agent WHERE is_active = 1 LIMIT 1;", -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *k = (const char *)sqlite3_column_text(stmt, 0);
                const char *u = (const char *)sqlite3_column_text(stmt, 1);
                if (k) strncpy(api_key, k, sizeof(api_key) - 1);
                if (u && u[0] != '\0') strncpy(api_url, u, sizeof(api_url) - 1);
            }
            sqlite3_finalize(stmt);
        }

        if (sqlite3_prepare_v2(db, "SELECT model_id FROM model_penggunaan ORDER BY priority_order ASC, id ASC LIMIT 1;", -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *m = (const char *)sqlite3_column_text(stmt, 0);
                if (m) strncpy(model_id, m, sizeof(model_id) - 1);
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    if (api_key[0] == '\0' || model_id[0] == '\0') {
        return 0; // AI API not configured
    }

    char *b64 = base64_encode(jpeg_buf, jpeg_sz);
    if (!b64) return 0;

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(b64);
        return 0;
    }

    char ep_url[1024];
    snprintf(ep_url, sizeof(ep_url), "%s/chat/completions", api_url);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model_id);

    cJSON *messages = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");

    char sys_prompt[512];
    snprintf(sys_prompt, sizeof(sys_prompt),
        "You are an expert manga localization translator. Detect all speech bubbles and text on this page. "
        "Return ONLY a valid JSON array of objects with percentage coordinates (0-100) and translation in %s: "
        "[{\"box\": [ymin, xmin, ymax, xmax], \"translation\": \"...\"}]. "
        "If no text bubbles exist, return []. Output raw JSON only with no markdown fences.",
        target_lang);
    cJSON_AddStringToObject(sys, "content", sys_prompt);
    cJSON_AddItemToArray(messages, sys);

    cJSON *usr = cJSON_CreateObject();
    cJSON_AddStringToObject(usr, "role", "user");
    cJSON *parts = cJSON_CreateArray();

    cJSON *p_text = cJSON_CreateObject();
    cJSON_AddStringToObject(p_text, "type", "text");
    cJSON_AddStringToObject(p_text, "text", "Translate all dialogue bubbles.");
    cJSON_AddItemToArray(parts, p_text);

    cJSON *p_img = cJSON_CreateObject();
    cJSON_AddStringToObject(p_img, "type", "image_url");
    cJSON *img_url = cJSON_CreateObject();

    char *data_url = malloc(strlen(b64) + 64);
    if (data_url) {
        sprintf(data_url, "data:image/jpeg;base64,%s", b64);
        cJSON_AddStringToObject(img_url, "url", data_url);
        free(data_url);
    }
    cJSON_AddItemToObject(p_img, "image_url", img_url);
    cJSON_AddItemToArray(parts, p_img);

    cJSON_AddItemToObject(usr, "content", parts);
    cJSON_AddItemToArray(messages, usr);
    cJSON_AddItemToObject(req, "messages", messages);
    cJSON_AddNumberToObject(req, "temperature", 0.1);

    char *payload = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    free(b64);

    struct curl_slist *headers = NULL;
    char auth_hdr[600];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", api_key);
    headers = curl_slist_append(headers, auth_hdr);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    MemChunk chunk = { malloc(1), 0 };
    chunk.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, ep_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_mem_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(payload);
    curl_easy_cleanup(curl);

    int count = 0;
    if (res == CURLE_OK && chunk.data) {
        cJSON *root = cJSON_Parse(chunk.data);
        if (root) {
            cJSON *choices = cJSON_GetObjectItem(root, "choices");
            if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                cJSON *first = cJSON_GetArrayItem(choices, 0);
                cJSON *msg = cJSON_GetObjectItem(first, "message");
                cJSON *content = cJSON_GetObjectItem(msg, "content");
                if (content && content->valuestring) {
                    const char *json_start = strchr(content->valuestring, '[');
                    const char *json_end = strrchr(content->valuestring, ']');
                    if (json_start && json_end && json_end > json_start) {
                        size_t jlen = json_end - json_start + 1;
                        char *jbuf = malloc(jlen + 1);
                        if (jbuf) {
                            strncpy(jbuf, json_start, jlen);
                            jbuf[jlen] = '\0';
                            cJSON *barr = cJSON_Parse(jbuf);
                            if (barr && cJSON_IsArray(barr)) {
                                int bsz = cJSON_GetArraySize(barr);
                                for (int i = 0; i < bsz && count < max_bubbles; i++) {
                                    cJSON *bitem = cJSON_GetArrayItem(barr, i);
                                    cJSON *c_box = cJSON_GetObjectItem(bitem, "box");
                                    cJSON *c_tr = cJSON_GetObjectItem(bitem, "translation");
                                    if (c_box && cJSON_IsArray(c_box) && cJSON_GetArraySize(c_box) == 4 && c_tr && c_tr->valuestring) {
                                        bubbles[count].ymin = (float)cJSON_GetArrayItem(c_box, 0)->valuedouble;
                                        bubbles[count].xmin = (float)cJSON_GetArrayItem(c_box, 1)->valuedouble;
                                        bubbles[count].ymax = (float)cJSON_GetArrayItem(c_box, 2)->valuedouble;
                                        bubbles[count].xmax = (float)cJSON_GetArrayItem(c_box, 3)->valuedouble;
                                        strncpy(bubbles[count].translation, c_tr->valuestring, sizeof(bubbles[count].translation) - 1);
                                        bubbles[count].translation[sizeof(bubbles[count].translation) - 1] = '\0';
                                        count++;
                                    }
                                }
                                cJSON_Delete(barr);
                            }
                            free(jbuf);
                        }
                    }
                }
            }
            cJSON_Delete(root);
        }
    }

    if (chunk.data) free(chunk.data);
    return count;
}

// Inpaint speech bubbles and typeset translated text onto manga page
static bool process_and_typeset_image(const wchar_t *src_img_path, const wchar_t *dst_img_path,
                                      DetectedBubble *bubbles, int bubble_count) {
    IWICImagingFactory *pFactory = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&pFactory);
    if (FAILED(hr)) return false;

    IWICBitmapDecoder *pDecoder = NULL;
    hr = pFactory->lpVtbl->CreateDecoderFromFilename(
        pFactory, src_img_path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr)) {
        pFactory->lpVtbl->Release(pFactory);
        return false;
    }

    IWICBitmapFrameDecode *pFrame = NULL;
    pDecoder->lpVtbl->GetFrame(pDecoder, 0, &pFrame);
    if (!pFrame) {
        pDecoder->lpVtbl->Release(pDecoder);
        pFactory->lpVtbl->Release(pFactory);
        return false;
    }

    UINT w = 0, h = 0;
    pFrame->lpVtbl->GetSize(pFrame, &w, &h);

    IWICFormatConverter *pConverter = NULL;
    pFactory->lpVtbl->CreateFormatConverter(pFactory, &pConverter);
    pConverter->lpVtbl->Initialize(pConverter, (IWICBitmapSource *)pFrame, &GUID_WICPixelFormat32bppBGR,
                                   WICBitmapDitherTypeNone, NULL, 0.0f, WICBitmapPaletteTypeCustom);

    // Create DIB section to hold bitmap pixels for GDI drawing
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -(LONG)h; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(hdcMem, hBmp);

    // Copy WIC pixels to DIB bits
    pConverter->lpVtbl->CopyPixels(pConverter, NULL, w * 4, w * h * 4, (BYTE *)bits);

    // Inpaint & Typeset each detected bubble
    for (int b = 0; b < bubble_count; b++) {
        int x1 = (int)((bubbles[b].xmin / 100.0f) * w);
        int y1 = (int)((bubbles[b].ymin / 100.0f) * h);
        int x2 = (int)((bubbles[b].xmax / 100.0f) * w);
        int y2 = (int)((bubbles[b].ymax / 100.0f) * h);

        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > (int)w) x2 = (int)w;
        if (y2 > (int)h) y2 = (int)h;

        int bw = x2 - x1;
        int bh = y2 - y1;
        if (bw < 10 || bh < 10) continue;

        // Inpaint: Fill clean white speech bubble
        HBRUSH hWhiteBrush = CreateSolidBrush(RGB(255, 255, 255));
        HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
        SelectObject(hdcMem, hWhiteBrush);
        SelectObject(hdcMem, hPen);
        RoundRect(hdcMem, x1, y1, x2, y2, 20, 20);
        DeleteObject(hWhiteBrush);
        DeleteObject(hPen);

        // Convert translation text to UTF-16
        wchar_t wText[1024];
        MultiByteToWideChar(CP_UTF8, 0, bubbles[b].translation, -1, wText, 1024);

        // Adaptive font sizing (auto-fit into bubble)
        int best_size = 18;
        if (bh < 60 || bw < 100) best_size = 14;
        if (bh < 40 || bw < 70) best_size = 11;

        HFONT hFont = CreateFontW(best_size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Comic Sans MS");
        SelectObject(hdcMem, hFont);
        SetTextColor(hdcMem, RGB(0, 0, 0));
        SetBkMode(hdcMem, TRANSPARENT);

        RECT rcText = { x1 + 6, y1 + 6, x2 - 6, y2 - 6 };
        DrawTextW(hdcMem, wText, -1, &rcText, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);

        DeleteObject(hFont);
    }

    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    // Save final modified image to destination path
    IWICBitmap *pOutBmp = NULL;
    pFactory->lpVtbl->CreateBitmapFromMemory(pFactory, w, h, &GUID_WICPixelFormat32bppBGR,
                                             w * 4, w * h * 4, (BYTE *)bits, &pOutBmp);

    IWICStream *pStream = NULL;
    pFactory->lpVtbl->CreateStream(pFactory, &pStream);
    pStream->lpVtbl->InitializeFromFilename(pStream, dst_img_path, GENERIC_WRITE);

    IWICBitmapEncoder *pEncoder = NULL;
    pFactory->lpVtbl->CreateEncoder(pFactory, &GUID_ContainerFormatJpeg, NULL, &pEncoder);
    pEncoder->lpVtbl->Initialize(pEncoder, (IStream *)pStream, WICBitmapEncoderNoCache);

    IWICBitmapFrameEncode *pOutFrame = NULL;
    pEncoder->lpVtbl->CreateNewFrame(pEncoder, &pOutFrame, NULL);
    pOutFrame->lpVtbl->Initialize(pOutFrame, NULL);
    pOutFrame->lpVtbl->SetSize(pOutFrame, w, h);
    pOutFrame->lpVtbl->WriteSource(pOutFrame, (IWICBitmapSource *)pOutBmp, NULL);
    pOutFrame->lpVtbl->Commit(pOutFrame);
    pEncoder->lpVtbl->Commit(pEncoder);

    pOutFrame->lpVtbl->Release(pOutFrame);
    pEncoder->lpVtbl->Release(pEncoder);
    pStream->lpVtbl->Release(pStream);
    pOutBmp->lpVtbl->Release(pOutBmp);

    DeleteObject(hBmp);
    pConverter->lpVtbl->Release(pConverter);
    pFrame->lpVtbl->Release(pFrame);
    pDecoder->lpVtbl->Release(pDecoder);
    pFactory->lpVtbl->Release(pFactory);

    return true;
}

// Background thread procedure for Manga Translation
static DWORD WINAPI MangaTranslationThreadProc(LPVOID lpParam) {
    CoInitialize(NULL);

    wchar_t manga_root[MAX_PATH];
    GetWindowTextW(hTransFolderEdit, manga_root, MAX_PATH);

    wchar_t filter_w[256];
    GetWindowTextW(hTransFilterEdit, filter_w, 256);

    int lang_idx = (int)SendMessageW(hTransLangCombo, CB_GETCURSEL, 0, 0);
    const char *target_lang_code = "Indonesian";
    const wchar_t *lang_tag = L"[ID]";
    if (lang_idx == 1) { target_lang_code = "English"; lang_tag = L"[EN]"; }
    else if (lang_idx == 2) { target_lang_code = "Japanese"; lang_tag = L"[JA]"; }

    bool auto_pdf = (SendMessageW(hTransAutoPdfChk, BM_GETCHECK, 0, 0) == BST_CHECKED);

    append_trans_log(L"=========================================");
    append_trans_log(L"[1/4] Memulai verifikasi & pemindaian folder chapter...");

    TransChapterInfo *chapters = (TransChapterInfo *)calloc(MAX_CHAPTERS, sizeof(TransChapterInfo));
    int total_ch = 0;

    wchar_t pat[MAX_PATH];
    _snwprintf(pat, MAX_PATH, L"%s\\*.*", manga_root);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pat, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0 &&
                wcsstr(fd.cFileName, L"[ID]") == NULL && wcsstr(fd.cFileName, L"[EN]") == NULL &&
                wcsstr(fd.cFileName, L"[JA]") == NULL) {

                char c_name[MAX_PATH];
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, c_name, MAX_PATH, NULL, NULL);

                int ch_num = extract_chapter_number_from_string(c_name);
                if (ch_num >= 0 && total_ch < MAX_CHAPTERS) {
                    if (is_chapter_in_filter(ch_num, filter_w)) {
                        wcsncpy(chapters[total_ch].folder_name, fd.cFileName, MAX_PATH - 1);
                        _snwprintf(chapters[total_ch].full_path, MAX_PATH, L"%s\\%s", manga_root, fd.cFileName);
                        chapters[total_ch].chapter_num = ch_num;

                        // Scan images count
                        wchar_t img_pat[MAX_PATH];
                        _snwprintf(img_pat, MAX_PATH, L"%s\\*.*", chapters[total_ch].full_path);
                        WIN32_FIND_DATAW ifd;
                        HANDLE hImg = FindFirstFileW(img_pat, &ifd);
                        int icnt = 0;
                        if (hImg != INVALID_HANDLE_VALUE) {
                            do {
                                if (!(ifd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                                    const wchar_t *ext = wcsrchr(ifd.cFileName, L'.');
                                    if (ext && (_wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0 ||
                                                _wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".webp") == 0)) {
                                        icnt++;
                                    }
                                }
                            } while (FindNextFileW(hImg, &ifd));
                            FindClose(hImg);
                        }
                        chapters[total_ch].image_count = icnt;
                        total_ch++;
                    }
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    if (total_ch == 0) {
        append_trans_log(L"[Error] Tidak ditemukan folder chapter yang lolos filter.");
        free(chapters);
        CoUninitialize();
        is_translating = false;
        EnableWindow(hTransStartBtn, TRUE);
        EnableWindow(hTransStopBtn, FALSE);
        MessageBeep(MB_ICONASTERISK);
        return 0;
    }

    wchar_t logmsg[512];
    _snwprintf(logmsg, sizeof(logmsg)/sizeof(wchar_t), L"-> Ditemukan %d chapter yang akan diterjemahkan.", total_ch);
    append_trans_log(logmsg);

    int total_pages_translated = 0;
    int success_ch = 0;

    // Process each chapter
    for (int c = 0; c < total_ch; c++) {
        if (stop_trans_requested) break;

        _snwprintf(logmsg, sizeof(logmsg)/sizeof(wchar_t), L"\n--- [%d/%d] Menerjemahkan %s (%d halaman) ---",
                   c + 1, total_ch, chapters[c].folder_name, chapters[c].image_count);
        append_trans_log(logmsg);

        if (chapters[c].image_count == 0) {
            append_trans_log(L"-> [Skip] Chapter kosong, tidak ada gambar.");
            continue;
        }

        // Create destination folder: <MangaRoot>\<ChapterName> [ID]
        wchar_t dest_chapter_dir[MAX_PATH];
        _snwprintf(dest_chapter_dir, MAX_PATH, L"%s\\%s %s", manga_root, chapters[c].folder_name, lang_tag);
        CreateDirectoryW(dest_chapter_dir, NULL);

        // Collect and sort images
        wchar_t img_pat[MAX_PATH];
        _snwprintf(img_pat, MAX_PATH, L"%s\\*.*", chapters[c].full_path);
        WIN32_FIND_DATAW ifd;
        HANDLE hImg = FindFirstFileW(img_pat, &ifd);

        wchar_t **src_imgs = (wchar_t **)calloc(chapters[c].image_count, sizeof(wchar_t *));
        wchar_t **dst_imgs = (wchar_t **)calloc(chapters[c].image_count, sizeof(wchar_t *));
        int icnt = 0;

        if (hImg != INVALID_HANDLE_VALUE) {
            do {
                if (!(ifd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    const wchar_t *ext = wcsrchr(ifd.cFileName, L'.');
                    if (ext && (_wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0 ||
                                _wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".webp") == 0)) {
                        if (icnt < chapters[c].image_count) {
                            src_imgs[icnt] = (wchar_t *)malloc(MAX_PATH * sizeof(wchar_t));
                            dst_imgs[icnt] = (wchar_t *)malloc(MAX_PATH * sizeof(wchar_t));
                            _snwprintf(src_imgs[icnt], MAX_PATH, L"%s\\%s", chapters[c].full_path, ifd.cFileName);
                            _snwprintf(dst_imgs[icnt], MAX_PATH, L"%s\\%03d.jpg", dest_chapter_dir, icnt + 1);
                            icnt++;
                        }
                    }
                }
            } while (FindNextFileW(hImg, &ifd));
            FindClose(hImg);
        }

        // Translate each image page
        int ch_pages_done = 0;
        for (int p = 0; p < icnt; p++) {
            if (stop_trans_requested) break;

            BYTE *jpeg_buf = NULL;
            DWORD jpeg_sz = 0;
            UINT w = 0, h = 0;
            DetectedBubble bubbles[64];
            int bcount = 0;

            if (image_to_jpeg_buffer(src_imgs[p], &jpeg_buf, &jpeg_sz, &w, &h) && jpeg_buf) {
                bcount = call_vision_translate(jpeg_buf, jpeg_sz, target_lang_code, bubbles, 64);
                free(jpeg_buf);
            }

            if (bcount > 0) {
                _snwprintf(logmsg, sizeof(logmsg)/sizeof(wchar_t), L"-> Hal %03d: Terdeteksi %d balon percakapan, menerjemahkan...", p + 1, bcount);
                append_trans_log(logmsg);
            }

            process_and_typeset_image(src_imgs[p], dst_imgs[p], bubbles, bcount);
            ch_pages_done++;
            total_pages_translated++;
        }

        // Auto PDF generation for translated chapter if requested
        if (auto_pdf && ch_pages_done > 0 && !stop_trans_requested) {
            wchar_t pdf_out[MAX_PATH];
            _snwprintf(pdf_out, MAX_PATH, L"%s\\%s %s.pdf", manga_root, chapters[c].folder_name, lang_tag);
            if (create_pdf_from_images(pdf_out, (const wchar_t **)dst_imgs, ch_pages_done, &stop_trans_requested)) {
                _snwprintf(logmsg, sizeof(logmsg)/sizeof(wchar_t), L"-> [PDF Sukses] Dibuat: %s %s.pdf", chapters[c].folder_name, lang_tag);
                append_trans_log(logmsg);
            }
        }

        for (int k = 0; k < icnt; k++) {
            free(src_imgs[k]);
            free(dst_imgs[k]);
        }
        free(src_imgs);
        free(dst_imgs);

        success_ch++;
    }

    free(chapters);
    CoUninitialize();

    append_trans_log(L"\n=========================================");
    if (stop_trans_requested) {
        append_trans_log(L"STATUS: Penerjemahan DIBATALKAN oleh pengguna.");
    } else {
        append_trans_log(L"STATUS: SEMUA PENERJEMAHAN BERHASIL SELESAI!");
    }
    append_trans_log(L"=========================================");

    // Sound notification on completion
    MessageBeep(MB_ICONASTERISK);

    wchar_t mb_msg[512];
    _snwprintf(mb_msg, sizeof(mb_msg)/sizeof(wchar_t),
               L"%s\n\n- Chapter Selesai: %d\n- Total Halaman Diterjemahkan: %d",
               _TW("str_trans_alert_success_summary"), success_ch, total_pages_translated);
    MessageBoxW(hParentWnd, mb_msg, _TW("str_trans_alert_result_title"), MB_OK | MB_ICONINFORMATION);

    is_translating = false;
    EnableWindow(hTransStartBtn, TRUE);
    EnableWindow(hTransStopBtn, FALSE);
    return 0;
}

void manga_translator_init(HWND hwndParent, HINSTANCE hInst) {
    hParentWnd = hwndParent;
    hTransInst = hInst;
    g_trans_ctrl_count = 0;

    // Folder Label
    hTransFolderLbl = CreateWindowW(L"STATIC", _TW("str_trans_folder_label"), WS_CHILD,
                                   25, 38, 475, 18, hwndParent, NULL, hInst, NULL);
    apply_gui_font(hTransFolderLbl);
    g_trans_controls[g_trans_ctrl_count++] = hTransFolderLbl;

    // Folder Edit
    hTransFolderEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | ES_AUTOHSCROLL,
                                      25, 58, 475, 24, hwndParent, (HMENU)ID_TRANS_FOLDER_EDIT, hInst, NULL);
    apply_gui_font(hTransFolderEdit);
    g_trans_controls[g_trans_ctrl_count++] = hTransFolderEdit;

    // Browse Button
    hTransBrowseBtn = CreateWindowW(L"BUTTON", _TW("str_btn_browse"), WS_CHILD | BS_PUSHBUTTON,
                                   508, 57, 90, 26, hwndParent, (HMENU)ID_TRANS_BROWSE_BTN, hInst, NULL);
    apply_gui_font(hTransBrowseBtn);
    g_trans_controls[g_trans_ctrl_count++] = hTransBrowseBtn;

    // Open Folder Button
    hTransOpenBtn = CreateWindowW(L"BUTTON", _TW("str_pdf_btn_open"), WS_CHILD | BS_PUSHBUTTON,
                                 605, 57, 95, 26, hwndParent, (HMENU)ID_TRANS_OPEN_BTN, hInst, NULL);
    apply_gui_font(hTransOpenBtn);
    g_trans_controls[g_trans_ctrl_count++] = hTransOpenBtn;

    // Target Language Label
    hTransLangLbl = CreateWindowW(L"STATIC", _TW("str_trans_lang_label"), WS_CHILD,
                                 25, 92, 220, 18, hwndParent, NULL, hInst, NULL);
    apply_gui_font(hTransLangLbl);
    g_trans_controls[g_trans_ctrl_count++] = hTransLangLbl;

    // Target Language ComboBox
    hTransLangCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
                                     25, 112, 220, 120, hwndParent, (HMENU)ID_TRANS_TARGET_LANG_COMBO, hInst, NULL);
    apply_gui_font(hTransLangCombo);
    g_trans_controls[g_trans_ctrl_count++] = hTransLangCombo;

    SendMessageW(hTransLangCombo, CB_ADDSTRING, 0, (LPARAM)L"Bahasa Indonesia (ID)");
    SendMessageW(hTransLangCombo, CB_ADDSTRING, 0, (LPARAM)L"English (EN)");
    SendMessageW(hTransLangCombo, CB_ADDSTRING, 0, (LPARAM)L"Japanese (JA)");
    SendMessageW(hTransLangCombo, CB_SETCURSEL, 0, 0);

    // Chapter Filter Label
    hTransFilterLbl = CreateWindowW(L"STATIC", _TW("str_trans_filter_label"), WS_CHILD,
                                   265, 92, 435, 18, hwndParent, NULL, hInst, NULL);
    apply_gui_font(hTransFilterLbl);
    g_trans_controls[g_trans_ctrl_count++] = hTransFilterLbl;

    // Chapter Filter Edit
    hTransFilterEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | ES_AUTOHSCROLL,
                                      265, 112, 435, 24, hwndParent, (HMENU)ID_TRANS_CHAPTER_FILTER, hInst, NULL);
    apply_gui_font(hTransFilterEdit);
    g_trans_controls[g_trans_ctrl_count++] = hTransFilterEdit;

    // Checkbox: Save translated images
    hTransSaveImgChk = CreateWindowW(L"BUTTON", _TW("str_trans_save_img_chk"),
                                    WS_CHILD | BS_AUTOCHECKBOX,
                                    25, 148, 675, 20, hwndParent, (HMENU)ID_TRANS_SAVE_IMG_CHK, hInst, NULL);
    apply_gui_font(hTransSaveImgChk);
    g_trans_controls[g_trans_ctrl_count++] = hTransSaveImgChk;
    SendMessageW(hTransSaveImgChk, BM_SETCHECK, BST_CHECKED, 0);

    // Checkbox: Auto generate PDF
    hTransAutoPdfChk = CreateWindowW(L"BUTTON", _TW("str_trans_auto_pdf_chk"),
                                    WS_CHILD | BS_AUTOCHECKBOX,
                                    25, 172, 675, 20, hwndParent, (HMENU)ID_TRANS_AUTO_PDF_CHK, hInst, NULL);
    apply_gui_font(hTransAutoPdfChk);
    g_trans_controls[g_trans_ctrl_count++] = hTransAutoPdfChk;
    SendMessageW(hTransAutoPdfChk, BM_SETCHECK, BST_CHECKED, 0);

    // Button: Start Translation
    hTransStartBtn = CreateWindowW(L"BUTTON", _TW("str_trans_btn_start"),
                                  WS_CHILD | BS_DEFPUSHBUTTON,
                                  25, 204, 160, 32, hwndParent, (HMENU)ID_TRANS_START_BTN, hInst, NULL);
    apply_gui_font(hTransStartBtn);
    g_trans_controls[g_trans_ctrl_count++] = hTransStartBtn;

    // Button: Stop
    hTransStopBtn = CreateWindowW(L"BUTTON", _TW("str_trans_btn_stop"),
                                 WS_CHILD | BS_PUSHBUTTON | WS_DISABLED,
                                 195, 204, 90, 32, hwndParent, (HMENU)ID_TRANS_STOP_BTN, hInst, NULL);
    apply_gui_font(hTransStopBtn);
    g_trans_controls[g_trans_ctrl_count++] = hTransStopBtn;

    // Status Label
    hTransStatusLbl = CreateWindowW(L"STATIC", _TW("str_trans_status_idle"), WS_CHILD | SS_LEFTNOWORDWRAP,
                                   295, 212, 400, 20, hwndParent, (HMENU)ID_TRANS_STATUS_LBL, hInst, NULL);
    apply_gui_font(hTransStatusLbl);
    g_trans_controls[g_trans_ctrl_count++] = hTransStatusLbl;

    // Log Label
    hTransLogLbl = CreateWindowW(L"STATIC", _TW("str_trans_log_label"), WS_CHILD,
                                25, 246, 300, 18, hwndParent, NULL, hInst, NULL);
    apply_gui_font(hTransLogLbl);
    g_trans_controls[g_trans_ctrl_count++] = hTransLogLbl;

    // Log Edit box
    hTransLogEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                   25, 268, 675, 334, hwndParent, (HMENU)ID_TRANS_LOG_EDIT, hInst, NULL);
    apply_gui_font(hTransLogEdit);
    g_trans_controls[g_trans_ctrl_count++] = hTransLogEdit;
}

void manga_translator_switch_tab(bool show) {
    int cmd = show ? SW_SHOW : SW_HIDE;
    for (int i = 0; i < g_trans_ctrl_count; i++) {
        if (g_trans_controls[i]) ShowWindow(g_trans_controls[i], cmd);
    }
}

void manga_translator_on_command(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    WORD id = LOWORD(wParam);

    if (id == ID_TRANS_BROWSE_BTN) {
        wchar_t currentPath[MAX_PATH];
        GetWindowTextW(hTransFolderEdit, currentPath, MAX_PATH);

        wchar_t initialDir[MAX_PATH];
        find_deepest_existing_folder(currentPath, initialDir, MAX_PATH);

        BROWSEINFOW bi = { 0 };
        bi.hwndOwner = hwnd;
        bi.lpszTitle = _TW("str_browse_title");
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        bi.lpfn = BrowseFolderCallback;
        bi.lParam = (LPARAM)initialDir;

        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl != 0) {
            wchar_t chosenPath[MAX_PATH];
            if (SHGetPathFromIDListW(pidl, chosenPath)) {
                SetWindowTextW(hTransFolderEdit, chosenPath);
            }
            CoTaskMemFree(pidl);
        }
    } else if (id == ID_TRANS_OPEN_BTN) {
        wchar_t path[MAX_PATH];
        GetWindowTextW(hTransFolderEdit, path, MAX_PATH);
        if (wcslen(path) > 0) {
            ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
        }
    } else if (id == ID_TRANS_START_BTN) {
        if (is_translating) return;

        wchar_t path[MAX_PATH];
        GetWindowTextW(hTransFolderEdit, path, MAX_PATH);
        if (wcslen(path) == 0) {
            MessageBoxW(hwnd, _TW("str_trans_alert_folder_empty"), _TW("str_alert_warning"), MB_OK | MB_ICONWARNING);
            return;
        }

        stop_trans_requested = false;
        is_translating = true;
        EnableWindow(hTransStartBtn, FALSE);
        EnableWindow(hTransStopBtn, TRUE);

        if (hTransThread) {
            CloseHandle(hTransThread);
            hTransThread = NULL;
        }
        hTransThread = CreateThread(NULL, 0, MangaTranslationThreadProc, NULL, 0, NULL);
    } else if (id == ID_TRANS_STOP_BTN) {
        if (is_translating) {
            stop_trans_requested = true;
            append_trans_log(L"[Info] Mengirim permintaan pembatalan penerjemahan...");
            EnableWindow(hTransStopBtn, FALSE);
        }
    }
}

void manga_translator_refresh_lang(void) {
    if (hTransFolderLbl) SetWindowTextW(hTransFolderLbl, _TW("str_trans_folder_label"));
    if (hTransBrowseBtn) SetWindowTextW(hTransBrowseBtn, _TW("str_btn_browse"));
    if (hTransOpenBtn) SetWindowTextW(hTransOpenBtn, _TW("str_pdf_btn_open"));
    if (hTransLangLbl) SetWindowTextW(hTransLangLbl, _TW("str_trans_lang_label"));
    if (hTransFilterLbl) SetWindowTextW(hTransFilterLbl, _TW("str_trans_filter_label"));
    if (hTransSaveImgChk) SetWindowTextW(hTransSaveImgChk, _TW("str_trans_save_img_chk"));
    if (hTransAutoPdfChk) SetWindowTextW(hTransAutoPdfChk, _TW("str_trans_auto_pdf_chk"));
    if (hTransStartBtn) SetWindowTextW(hTransStartBtn, _TW("str_trans_btn_start"));
    if (hTransStopBtn) SetWindowTextW(hTransStopBtn, _TW("str_trans_btn_stop"));
    if (hTransStatusLbl) SetWindowTextW(hTransStatusLbl, _TW("str_trans_status_idle"));
    if (hTransLogLbl) SetWindowTextW(hTransLogLbl, _TW("str_trans_log_label"));
}
