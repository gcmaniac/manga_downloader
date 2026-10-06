#ifndef DB_MIGRATION_H
#define DB_MIGRATION_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

typedef void (*db_migration_log_fn)(const char *message);

// Mengunci window hwndParent, memeriksa dan menjalankan migrasi database SQLite
// sampai seluruh file dan tabel yang dibutuhkan tersedia dan valid.
// Setelah selesai, kunci window akan dilepas.
bool db_migration_run_all(HWND hwndParent, db_migration_log_fn log_cb, char *err_buf, size_t err_buf_len);

// Mendapatkan path absolut ke file database SQLite
void db_migration_get_db_path(wchar_t *path_out, size_t max_len);

#endif // DB_MIGRATION_H
