#ifndef AI_AGENT_H
#define AI_AGENT_H

#include <windows.h>
#include <commctrl.h>
#include <stdbool.h>

#define WM_APP_SCAN_DONE            (WM_USER + 201)

// Control IDs for Settings Tab (Tab 1: Pengaturan & Uji AI)
#define ID_SERVER_NAME_COMBO        301
#define ID_SERVER_URL_EDIT          302
#define ID_API_KEY_EDIT             303
#define ID_SCAN_BTN                 304
#define ID_SCAN_STATUS              305
#define ID_SORT1_COMBO              306
#define ID_ORDER1_COMBO             307
#define ID_SORT2_COMBO              308
#define ID_ORDER2_COMBO             309
#define ID_MODEL_LISTVIEW           310

// Control IDs for Catalog Tab (Tab 2: Katalog Model Teruji)
#define ID_TAB2_FILTER_COMBO        320
#define ID_TAB2_SORT1_COMBO         321
#define ID_TAB2_ORDER1_COMBO        322
#define ID_TAB2_SORT2_COMBO         323
#define ID_TAB2_ORDER2_COMBO        324
#define ID_TAB2_USE_MODEL_BTN       325
#define ID_TAB2_USE_STATUS          326
#define ID_TAB2_LISTVIEW            327
#define ID_TAB2_FILTER_FROM_EDIT    328
#define ID_TAB2_FILTER_TO_EDIT      329
#define ID_TAB2_APPLY_FILTER_BTN    334

// Control IDs for Used Models Tab (Tab 3: Model AI Digunakan)
#define ID_TAB3_DELETE_BTN          330
#define ID_TAB3_CLEAR_BTN           331
#define ID_TAB3_STATUS              332
#define ID_TAB3_LISTVIEW            333

void ai_agent_init(HWND hwndParent, HINSTANCE hInst);
void ai_agent_switch_tab(int tab_index);
void ai_agent_on_command(HWND hwnd, WPARAM wParam, LPARAM lParam);
void ai_agent_on_scan_done(HWND hwnd, int count);
void ai_agent_delete_selected_used_models(HWND hwnd);
void ai_agent_refresh_settings_list(void);
void ai_agent_refresh_catalog_list(void);
void ai_agent_refresh_used_models_list(void);
void ai_agent_refresh_lang(void);

#endif // AI_AGENT_H
