#ifndef PDF_CONVERTER_H
#define PDF_CONVERTER_H

#include <windows.h>
#include <stdbool.h>

// Control IDs for PDF Converter Tab
#define ID_PDF_FOLDER_EDIT          401
#define ID_PDF_BROWSE_BTN           402
#define ID_PDF_OPEN_BTN             403
#define ID_PDF_RADIO_CHAPTER        404
#define ID_PDF_RADIO_VOLUME         405
#define ID_PDF_RADIO_VOL_CUSTOM     406
#define ID_PDF_RADIO_VOL_AUTO       407
#define ID_PDF_VOL_COUNT_EDIT       408
#define ID_PDF_VOL_TEXTAREA         409
#define ID_PDF_START_BTN            410
#define ID_PDF_STOP_BTN             411
#define ID_PDF_STATUS_LBL           412
#define ID_PDF_LOG_EDIT             413

void pdf_converter_init(HWND hwndParent, HINSTANCE hInst);
void pdf_converter_switch_tab(bool show);
void pdf_converter_on_command(HWND hwnd, WPARAM wParam, LPARAM lParam);
void pdf_converter_refresh_lang(void);

// Shared multi-page PDF generator
bool create_pdf_from_images(const wchar_t *pdf_output_path, const wchar_t **image_paths, int image_count, volatile bool *p_stop);

#endif // PDF_CONVERTER_H
