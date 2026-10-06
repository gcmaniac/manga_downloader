#ifndef MANGA_TRANSLATOR_H
#define MANGA_TRANSLATOR_H

#include <windows.h>
#include <stdbool.h>

// Control IDs for Manga Translator Tab
#define ID_TRANS_FOLDER_EDIT        501
#define ID_TRANS_BROWSE_BTN         502
#define ID_TRANS_OPEN_BTN           503
#define ID_TRANS_TARGET_LANG_COMBO  504
#define ID_TRANS_CHAPTER_FILTER     505
#define ID_TRANS_SAVE_IMG_CHK       506
#define ID_TRANS_AUTO_PDF_CHK       507
#define ID_TRANS_START_BTN          508
#define ID_TRANS_STOP_BTN           509
#define ID_TRANS_STATUS_LBL         510
#define ID_TRANS_LOG_EDIT           511

void manga_translator_init(HWND hwndParent, HINSTANCE hInst);
void manga_translator_switch_tab(bool show);
void manga_translator_on_command(HWND hwnd, WPARAM wParam, LPARAM lParam);
void manga_translator_refresh_lang(void);

#endif // MANGA_TRANSLATOR_H
