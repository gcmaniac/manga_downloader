#include "db_migration.h"
#include "lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <shlobj.h>

void db_migration_get_db_path(wchar_t *path_out, size_t max_len) {
    GetModuleFileNameW(NULL, path_out, (DWORD)max_len);
    wchar_t *p = wcsrchr(path_out, L'\\');
    if (p) *(p + 1) = L'\0';
    wcsncat(path_out, L"manga_downloader.db", max_len - wcslen(path_out) - 1);
}

static void log_msg(db_migration_log_fn cb, const char *msg) {
    if (cb) {
        cb(msg);
    }
}

static int get_current_schema_version(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    int version = 0;
    const char *sql = "SELECT COALESCE(MAX(version), 0) FROM _schema_migrations;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            version = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return version;
}

static bool record_migration(sqlite3 *db, int version, const char *name) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO _schema_migrations (version, name) VALUES (?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, version);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

// Migrasi 1: Tabel model, server_agent, dan model_penggunaan
static bool apply_migration_v1(sqlite3 *db, db_migration_log_fn cb) {
    log_msg(cb, _T("str_db_mig_v1"));

    const char *v1_sql =
        "CREATE TABLE IF NOT EXISTS model ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    provider TEXT NOT NULL,"
        "    model_id TEXT NOT NULL,"
        "    name TEXT NOT NULL,"
        "    rating REAL NOT NULL DEFAULT 0.0,"
        "    response_time_ms INTEGER NOT NULL DEFAULT 0,"
        "    input_price REAL NOT NULL DEFAULT 0.0,"
        "    output_price REAL NOT NULL DEFAULT 0.0,"
        "    context_length INTEGER NOT NULL DEFAULT 0,"
        "    server_name TEXT NOT NULL DEFAULT 'OpenRouter',"
        "    is_selected INTEGER NOT NULL DEFAULT 0,"
        "    priority_order INTEGER NOT NULL DEFAULT 0,"
        "    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "    UNIQUE(provider, model_id)"
        ");"
        "CREATE TABLE IF NOT EXISTS server_agent ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    name TEXT UNIQUE NOT NULL,"
        "    url TEXT NOT NULL,"
        "    api_key TEXT DEFAULT '',"
        "    is_active INTEGER DEFAULT 1,"
        "    created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS model_penggunaan ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    provider TEXT NOT NULL,"
        "    model_id TEXT NOT NULL,"
        "    name TEXT NOT NULL,"
        "    rating REAL NOT NULL DEFAULT 0.0,"
        "    response_time_ms INTEGER NOT NULL DEFAULT 0,"
        "    input_price REAL NOT NULL DEFAULT 0.0,"
        "    output_price REAL NOT NULL DEFAULT 0.0,"
        "    context_length INTEGER NOT NULL DEFAULT 0,"
        "    priority_order INTEGER NOT NULL DEFAULT 0,"
        "    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "    UNIQUE(provider, model_id)"
        ");";

    char *errmsg = NULL;
    if (sqlite3_exec(db, v1_sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        char err_log[512];
        snprintf(err_log, sizeof(err_log), _T("str_db_mig_err_v1"), errmsg ? errmsg : "DDL v1 error");
        log_msg(cb, err_log);
        if (errmsg) sqlite3_free(errmsg);
        return false;
    }

    record_migration(db, 1, "001_initial_schema");
    log_msg(cb, _T("str_db_mig_v1_done"));
    return true;
}

// Migrasi 2: Indeks performa untuk kueri cepat
static bool apply_migration_v2(sqlite3 *db, db_migration_log_fn cb) {
    log_msg(cb, _T("str_db_mig_v2"));

    const char *v2_sql =
        "CREATE INDEX IF NOT EXISTS idx_model_rating ON model(rating DESC);"
        "CREATE INDEX IF NOT EXISTS idx_model_response ON model(response_time_ms ASC);"
        "CREATE INDEX IF NOT EXISTS idx_model_pricing ON model(input_price ASC, output_price ASC);"
        "CREATE INDEX IF NOT EXISTS idx_model_provider ON model(provider);"
        "CREATE INDEX IF NOT EXISTS idx_used_priority ON model_penggunaan(priority_order ASC);";

    char *errmsg = NULL;
    if (sqlite3_exec(db, v2_sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        char err_log[512];
        snprintf(err_log, sizeof(err_log), _T("str_db_mig_err_v2"), errmsg ? errmsg : "DDL v2 error");
        log_msg(cb, err_log);
        if (errmsg) sqlite3_free(errmsg);
        return false;
    }

    record_migration(db, 2, "002_performance_indexes");
    log_msg(cb, _T("str_db_mig_v2_done"));
    return true;
}

// Migrasi 3: Seeding server default dan model katalog awal jika kosong
static bool apply_migration_v3(sqlite3 *db, db_migration_log_fn cb) {
    log_msg(cb, _T("str_db_mig_v3"));

    // 1. Seed server OpenRouter default jika belum ada
    const char *seed_server_sql =
        "INSERT OR IGNORE INTO server_agent (name, url, api_key, is_active) "
        "VALUES ('OpenRouter', 'https://openrouter.ai/api/v1', '', 1);";
    sqlite3_exec(db, seed_server_sql, NULL, NULL, NULL);

    // 2. Cek apakah tabel model masih kosong. Jika kosong, tambahkan model-model utama
    sqlite3_stmt *stmt = NULL;
    int model_count = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM model;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            model_count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    if (model_count == 0) {
        log_msg(cb, _T("str_db_mig_v3_seed"));
        const char *seed_models_sql =
            "INSERT OR IGNORE INTO model (provider, model_id, name, rating, response_time_ms, input_price, output_price, context_length, server_name, is_selected, priority_order) VALUES "
            "('anthropic', 'anthropic/claude-3.5-sonnet', 'Claude 3.5 Sonnet', 5.00, 210, 0.000003, 0.000015, 200000, 'OpenRouter', 1, 1),"
            "('openai', 'openai/gpt-4o', 'GPT-4o (Omni)', 4.95, 195, 0.0000025, 0.000010, 128000, 'OpenRouter', 0, 2),"
            "('google', 'google/gemini-flash-1.5', 'Gemini 1.5 Flash', 4.88, 145, 0.00000035, 0.00000105, 1000000, 'OpenRouter', 0, 3),"
            "('meta-llama', 'meta-llama/llama-3.1-70b-instruct', 'Meta Llama 3.1 70B', 4.82, 220, 0.00000052, 0.00000075, 131072, 'OpenRouter', 0, 4),"
            "('mistralai', 'mistralai/mistral-large-2407', 'Mistral Large 2', 4.80, 230, 0.000002, 0.000006, 128000, 'OpenRouter', 0, 5),"
            "('qwen', 'qwen/qwen-2.5-72b-instruct', 'Qwen 2.5 72B Instruct', 4.85, 205, 0.0000004, 0.0000004, 32768, 'OpenRouter', 0, 6),"
            "('deepseek', 'deepseek/deepseek-chat', 'DeepSeek V2.5', 4.78, 180, 0.00000014, 0.00000028, 64000, 'OpenRouter', 0, 7);"
        ;
        sqlite3_exec(db, seed_models_sql, NULL, NULL, NULL);

        // Tambahkan model default aktif ke tabel model_penggunaan jika kosong
        const char *seed_used_sql =
            "INSERT OR IGNORE INTO model_penggunaan (provider, model_id, name, rating, response_time_ms, input_price, output_price, context_length, priority_order) "
            "VALUES ('anthropic', 'anthropic/claude-3.5-sonnet', 'Claude 3.5 Sonnet', 5.00, 210, 0.000003, 0.000015, 200000, 1);";
        sqlite3_exec(db, seed_used_sql, NULL, NULL, NULL);
    }

    record_migration(db, 3, "003_seed_default_catalog");
    log_msg(cb, _T("str_db_mig_v3_done"));
    return true;
}

// Verifikasi integritas tabel-tabel utama
static bool verify_required_tables(sqlite3 *db, char *err_buf, size_t err_buf_len) {
    const char *required_tables[] = {
        "model",
        "server_agent",
        "model_penggunaan",
        "_schema_migrations"
    };

    for (size_t i = 0; i < sizeof(required_tables)/sizeof(required_tables[0]); i++) {
        sqlite3_stmt *stmt = NULL;
        char query[256];
        snprintf(query, sizeof(query), "SELECT 1 FROM sqlite_master WHERE type='table' AND name='%s';", required_tables[i]);
        bool found = false;
        if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                found = true;
            }
            sqlite3_finalize(stmt);
        }

        if (!found) {
            if (err_buf) {
                snprintf(err_buf, err_buf_len, _T("str_db_err_table_missing"), required_tables[i]);
            }
            return false;
        }
    }
    return true;
}

bool db_migration_run_all(HWND hwndParent, db_migration_log_fn log_cb, char *err_buf, size_t err_buf_len) {
    // 1. Kunci aplikasi: disable window dan tampilkan kursor wait
    HCURSOR hOldCursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
    if (hwndParent && IsWindow(hwndParent)) {
        EnableWindow(hwndParent, FALSE);
        UpdateWindow(hwndParent);
    }

    log_msg(log_cb, _T("str_db_lock"));

    // 2. Pastikan folder kerja & folder downloads tersedia
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t *p = wcsrchr(exePath, L'\\');
    if (p) *(p + 1) = L'\0';

    wchar_t downloadsPath[MAX_PATH];
    wcsncpy(downloadsPath, exePath, MAX_PATH - 1);
    downloadsPath[MAX_PATH - 1] = L'\0';
    wcsncat(downloadsPath, L"downloads", MAX_PATH - wcslen(downloadsPath) - 1);
    CreateDirectoryW(downloadsPath, NULL);

    // 3. Tentukan path database SQLite
    wchar_t dbPathW[MAX_PATH];
    db_migration_get_db_path(dbPathW, MAX_PATH);

    char dbPathA[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, dbPathW, -1, dbPathA, MAX_PATH, NULL, NULL);

    sqlite3 *db = NULL;
    if (sqlite3_open(dbPathA, &db) != SQLITE_OK) {
        if (err_buf) {
            snprintf(err_buf, err_buf_len, _T("str_db_err_open"), sqlite3_errmsg(db));
        }
        if (db) sqlite3_close(db);

        // Buka kembali kunci jendela sebelum keluar
        if (hwndParent && IsWindow(hwndParent)) EnableWindow(hwndParent, TRUE);
        SetCursor(hOldCursor);
        return false;
    }

    // 4. Inisialisasi tabel metadata migrasi
    const char *init_meta_sql =
        "CREATE TABLE IF NOT EXISTS _schema_migrations ("
        "    version INTEGER PRIMARY KEY,"
        "    name TEXT NOT NULL,"
        "    applied_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");";
    char *meta_err = NULL;
    if (sqlite3_exec(db, init_meta_sql, NULL, NULL, &meta_err) != SQLITE_OK) {
        if (err_buf) {
            snprintf(err_buf, err_buf_len, _T("str_db_err_init_meta"), meta_err ? meta_err : "Unknown");
        }
        if (meta_err) sqlite3_free(meta_err);
        sqlite3_close(db);
        if (hwndParent && IsWindow(hwndParent)) EnableWindow(hwndParent, TRUE);
        SetCursor(hOldCursor);
        return false;
    }

    int current_version = get_current_schema_version(db);
    char ver_msg[128];
    snprintf(ver_msg, sizeof(ver_msg), _T("str_db_schema_version"), current_version);
    log_msg(log_cb, ver_msg);

    // 5. Jalankan migrasi secara bertahap jika versi belum tercapai
    bool success = true;

    if (current_version < 1) {
        sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
        if (apply_migration_v1(db, log_cb)) {
            sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        } else {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            success = false;
        }
    }

    if (success && current_version < 2) {
        sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
        if (apply_migration_v2(db, log_cb)) {
            sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        } else {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            success = false;
        }
    }

    if (success && current_version < 3) {
        sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
        if (apply_migration_v3(db, log_cb)) {
            sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        } else {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            success = false;
        }
    }

    // 6. Verifikasi akhir: semua tabel yang dibutuhkan harus ada
    if (success) {
        success = verify_required_tables(db, err_buf, err_buf_len);
    }

    sqlite3_close(db);

    // 7. Buka kunci aplikasi dan kembalikan status UI
    if (hwndParent && IsWindow(hwndParent)) {
        EnableWindow(hwndParent, TRUE);
        UpdateWindow(hwndParent);
    }
    SetCursor(hOldCursor);

    if (success) {
        log_msg(log_cb, _T("str_db_verified_ready"));
        log_msg(log_cb, _T("str_db_unlocked"));
    } else {
        log_msg(log_cb, _T("str_db_failed"));
    }

    return success;
}
