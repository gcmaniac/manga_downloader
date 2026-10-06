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

#include "pdf_converter.h"
#include "db_migration.h"
#include "lang.h"
#include "scrapers/scraper.h"

#define MAX_VOL_ITEMS 500
#define MAX_PATH_PAGES 2000

typedef struct {
    int volume_num;
    int start_chapter;
    int end_chapter;
    char title[128];
} VolumeMapItem;

typedef struct {
    wchar_t folder_name[MAX_PATH];
    wchar_t full_path[MAX_PATH];
    int chapter_num;
    int image_count;
} ChapterFolderInfo;

// UI Handles
static HWND hParentWnd = NULL;
static HINSTANCE hPdfInst = NULL;
static HWND hPdfFolderLbl = NULL;
static HWND hPdfFolderEdit = NULL;
static HWND hPdfBrowseBtn = NULL;
static HWND hPdfOpenBtn = NULL;
static HWND hPdfModeLbl = NULL;
static HWND hPdfRadioChapter = NULL;
static HWND hPdfRadioVolume = NULL;
static HWND hPdfRadioVolCustom = NULL;
static HWND hPdfRadioVolAuto = NULL;
static HWND hPdfVolCountEdit = NULL;
static HWND hPdfVolPerLbl = NULL;
static HWND hPdfVolTextarea = NULL;
static HWND hPdfStartBtn = NULL;
static HWND hPdfStopBtn = NULL;
static HWND hPdfStatusLbl = NULL;
static HWND hPdfLogLbl = NULL;
static HWND hPdfLogEdit = NULL;

static HWND g_pdf_controls[20];
static int g_pdf_ctrl_count = 0;

static volatile bool is_converting = false;
static volatile bool stop_pdf_requested = false;
static HANDLE hPdfThread = NULL;

static void apply_gui_font(HWND hwndCtrl) {
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(hwndCtrl, WM_SETFONT, (WPARAM)hFont, TRUE);
}

static void append_pdf_log(const wchar_t *text) {
    if (!hPdfLogEdit) return;
    int len = GetWindowTextLengthW(hPdfLogEdit);
    SendMessageW(hPdfLogEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(hPdfLogEdit, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(hPdfLogEdit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(hPdfLogEdit, EM_SCROLLCARET, 0, 0);
}

// Deepest existing folder helper for Browse button
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

// Convert any image (WebP, PNG, JPG) to JPEG in memory using Windows Imaging Component (WIC)
static bool image_file_to_jpeg_buffer(const wchar_t *img_path, BYTE **out_buf, DWORD *out_size, UINT *out_w, UINT *out_h) {
    *out_buf = NULL;
    *out_size = 0;
    *out_w = 0;
    *out_h = 0;

    // Direct check for fast-path JPEG (avoid re-encoding if already JPEG)
    const wchar_t *dot = wcsrchr(img_path, L'.');
    if (dot && (_wcsicmp(dot, L".jpg") == 0 || _wcsicmp(dot, L".jpeg") == 0)) {
        FILE *f = _wfopen(img_path, L"rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                BYTE *buf = (BYTE *)malloc(sz);
                if (buf && fread(buf, 1, sz, f) == (size_t)sz) {
                    // Extract dimensions using simple JPEG marker scan
                    UINT w = 0, h = 0;
                    for (long i = 0; i < sz - 8; i++) {
                        if (buf[i] == 0xFF && (buf[i+1] == 0xC0 || buf[i+1] == 0xC2)) { // SOF0 or SOF2
                            h = ((UINT)buf[i+5] << 8) | buf[i+6];
                            w = ((UINT)buf[i+7] << 8) | buf[i+8];
                            break;
                        }
                    }
                    if (w > 0 && h > 0) {
                        fclose(f);
                        *out_buf = buf;
                        *out_size = (DWORD)sz;
                        *out_w = w;
                        *out_h = h;
                        return true;
                    }
                }
                if (buf) free(buf);
            }
            fclose(f);
        }
    }

    // Universal decoder via WIC (handles WebP, PNG, BMP, and any JPG without SOF0)
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
    hr = CreateStreamOnHGlobal(NULL, TRUE, &pStream);
    if (FAILED(hr)) {
        pFrame->lpVtbl->Release(pFrame);
        pDecoder->lpVtbl->Release(pDecoder);
        pFactory->lpVtbl->Release(pFactory);
        return false;
    }

    IWICBitmapEncoder *pEncoder = NULL;
    hr = pFactory->lpVtbl->CreateEncoder(pFactory, &GUID_ContainerFormatJpeg, NULL, &pEncoder);
    if (FAILED(hr)) {
        pStream->lpVtbl->Release(pStream);
        pFrame->lpVtbl->Release(pFrame);
        pDecoder->lpVtbl->Release(pDecoder);
        pFactory->lpVtbl->Release(pFactory);
        return false;
    }

    pEncoder->lpVtbl->Initialize(pEncoder, pStream, WICBitmapEncoderNoCache);

    IWICBitmapFrameEncode *pFrameEncode = NULL;
    IPropertyBag2 *pPropertybag = NULL;
    pEncoder->lpVtbl->CreateNewFrame(pEncoder, &pFrameEncode, &pPropertybag);

    PROPBAG2 option = { 0 };
    option.pstrName = L"ImageQuality";
    VARIANT varValue;
    VariantInit(&varValue);
    varValue.vt = VT_R4;
    varValue.fltVal = 0.90f; // High quality 90%
    if (pPropertybag) {
        pPropertybag->lpVtbl->Write(pPropertybag, 1, &option, &varValue);
    }

    pFrameEncode->lpVtbl->Initialize(pFrameEncode, pPropertybag);
    pFrameEncode->lpVtbl->SetSize(pFrameEncode, w, h);

    hr = pFrameEncode->lpVtbl->WriteSource(pFrameEncode, (IWICBitmapSource *)pFrame, NULL);
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

    if (pPropertybag) pPropertybag->lpVtbl->Release(pPropertybag);
    pFrameEncode->lpVtbl->Release(pFrameEncode);
    pEncoder->lpVtbl->Release(pEncoder);
    pStream->lpVtbl->Release(pStream);
    pFrame->lpVtbl->Release(pFrame);
    pDecoder->lpVtbl->Release(pDecoder);
    pFactory->lpVtbl->Release(pFactory);

    return (*out_buf != NULL && *out_size > 0);
}

// Generate multi-page PDF document from a list of image file paths
bool create_pdf_from_images(const wchar_t *pdf_output_path, const wchar_t **image_paths, int image_count, volatile bool *p_stop) {
    if (!pdf_output_path || !image_paths || image_count <= 0) return false;

    FILE *pdf = _wfopen(pdf_output_path, L"wb");
    if (!pdf) return false;

    // Header
    fprintf(pdf, "%%PDF-1.4\n%%\xE2\xE3\xCF\xD3\n");

    // Total objects:
    // Obj 1: Catalog
    // Obj 2: Pages
    // For each page i (0 to N-1):
    //   Obj 3 + 3*i : Page object
    //   Obj 4 + 3*i : Contents stream (renders image)
    //   Obj 5 + 3*i : Image XObject (JPEG stream)
    int total_objs = 2 + (image_count * 3);
    long *xref = (long *)calloc(total_objs + 1, sizeof(long));
    if (!xref) {
        fclose(pdf);
        _wremove(pdf_output_path);
        return false;
    }

    // 1 0 obj: Catalog
    xref[1] = ftell(pdf);
    fprintf(pdf, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    // 2 0 obj: Pages
    xref[2] = ftell(pdf);
    fprintf(pdf, "2 0 obj\n<< /Type /Pages /Kids [");
    for (int i = 0; i < image_count; i++) {
        fprintf(pdf, "%d 0 R%s", 3 + (3 * i), (i + 1 < image_count) ? " " : "");
    }
    fprintf(pdf, "] /Count %d >>\nendobj\n", image_count);

    // Write each page
    for (int i = 0; i < image_count; i++) {
        if (p_stop && *p_stop) {
            free(xref);
            fclose(pdf);
            _wremove(pdf_output_path);
            return false;
        }

        BYTE *jpeg_buf = NULL;
        DWORD jpeg_sz = 0;
        UINT w = 0, h = 0;
        if (!image_file_to_jpeg_buffer(image_paths[i], &jpeg_buf, &jpeg_sz, &w, &h) || !jpeg_buf || jpeg_sz == 0) {
            // Fallback: 100x100 blank placeholder if corrupt
            w = 100; h = 100;
        }

        int obj_page = 3 + (3 * i);
        int obj_content = 4 + (3 * i);
        int obj_image = 5 + (3 * i);

        // Page Object
        xref[obj_page] = ftell(pdf);
        fprintf(pdf, "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %u %u] /Contents %d 0 R /Resources << /XObject << /Im%d %d 0 R >> >> >>\nendobj\n",
                obj_page, w, h, obj_content, i + 1, obj_image);

        // Content Stream
        xref[obj_content] = ftell(pdf);
        char stream_buf[256];
        int slen = snprintf(stream_buf, sizeof(stream_buf), "q\n%u 0 0 %u 0 0 cm\n/Im%d Do\nQ\n", w, h, i + 1);
        fprintf(pdf, "%d 0 obj\n<< /Length %d >>\nstream\n%sendstream\nendobj\n", obj_content, slen, stream_buf);

        // Image XObject
        xref[obj_image] = ftell(pdf);
        if (jpeg_buf && jpeg_sz > 0) {
            fprintf(pdf, "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %u /Height %u /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length %lu >>\nstream\n",
                    obj_image, w, h, jpeg_sz);
            fwrite(jpeg_buf, 1, jpeg_sz, pdf);
            fprintf(pdf, "\nendstream\nendobj\n");
            free(jpeg_buf);
        } else {
            fprintf(pdf, "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 3 >>\nstream\n\xFF\xFF\xFF\nendstream\nendobj\n", obj_image);
        }
    }

    // Xref Table & Trailer
    long xref_pos = ftell(pdf);
    fprintf(pdf, "xref\n0 %d\n0000000000 65535 f \n", total_objs + 1);
    for (int i = 1; i <= total_objs; i++) {
        fprintf(pdf, "%010ld 00000 n \n", xref[i]);
    }
    fprintf(pdf, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", total_objs + 1, xref_pos);

    free(xref);
    fclose(pdf);
    return true;
}

// Compare chapter folders chronologically by extracted chapter number
static int compare_chapters_asc(const void *a, const void *b) {
    const ChapterFolderInfo *ca = (const ChapterFolderInfo *)a;
    const ChapterFolderInfo *cb = (const ChapterFolderInfo *)b;
    if (ca->chapter_num != cb->chapter_num) {
        return ca->chapter_num - cb->chapter_num;
    }
    return _wcsicmp(ca->folder_name, cb->folder_name);
}

// Compare image paths by filename
static int compare_image_paths(const void *a, const void *b) {
    const wchar_t *pa = *(const wchar_t **)a;
    const wchar_t *pb = *(const wchar_t **)b;
    return _wcsicmp(pa, pb);
}

// Parse volumes from textarea text using active OpenRouter AI Model or local line-by-line fallback
typedef struct {
    char *data;
    size_t size;
} MemoryChunk;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    MemoryChunk *mem = (MemoryChunk *)userp;
    char *p = realloc(mem->data, mem->size + total + 1);
    if (!p) return 0;
    mem->data = p;
    memcpy(mem->data + mem->size, ptr, total);
    mem->size += total;
    mem->data[mem->size] = '\0';
    return total;
}

static int parse_volumes_from_text(const char *raw_text, VolumeMapItem *volumes, int max_volumes) {
    if (!raw_text || raw_text[0] == '\0' || !volumes || max_volumes <= 0) return 0;

    int count = 0;

    // 1. Try AI Agent via OpenRouter API
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

    if (api_key[0] != '\0' && model_id[0] != '\0') {
        append_pdf_log(L"[AI Agent] Mengirim teks daftar volume ke model AI aktif...");
        CURL *curl = curl_easy_init();
        if (curl) {
            char ep_url[1024];
            snprintf(ep_url, sizeof(ep_url), "%s/chat/completions", api_url);

            cJSON *req = cJSON_CreateObject();
            cJSON_AddStringToObject(req, "model", model_id);

            cJSON *messages = cJSON_CreateArray();
            cJSON *sys = cJSON_CreateObject();
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddStringToObject(sys, "content",
                "You are a manga assistant. Parse the volume list provided by user. Return ONLY a valid JSON array of objects with integer fields: \"volume\", \"start_chapter\", \"end_chapter\". Example: [{\"volume\": 1, \"start_chapter\": 1, \"end_chapter\": 10}]. Output raw JSON only, no markdown, no explanation.");
            cJSON_AddItemToArray(messages, sys);

            cJSON *usr = cJSON_CreateObject();
            cJSON_AddStringToObject(usr, "role", "user");
            cJSON_AddStringToObject(usr, "content", raw_text);
            cJSON_AddItemToArray(messages, usr);

            cJSON_AddItemToObject(req, "messages", messages);
            cJSON_AddNumberToObject(req, "temperature", 0.0);

            char *payload = cJSON_PrintUnformatted(req);
            cJSON_Delete(req);

            struct curl_slist *headers = NULL;
            char auth_hdr[600];
            snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", api_key);
            headers = curl_slist_append(headers, auth_hdr);
            headers = curl_slist_append(headers, "Content-Type: application/json");

            MemoryChunk chunk = { malloc(1), 0 };
            chunk.data[0] = '\0';

            curl_easy_setopt(curl, CURLOPT_URL, ep_url);
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

            CURLcode res = curl_easy_perform(curl);
            curl_slist_free_all(headers);
            free(payload);
            curl_easy_cleanup(curl);

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
                                    cJSON *varr = cJSON_Parse(jbuf);
                                    if (varr && cJSON_IsArray(varr)) {
                                        int asz = cJSON_GetArraySize(varr);
                                        for (int i = 0; i < asz && count < max_volumes; i++) {
                                            cJSON *vitem = cJSON_GetArrayItem(varr, i);
                                            cJSON *c_vol = cJSON_GetObjectItem(vitem, "volume");
                                            cJSON *c_start = cJSON_GetObjectItem(vitem, "start_chapter");
                                            cJSON *c_end = cJSON_GetObjectItem(vitem, "end_chapter");
                                            if (c_vol && c_start && c_end) {
                                                volumes[count].volume_num = c_vol->valueint;
                                                volumes[count].start_chapter = c_start->valueint;
                                                volumes[count].end_chapter = c_end->valueint;
                                                snprintf(volumes[count].title, sizeof(volumes[count].title), "Volume %02d", volumes[count].volume_num);
                                                count++;
                                            }
                                        }
                                        cJSON_Delete(varr);
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
        }
    }

    if (count > 0) {
        wchar_t msg[128];
        _snwprintf(msg, sizeof(msg)/sizeof(wchar_t), L"[AI Agent] Berhasil memetakan %d volume dari teks.", count);
        append_pdf_log(msg);
        return count;
    }

    // 2. Deterministic Local Fallback Parser
    append_pdf_log(L"[Parser] Menggunakan parser lokal deterministik untuk membaca volume...");
    const char *line = raw_text;
    while (*line && count < max_volumes) {
        while (*line == '\r' || *line == '\n') line++;
        if (*line == '\0') break;

        const char *next = strchr(line, '\n');
        size_t llen = next ? (size_t)(next - line) : strlen(line);
        char lbuf[512];
        if (llen >= sizeof(lbuf)) llen = sizeof(lbuf) - 1;
        memcpy(lbuf, line, llen);
        lbuf[llen] = '\0';

        int v_num = -1, start_ch = -1, end_ch = -1;
        char *p = lbuf;
        while (*p) {
            if (isdigit((unsigned char)*p)) {
                if (v_num < 0) v_num = atoi(p);
                else if (start_ch < 0) start_ch = atoi(p);
                else if (end_ch < 0) { end_ch = atoi(p); break; }
                while (*p && isdigit((unsigned char)*p)) p++;
            } else {
                p++;
            }
        }

        if (v_num > 0) {
            if (start_ch < 0) start_ch = 1;
            if (end_ch < 0) end_ch = start_ch;
            if (end_ch < start_ch) { int t = start_ch; start_ch = end_ch; end_ch = t; }

            volumes[count].volume_num = v_num;
            volumes[count].start_chapter = start_ch;
            volumes[count].end_chapter = end_ch;
            snprintf(volumes[count].title, sizeof(volumes[count].title), "Volume %02d", v_num);
            count++;
        }

        line = next ? next + 1 : line + llen;
    }

    return count;
}

// Background thread procedure for PDF conversion
static DWORD WINAPI PdfConversionThreadProc(LPVOID lpParam) {
    CoInitialize(NULL);

    wchar_t manga_root[MAX_PATH];
    GetWindowTextW(hPdfFolderEdit, manga_root, MAX_PATH);

    append_pdf_log(L"=========================================");
    append_pdf_log(L"[1/4] Memulai verifikasi folder manga...");

    // 1. Scan Chapter Folders
    ChapterFolderInfo *all_chapters = (ChapterFolderInfo *)calloc(MAX_CHAPTERS, sizeof(ChapterFolderInfo));
    int total_ch = 0;

    wchar_t search_pattern[MAX_PATH];
    _snwprintf(search_pattern, MAX_PATH, L"%s\\*.*", manga_root);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search_pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0 &&
                _wcsnicmp(fd.cFileName, L"Volume ", 7) != 0 && _wcsnicmp(fd.cFileName, L"Vol ", 4) != 0) {

                char c_name[MAX_PATH];
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, c_name, MAX_PATH, NULL, NULL);

                int ch_num = extract_chapter_number_from_string(c_name);
                if (ch_num >= 0 && total_ch < MAX_CHAPTERS) {
                    wcsncpy(all_chapters[total_ch].folder_name, fd.cFileName, MAX_PATH - 1);
                    _snwprintf(all_chapters[total_ch].full_path, MAX_PATH, L"%s\\%s", manga_root, fd.cFileName);
                    all_chapters[total_ch].chapter_num = ch_num;

                    // Scan images inside chapter
                    wchar_t img_pat[MAX_PATH];
                    _snwprintf(img_pat, MAX_PATH, L"%s\\*.*", all_chapters[total_ch].full_path);
                    WIN32_FIND_DATAW ifd;
                    HANDLE hImg = FindFirstFileW(img_pat, &ifd);
                    int icnt = 0;
                    if (hImg != INVALID_HANDLE_VALUE) {
                        do {
                            if (!(ifd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                                const wchar_t *ext = wcsrchr(ifd.cFileName, L'.');
                                if (ext && (_wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0 ||
                                            _wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".webp") == 0 ||
                                            _wcsicmp(ext, L".bmp") == 0)) {
                                    icnt++;
                                }
                            }
                        } while (FindNextFileW(hImg, &ifd));
                        FindClose(hImg);
                    }
                    all_chapters[total_ch].image_count = icnt;
                    total_ch++;
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    if (total_ch == 0) {
        append_pdf_log(L"[Error] Tidak ditemukan folder chapter yang valid.");
        free(all_chapters);
        CoUninitialize();
        is_converting = false;
        EnableWindow(hPdfStartBtn, TRUE);
        EnableWindow(hPdfStopBtn, FALSE);
        MessageBeep(MB_ICONASTERISK);
        return 0;
    }

    // Sort chapters in ascending order (Chapter 0, Chapter 1, ...)
    qsort(all_chapters, total_ch, sizeof(ChapterFolderInfo), compare_chapters_asc);

    wchar_t log_buf[512];
    _snwprintf(log_buf, sizeof(log_buf)/sizeof(wchar_t), L"-> Ditemukan total %d folder chapter.", total_ch);
    append_pdf_log(log_buf);

    bool is_mode_volume = (SendMessageW(hPdfRadioVolume, BM_GETCHECK, 0, 0) == BST_CHECKED);

    wchar_t success_summary[4096] = L"";
    wchar_t failed_summary[4096] = L"";
    int success_cnt = 0, failed_cnt = 0;

    if (!is_mode_volume) {
        // =========================================================================
        // MODE: PER CHAPTER (1 PDF per chapter)
        // =========================================================================
        append_pdf_log(L"[2/4] Mode Per Chapter dipilih. Mengonversi tiap chapter ke PDF...");

        for (int i = 0; i < total_ch; i++) {
            if (stop_pdf_requested) break;

            _snwprintf(log_buf, sizeof(log_buf)/sizeof(wchar_t), L"--- [%d/%d] Memproses %s (No: %d) ---",
                       i + 1, total_ch, all_chapters[i].folder_name, all_chapters[i].chapter_num);
            append_pdf_log(log_buf);

            // Step 4: Check if chapter has valid images
            if (all_chapters[i].image_count == 0) {
                _snwprintf(log_buf, sizeof(log_buf)/sizeof(wchar_t), L"-> [Skip] Chapter '%s' tidak memiliki gambar. Dilewati.", all_chapters[i].folder_name);
                append_pdf_log(log_buf);

                if (failed_cnt < 20) {
                    wchar_t item[256];
                    _snwprintf(item, sizeof(item)/sizeof(wchar_t), L"- %s (Tidak ada gambar)\n", all_chapters[i].folder_name);
                    wcscat(failed_summary, item);
                }
                failed_cnt++;
                continue;
            }

            // Collect all image paths in chapter
            wchar_t img_pat[MAX_PATH];
            _snwprintf(img_pat, MAX_PATH, L"%s\\*.*", all_chapters[i].full_path);
            WIN32_FIND_DATAW ifd;
            HANDLE hImg = FindFirstFileW(img_pat, &ifd);

            wchar_t **img_list = (wchar_t **)calloc(all_chapters[i].image_count, sizeof(wchar_t *));
            int icnt = 0;
            if (hImg != INVALID_HANDLE_VALUE) {
                do {
                    if (!(ifd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        const wchar_t *ext = wcsrchr(ifd.cFileName, L'.');
                        if (ext && (_wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0 ||
                                    _wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".webp") == 0 ||
                                    _wcsicmp(ext, L".bmp") == 0)) {
                            if (icnt < all_chapters[i].image_count) {
                                img_list[icnt] = (wchar_t *)malloc(MAX_PATH * sizeof(wchar_t));
                                _snwprintf(img_list[icnt], MAX_PATH, L"%s\\%s", all_chapters[i].full_path, ifd.cFileName);
                                icnt++;
                            }
                        }
                    }
                } while (FindNextFileW(hImg, &ifd));
                FindClose(hImg);
            }

            // Sort image filenames ascending (001, 002, ...)
            qsort(img_list, icnt, sizeof(wchar_t *), compare_image_paths);

            // PDF output path
            wchar_t pdf_out[MAX_PATH];
            _snwprintf(pdf_out, MAX_PATH, L"%s\\%s.pdf", manga_root, all_chapters[i].folder_name);

            bool ok = create_pdf_from_images(pdf_out, (const wchar_t **)img_list, icnt, &stop_pdf_requested);

            for (int k = 0; k < icnt; k++) free(img_list[k]);
            free(img_list);

            if (ok) {
                _snwprintf(log_buf, sizeof(log_buf)/sizeof(wchar_t), L"-> [Sukses] Dibuat: %s.pdf (%d halaman)", all_chapters[i].folder_name, icnt);
                append_pdf_log(log_buf);

                if (success_cnt < 20) {
                    wchar_t item[256];
                    _snwprintf(item, sizeof(item)/sizeof(wchar_t), L"- %s\n", all_chapters[i].folder_name);
                    wcscat(success_summary, item);
                }
                success_cnt++;
            } else {
                _snwprintf(log_buf, sizeof(log_buf)/sizeof(wchar_t), L"-> [Gagal] Pembuatan PDF untuk %s gagal.", all_chapters[i].folder_name);
                append_pdf_log(log_buf);

                if (failed_cnt < 20) {
                    wchar_t item[256];
                    _snwprintf(item, sizeof(item)/sizeof(wchar_t), L"- %s\n", all_chapters[i].folder_name);
                    wcscat(failed_summary, item);
                }
                failed_cnt++;
            }
        }
    } else {
        // =========================================================================
        // MODE: PER VOLUME (Merge chapters into volumes & reorganize folders)
        // =========================================================================
        append_pdf_log(L"[2/4] Mode Per Volume dipilih. Menentukan partisi volume...");

        VolumeMapItem *vol_maps = (VolumeMapItem *)calloc(MAX_VOL_ITEMS, sizeof(VolumeMapItem));
        int vol_count = 0;

        bool use_custom_text = (SendMessageW(hPdfRadioVolCustom, BM_GETCHECK, 0, 0) == BST_CHECKED);
        if (use_custom_text) {
            int tlen = GetWindowTextLengthW(hPdfVolTextarea);
            if (tlen > 0) {
                wchar_t *wtxt = (wchar_t *)malloc((tlen + 1) * sizeof(wchar_t));
                GetWindowTextW(hPdfVolTextarea, wtxt, tlen + 1);
                char *utxt = (char *)malloc((tlen * 3) + 1);
                WideCharToMultiByte(CP_UTF8, 0, wtxt, -1, utxt, (tlen * 3) + 1, NULL, NULL);

                vol_count = parse_volumes_from_text(utxt, vol_maps, MAX_VOL_ITEMS);
                free(wtxt);
                free(utxt);
            }
        } else {
            // Auto split: 1 volume = X chapters
            wchar_t wcnt[32];
            GetWindowTextW(hPdfVolCountEdit, wcnt, 32);
            int ch_per_vol = _wtoi(wcnt);
            if (ch_per_vol <= 0) ch_per_vol = 10;

            for (int i = 0; i < total_ch && vol_count < MAX_VOL_ITEMS; i += ch_per_vol) {
                vol_count++;
                vol_maps[vol_count - 1].volume_num = vol_count;
                vol_maps[vol_count - 1].start_chapter = all_chapters[i].chapter_num;
                int end_idx = i + ch_per_vol - 1;
                if (end_idx >= total_ch) end_idx = total_ch - 1;
                vol_maps[vol_count - 1].end_chapter = all_chapters[end_idx].chapter_num;
                snprintf(vol_maps[vol_count - 1].title, sizeof(vol_maps[vol_count - 1].title), "Volume %02d", vol_count);
            }
            _snwprintf(log_buf, sizeof(log_buf)/sizeof(wchar_t), L"-> Dihitung otomatis: %d volume (%d chapter/volume).", vol_count, ch_per_vol);
            append_pdf_log(log_buf);
        }

        if (vol_count == 0) {
            append_pdf_log(L"[Error] Tidak ada daftar volume yang dapat ditentukan.");
            free(vol_maps);
            free(all_chapters);
            CoUninitialize();
            is_converting = false;
            EnableWindow(hPdfStartBtn, TRUE);
            EnableWindow(hPdfStopBtn, FALSE);
            MessageBeep(MB_ICONASTERISK);
            return 0;
        }

        append_pdf_log(L"[3/4] Mengelompokkan & memindahkan folder chapter ke direktori Volume...");

        // Process each volume
        for (int v = 0; v < vol_count; v++) {
            if (stop_pdf_requested) break;

            wchar_t vol_title_w[128];
            MultiByteToWideChar(CP_UTF8, 0, vol_maps[v].title, -1, vol_title_w, 128);

            _snwprintf(log_buf, sizeof(log_buf)/sizeof(wchar_t), L"\n=== [%d/%d] Memproses %s (Chapter %d - %d) ===",
                       v + 1, vol_count, vol_title_w, vol_maps[v].start_chapter, vol_maps[v].end_chapter);
            append_pdf_log(log_buf);

            // Step 2: Create volume directory
            wchar_t vol_dir[MAX_PATH];
            _snwprintf(vol_dir, MAX_PATH, L"%s\\%s", manga_root, vol_title_w);
            CreateDirectoryW(vol_dir, NULL);

            // Relocate / Move matching chapter folders into Volume directory
            wchar_t **vol_images = (wchar_t **)calloc(MAX_PATH_PAGES, sizeof(wchar_t *));
            int total_vol_img = 0;
            int matched_chapters = 0;

            for (int i = 0; i < total_ch; i++) {
                if (all_chapters[i].chapter_num >= vol_maps[v].start_chapter &&
                    all_chapters[i].chapter_num <= vol_maps[v].end_chapter) {

                    matched_chapters++;
                    wchar_t dest_chapter_dir[MAX_PATH];
                    _snwprintf(dest_chapter_dir, MAX_PATH, L"%s\\%s", vol_dir, all_chapters[i].folder_name);

                    // Move folder if not already moved
                    if (_wcsicmp(all_chapters[i].full_path, dest_chapter_dir) != 0) {
                        if (MoveFileW(all_chapters[i].full_path, dest_chapter_dir)) {
                            wcsncpy(all_chapters[i].full_path, dest_chapter_dir, MAX_PATH - 1);
                        }
                    }

                    // Scan images inside moved chapter folder
                    wchar_t img_pat[MAX_PATH];
                    _snwprintf(img_pat, MAX_PATH, L"%s\\*.*", all_chapters[i].full_path);
                    WIN32_FIND_DATAW ifd;
                    HANDLE hImg = FindFirstFileW(img_pat, &ifd);
                    if (hImg != INVALID_HANDLE_VALUE) {
                        wchar_t *ch_imgs[500];
                        int ch_icnt = 0;
                        do {
                            if (!(ifd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                                const wchar_t *ext = wcsrchr(ifd.cFileName, L'.');
                                if (ext && (_wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0 ||
                                            _wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".webp") == 0 ||
                                            _wcsicmp(ext, L".bmp") == 0)) {
                                    if (ch_icnt < 500) {
                                        ch_imgs[ch_icnt] = (wchar_t *)malloc(MAX_PATH * sizeof(wchar_t));
                                        _snwprintf(ch_imgs[ch_icnt], MAX_PATH, L"%s\\%s", all_chapters[i].full_path, ifd.cFileName);
                                        ch_icnt++;
                                    }
                                }
                            }
                        } while (FindNextFileW(hImg, &ifd));
                        FindClose(hImg);

                        // Sort image pages inside chapter
                        qsort(ch_imgs, ch_icnt, sizeof(wchar_t *), compare_image_paths);

                        for (int k = 0; k < ch_icnt; k++) {
                            if (total_vol_img < MAX_PATH_PAGES) {
                                vol_images[total_vol_img++] = ch_imgs[k];
                            } else {
                                free(ch_imgs[k]);
                            }
                        }
                    }
                }
            }

            // Step 4: Check if volume has valid images
            if (total_vol_img == 0) {
                _snwprintf(log_buf, sizeof(log_buf)/sizeof(wchar_t), L"-> [Skip] %s tidak memiliki gambar chapter yang tersedia. Dilewati.", vol_title_w);
                append_pdf_log(log_buf);

                if (failed_cnt < 20) {
                    wchar_t item[256];
                    _snwprintf(item, sizeof(item)/sizeof(wchar_t), L"- %s (Chapter kosong/tidak ada)\n", vol_title_w);
                    wcscat(failed_summary, item);
                }
                failed_cnt++;
                free(vol_images);
                continue;
            }

            _snwprintf(log_buf, sizeof(log_buf)/sizeof(wchar_t), L"-> Mengonversi %s (%d chapter, %d halaman total) ke PDF...",
                       vol_title_w, matched_chapters, total_vol_img);
            append_pdf_log(log_buf);

            // Create Volume PDF: <MangaRoot>\<VolumeTitle>.pdf
            wchar_t vol_pdf[MAX_PATH];
            _snwprintf(vol_pdf, MAX_PATH, L"%s\\%s.pdf", manga_root, vol_title_w);

            bool ok = create_pdf_from_images(vol_pdf, (const wchar_t **)vol_images, total_vol_img, &stop_pdf_requested);

            for (int k = 0; k < total_vol_img; k++) free(vol_images[k]);
            free(vol_images);

            if (ok) {
                _snwprintf(log_buf, sizeof(log_buf)/sizeof(wchar_t), L"-> [Sukses] Dibuat: %s.pdf", vol_title_w);
                append_pdf_log(log_buf);

                if (success_cnt < 20) {
                    wchar_t item[256];
                    _snwprintf(item, sizeof(item)/sizeof(wchar_t), L"- %s (%d chapter, %d halaman)\n", vol_title_w, matched_chapters, total_vol_img);
                    wcscat(success_summary, item);
                }
                success_cnt++;
            } else {
                _snwprintf(log_buf, sizeof(log_buf)/sizeof(wchar_t), L"-> [Gagal] Pembuatan PDF untuk %s gagal.", vol_title_w);
                append_pdf_log(log_buf);

                if (failed_cnt < 20) {
                    wchar_t item[256];
                    _snwprintf(item, sizeof(item)/sizeof(wchar_t), L"- %s\n", vol_title_w);
                    wcscat(failed_summary, item);
                }
                failed_cnt++;
            }
        }

        free(vol_maps);
    }

    free(all_chapters);
    CoUninitialize();

    append_pdf_log(L"\n=========================================");
    if (stop_pdf_requested) {
        append_pdf_log(L"STATUS: Konversi PDF DIBATALKAN oleh pengguna.");
    } else {
        append_pdf_log(L"STATUS: SEMUA PROSES KONVERSI SELESAI!");
    }
    append_pdf_log(L"=========================================");

    // Sound notification on completion
    MessageBeep(MB_ICONASTERISK);

    // Step 5: MessageBox with summary of successful and failed volumes/chapters
    wchar_t mb_msg[8192];
    _snwprintf(mb_msg, sizeof(mb_msg)/sizeof(wchar_t),
               L"%s\n\n[Berhasil (%d)]:\n%s\n[Gagal / Dilewati (%d)]:\n%s",
               _TW("str_pdf_alert_success_summary"),
               success_cnt,
               (success_cnt > 0) ? success_summary : L"- Tidak ada -\n",
               failed_cnt,
               (failed_cnt > 0) ? failed_summary : L"- Tidak ada -\n");

    MessageBoxW(hParentWnd, mb_msg, _TW("str_pdf_alert_result_title"), MB_OK | (failed_cnt > 0 ? MB_ICONWARNING : MB_ICONINFORMATION));

    is_converting = false;
    EnableWindow(hPdfStartBtn, TRUE);
    EnableWindow(hPdfStopBtn, FALSE);
    return 0;
}

void pdf_converter_init(HWND hwndParent, HINSTANCE hInst) {
    hParentWnd = hwndParent;
    hPdfInst = hInst;
    g_pdf_ctrl_count = 0;

    // Folder Label
    hPdfFolderLbl = CreateWindowW(L"STATIC", _TW("str_pdf_folder_label"), WS_CHILD,
                                  25, 38, 475, 18, hwndParent, NULL, hInst, NULL);
    apply_gui_font(hPdfFolderLbl);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfFolderLbl;

    // Folder Edit
    hPdfFolderEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | ES_AUTOHSCROLL,
                                     25, 58, 475, 24, hwndParent, (HMENU)ID_PDF_FOLDER_EDIT, hInst, NULL);
    apply_gui_font(hPdfFolderEdit);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfFolderEdit;

    // Browse Button
    hPdfBrowseBtn = CreateWindowW(L"BUTTON", _TW("str_btn_browse"), WS_CHILD | BS_PUSHBUTTON,
                                  508, 57, 90, 26, hwndParent, (HMENU)ID_PDF_BROWSE_BTN, hInst, NULL);
    apply_gui_font(hPdfBrowseBtn);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfBrowseBtn;

    // Open Folder Button
    hPdfOpenBtn = CreateWindowW(L"BUTTON", _TW("str_pdf_btn_open"), WS_CHILD | BS_PUSHBUTTON,
                                605, 57, 95, 26, hwndParent, (HMENU)ID_PDF_OPEN_BTN, hInst, NULL);
    apply_gui_font(hPdfOpenBtn);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfOpenBtn;

    // Mode Selection Label
    hPdfModeLbl = CreateWindowW(L"STATIC", _TW("str_pdf_mode_label"), WS_CHILD,
                                25, 90, 200, 18, hwndParent, NULL, hInst, NULL);
    apply_gui_font(hPdfModeLbl);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfModeLbl;

    // Radio: Per Chapter
    hPdfRadioChapter = CreateWindowW(L"BUTTON", _TW("str_pdf_mode_chapter"),
                                     WS_CHILD | BS_AUTORADIOBUTTON | WS_GROUP,
                                     25, 108, 300, 20, hwndParent, (HMENU)ID_PDF_RADIO_CHAPTER, hInst, NULL);
    apply_gui_font(hPdfRadioChapter);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfRadioChapter;

    // Radio: Per Volume
    hPdfRadioVolume = CreateWindowW(L"BUTTON", _TW("str_pdf_mode_volume"),
                                    WS_CHILD | BS_AUTORADIOBUTTON,
                                    330, 108, 370, 20, hwndParent, (HMENU)ID_PDF_RADIO_VOLUME, hInst, NULL);
    apply_gui_font(hPdfRadioVolume);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfRadioVolume;

    // Default: Per Volume selected
    SendMessageW(hPdfRadioVolume, BM_SETCHECK, BST_CHECKED, 0);

    // Radio: Volume Custom (Textarea)
    hPdfRadioVolCustom = CreateWindowW(L"BUTTON", _TW("str_pdf_vol_custom"),
                                       WS_CHILD | BS_AUTORADIOBUTTON | WS_GROUP,
                                       25, 134, 450, 20, hwndParent, (HMENU)ID_PDF_RADIO_VOL_CUSTOM, hInst, NULL);
    apply_gui_font(hPdfRadioVolCustom);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfRadioVolCustom;
    SendMessageW(hPdfRadioVolCustom, BM_SETCHECK, BST_CHECKED, 0);

    // Textarea for Volume list
    hPdfVolTextarea = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
                                      25, 156, 675, 90, hwndParent, (HMENU)ID_PDF_VOL_TEXTAREA, hInst, NULL);
    apply_gui_font(hPdfVolTextarea);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfVolTextarea;
    SendMessageW(hPdfVolTextarea, EM_SETCUEBANNER, TRUE, (LPARAM)_TW("str_pdf_vol_placeholder"));

    // Radio: Volume Auto (Count)
    hPdfRadioVolAuto = CreateWindowW(L"BUTTON", _TW("str_pdf_vol_auto"),
                                     WS_CHILD | BS_AUTORADIOBUTTON,
                                     25, 252, 140, 22, hwndParent, (HMENU)ID_PDF_RADIO_VOL_AUTO, hInst, NULL);
    apply_gui_font(hPdfRadioVolAuto);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfRadioVolAuto;

    // Count Edit
    hPdfVolCountEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"10",
                                       WS_CHILD | ES_NUMBER | ES_CENTER,
                                       170, 252, 45, 22, hwndParent, (HMENU)ID_PDF_VOL_COUNT_EDIT, hInst, NULL);
    apply_gui_font(hPdfVolCountEdit);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfVolCountEdit;

    // Label: chapters per volume
    hPdfVolPerLbl = CreateWindowW(L"STATIC", _TW("str_pdf_vol_per"), WS_CHILD,
                                  222, 254, 250, 18, hwndParent, NULL, hInst, NULL);
    apply_gui_font(hPdfVolPerLbl);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfVolPerLbl;

    // Button: Start
    hPdfStartBtn = CreateWindowW(L"BUTTON", _TW("str_pdf_btn_start"),
                                 WS_CHILD | BS_DEFPUSHBUTTON,
                                 25, 282, 160, 32, hwndParent, (HMENU)ID_PDF_START_BTN, hInst, NULL);
    apply_gui_font(hPdfStartBtn);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfStartBtn;

    // Button: Stop
    hPdfStopBtn = CreateWindowW(L"BUTTON", _TW("str_pdf_btn_stop"),
                                WS_CHILD | BS_PUSHBUTTON | WS_DISABLED,
                                195, 282, 90, 32, hwndParent, (HMENU)ID_PDF_STOP_BTN, hInst, NULL);
    apply_gui_font(hPdfStopBtn);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfStopBtn;

    // Status Label
    hPdfStatusLbl = CreateWindowW(L"STATIC", _TW("str_pdf_status_idle"), WS_CHILD | SS_LEFTNOWORDWRAP,
                                  295, 290, 400, 20, hwndParent, (HMENU)ID_PDF_STATUS_LBL, hInst, NULL);
    apply_gui_font(hPdfStatusLbl);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfStatusLbl;

    // Log Label
    hPdfLogLbl = CreateWindowW(L"STATIC", _TW("str_pdf_log_label"), WS_CHILD,
                               25, 322, 300, 18, hwndParent, NULL, hInst, NULL);
    apply_gui_font(hPdfLogLbl);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfLogLbl;

    // Log Edit box
    hPdfLogEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                  25, 342, 675, 260, hwndParent, (HMENU)ID_PDF_LOG_EDIT, hInst, NULL);
    apply_gui_font(hPdfLogEdit);
    g_pdf_controls[g_pdf_ctrl_count++] = hPdfLogEdit;
}

void pdf_converter_switch_tab(bool show) {
    int cmd = show ? SW_SHOW : SW_HIDE;
    for (int i = 0; i < g_pdf_ctrl_count; i++) {
        if (g_pdf_controls[i]) ShowWindow(g_pdf_controls[i], cmd);
    }
}

void pdf_converter_on_command(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    WORD id = LOWORD(wParam);

    if (id == ID_PDF_BROWSE_BTN) {
        wchar_t currentPath[MAX_PATH];
        GetWindowTextW(hPdfFolderEdit, currentPath, MAX_PATH);

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
                SetWindowTextW(hPdfFolderEdit, chosenPath);
            }
            CoTaskMemFree(pidl);
        }
    } else if (id == ID_PDF_OPEN_BTN) {
        wchar_t path[MAX_PATH];
        GetWindowTextW(hPdfFolderEdit, path, MAX_PATH);
        if (wcslen(path) > 0) {
            ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
        }
    } else if (id == ID_PDF_RADIO_CHAPTER || id == ID_PDF_RADIO_VOLUME) {
        bool is_vol = (SendMessageW(hPdfRadioVolume, BM_GETCHECK, 0, 0) == BST_CHECKED);
        EnableWindow(hPdfRadioVolCustom, is_vol);
        EnableWindow(hPdfRadioVolAuto, is_vol);
        EnableWindow(hPdfVolTextarea, is_vol);
        EnableWindow(hPdfVolCountEdit, is_vol);
    } else if (id == ID_PDF_START_BTN) {
        if (is_converting) return;

        wchar_t path[MAX_PATH];
        GetWindowTextW(hPdfFolderEdit, path, MAX_PATH);
        if (wcslen(path) == 0) {
            MessageBoxW(hwnd, _TW("str_pdf_alert_folder_empty"), _TW("str_alert_warning"), MB_OK | MB_ICONWARNING);
            return;
        }

        stop_pdf_requested = false;
        is_converting = true;
        EnableWindow(hPdfStartBtn, FALSE);
        EnableWindow(hPdfStopBtn, TRUE);

        if (hPdfThread) {
            CloseHandle(hPdfThread);
            hPdfThread = NULL;
        }
        hPdfThread = CreateThread(NULL, 0, PdfConversionThreadProc, NULL, 0, NULL);
    } else if (id == ID_PDF_STOP_BTN) {
        if (is_converting) {
            stop_pdf_requested = true;
            append_pdf_log(L"[Info] Mengirim permintaan pembatalan konversi...");
            EnableWindow(hPdfStopBtn, FALSE);
        }
    }
}

void pdf_converter_refresh_lang(void) {
    if (hPdfFolderLbl) SetWindowTextW(hPdfFolderLbl, _TW("str_pdf_folder_label"));
    if (hPdfBrowseBtn) SetWindowTextW(hPdfBrowseBtn, _TW("str_btn_browse"));
    if (hPdfOpenBtn) SetWindowTextW(hPdfOpenBtn, _TW("str_pdf_btn_open"));
    if (hPdfModeLbl) SetWindowTextW(hPdfModeLbl, _TW("str_pdf_mode_label"));
    if (hPdfRadioChapter) SetWindowTextW(hPdfRadioChapter, _TW("str_pdf_mode_chapter"));
    if (hPdfRadioVolume) SetWindowTextW(hPdfRadioVolume, _TW("str_pdf_mode_volume"));
    if (hPdfRadioVolCustom) SetWindowTextW(hPdfRadioVolCustom, _TW("str_pdf_vol_custom"));
    if (hPdfRadioVolAuto) SetWindowTextW(hPdfRadioVolAuto, _TW("str_pdf_vol_auto"));
    if (hPdfVolPerLbl) SetWindowTextW(hPdfVolPerLbl, _TW("str_pdf_vol_per"));
    if (hPdfStartBtn) SetWindowTextW(hPdfStartBtn, _TW("str_pdf_btn_start"));
    if (hPdfStopBtn) SetWindowTextW(hPdfStopBtn, _TW("str_pdf_btn_stop"));
    if (hPdfStatusLbl) SetWindowTextW(hPdfStatusLbl, _TW("str_pdf_status_idle"));
    if (hPdfLogLbl) SetWindowTextW(hPdfLogLbl, _TW("str_pdf_log_label"));
    if (hPdfVolTextarea) SendMessageW(hPdfVolTextarea, EM_SETCUEBANNER, TRUE, (LPARAM)_TW("str_pdf_vol_placeholder"));
}
