#include "internal/db/db.h"

#include <stdio.h>
#include <string.h>

static const char *kSchemaSql =
    "PRAGMA foreign_keys = ON;\n"
    "PRAGMA user_version = 2;\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS games (\n"
    "    id          INTEGER PRIMARY KEY,\n"
    "    system      TEXT    NOT NULL,\n"
    "    name        TEXT    NOT NULL,\n"
    "    rom_path    TEXT    NOT NULL UNIQUE,\n"
    "    image_path  TEXT,\n"
    "    last_played INTEGER,\n"
    "    playtime_s  INTEGER NOT NULL DEFAULT 0\n"
    ");\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS apps (\n"
    "    id                  INTEGER PRIMARY KEY,\n"
    "    pak_dir             TEXT NOT NULL UNIQUE,\n"
    "    name                TEXT NOT NULL,\n"
    "    icon                TEXT,\n"
    "    platform            TEXT,\n"
    "    pak_version         TEXT,\n"
    "    min_jawaka_version  TEXT\n"
    ");\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS favorites (\n"
    "    kind       TEXT NOT NULL CHECK (kind IN ('game','app')),\n"
    "    target_id  INTEGER NOT NULL,\n"
    "    added_at   INTEGER NOT NULL,\n"
    "    PRIMARY KEY (kind, target_id)\n"
    ");\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS recents (\n"
    "    kind        TEXT NOT NULL CHECK (kind IN ('game','app')),\n"
    "    target_id   INTEGER NOT NULL,\n"
    "    last_opened INTEGER NOT NULL,\n"
    "    duration_s  INTEGER NOT NULL DEFAULT 0,\n"
    "    PRIMARY KEY (kind, target_id)\n"
    ");\n"
    "\n"
    "CREATE VIRTUAL TABLE IF NOT EXISTS games_fts USING fts5(\n"
    "    name, system, content='games', content_rowid='id'\n"
    ");\n"
    "\n"
    "CREATE TRIGGER IF NOT EXISTS games_ai AFTER INSERT ON games BEGIN\n"
    "    INSERT INTO games_fts(rowid, name, system)\n"
    "        VALUES (new.id, new.name, new.system);\n"
    "END;\n"
    "\n"
    "CREATE TRIGGER IF NOT EXISTS games_ad AFTER DELETE ON games BEGIN\n"
    "    INSERT INTO games_fts(games_fts, rowid, name, system)\n"
    "        VALUES ('delete', old.id, old.name, old.system);\n"
    "END;\n"
    "\n"
    "CREATE TRIGGER IF NOT EXISTS games_au AFTER UPDATE ON games BEGIN\n"
    "    INSERT INTO games_fts(games_fts, rowid, name, system)\n"
    "        VALUES ('delete', old.id, old.name, old.system);\n"
    "    INSERT INTO games_fts(rowid, name, system)\n"
    "        VALUES (new.id, new.name, new.system);\n"
    "END;\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS settings (\n"
    "    key   TEXT PRIMARY KEY,\n"
    "    value TEXT NOT NULL\n"
    ");\n";

static int jw__exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) {
            fprintf(stderr, "sqlite exec failed: %s\n", err);
            sqlite3_free(err);
        } else {
            fprintf(stderr, "sqlite exec failed: %s\n", sqlite3_errmsg(db));
        }
        return -1;
    }
    return 0;
}

int jw_db_open(const char *path, sqlite3 **out) {
    if (!path || !out) {
        return -1;
    }
    return sqlite3_open(path, out) == SQLITE_OK ? 0 : -1;
}

int jw_db_apply_schema(sqlite3 *db) {
    if (!db) {
        return -1;
    }
    return jw__exec(db, kSchemaSql);
}

void jw_db_close(sqlite3 *db) {
    if (db) {
        sqlite3_close(db);
    }
}

int jw_db_reset_library(sqlite3 *db) {
    if (!db) {
        return -1;
    }
    return jw__exec(db, "DELETE FROM apps; DELETE FROM games;");
}

int jw_db_insert_game(sqlite3 *db, const char *system, const char *name, const char *rom_path, const char *image_path) {
    static const char *sql =
        "INSERT OR REPLACE INTO games (system, name, rom_path, image_path) "
        "VALUES (?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;

    if (!db || !system || !name || !rom_path) {
        return -1;
    }

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_text(stmt, 1, system, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, rom_path, -1, SQLITE_TRANSIENT);
    if (image_path && image_path[0]) {
        sqlite3_bind_text(stmt, 4, image_path, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 4);
    }

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int jw_db_insert_app(sqlite3 *db, const char *pak_dir, const char *name, const char *icon, const char *platform, const char *pak_version, const char *min_jawaka_version) {
    static const char *sql =
        "INSERT OR REPLACE INTO apps (pak_dir, name, icon, platform, pak_version, min_jawaka_version) "
        "VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;

    if (!db || !pak_dir || !name) {
        return -1;
    }

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_text(stmt, 1, pak_dir, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    if (icon && icon[0]) sqlite3_bind_text(stmt, 3, icon, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 3);
    if (platform && platform[0]) sqlite3_bind_text(stmt, 4, platform, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 4);
    if (pak_version && pak_version[0]) sqlite3_bind_text(stmt, 5, pak_version, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 5);
    if (min_jawaka_version && min_jawaka_version[0]) sqlite3_bind_text(stmt, 6, min_jawaka_version, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 6);

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

static int jw__query_int(sqlite3 *db, const char *sql, int *out) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    int value = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    *out = value;
    return 0;
}

static int jw__query_string(sqlite3 *db, const char *sql, char *out, size_t out_size) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    out[0] = '\0';
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text) {
            snprintf(out, out_size, "%s", text);
        }
    }
    sqlite3_finalize(stmt);
    return 0;
}

int jw_db_list_systems(const char *db_path, jw_system_entry *out, int max_count, int *out_count) {
    if (!db_path || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    static const char *sql =
        "SELECT system, COUNT(*) FROM games GROUP BY system ORDER BY system LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_int(stmt, 1, max_count);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_count < max_count) {
        const unsigned char *name = sqlite3_column_text(stmt, 0);
        int count = sqlite3_column_int(stmt, 1);
        if (name) {
            snprintf(out[*out_count].name, sizeof(out[*out_count].name), "%s", name);
            out[*out_count].game_count = count;
            (*out_count)++;
        }
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}

int jw_db_read_summary(const char *db_path, jw_library_summary *out) {
    if (!db_path || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    int rc = 0;
    rc |= jw__query_int(db, "SELECT COUNT(*) FROM games;", &out->game_count);
    rc |= jw__query_int(db, "SELECT COUNT(*) FROM apps;", &out->app_count);
    rc |= jw__query_string(db,
        "SELECT group_concat(system, ', ') FROM ("
        "SELECT DISTINCT system FROM games ORDER BY system LIMIT 4"
        ");",
        out->systems_summary, sizeof(out->systems_summary));
    rc |= jw__query_string(db,
        "SELECT group_concat(name, ', ') FROM ("
        "SELECT name FROM games ORDER BY name LIMIT 4"
        ");",
        out->sample_summary, sizeof(out->sample_summary));

    if (!out->systems_summary[0]) {
        snprintf(out->systems_summary, sizeof(out->systems_summary), "%s", "none");
    }
    if (!out->sample_summary[0]) {
        jw__query_string(db,
            "SELECT group_concat(name, ', ') FROM ("
            "SELECT name FROM apps ORDER BY name LIMIT 4"
            ");",
            out->sample_summary, sizeof(out->sample_summary));
    }
    if (!out->sample_summary[0]) {
        snprintf(out->sample_summary, sizeof(out->sample_summary), "%s", "none");
    }

    jw_db_close(db);
    return rc == 0 ? 0 : -1;
}

int jw_db_get_setting(const char *db_path, const char *key,
                       char *out, size_t out_size) {
    if (!db_path || !key || !out || out_size == 0) return -1;
    out[0] = '\0';

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;

    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql = "SELECT value FROM settings WHERE key = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text) snprintf(out, out_size, "%s", text);
    }
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}

int jw_db_set_setting(const char *db_path, const char *key, const char *value) {
    if (!db_path || !key || !value) return -1;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;

    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql =
        "INSERT INTO settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, key,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_get_theme_name(const char *db_path, char *out, size_t out_size) {
    return jw_db_get_setting(db_path, "theme_name", out, out_size);
}

int jw_db_list_apps(const char *db_path, jw_app_entry *out, int max_count, int *out_count) {
    if (!db_path || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    static const char *sql =
        "SELECT name, pak_dir, icon FROM apps ORDER BY name LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_int(stmt, 1, max_count);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_count < max_count) {
        const unsigned char *name = sqlite3_column_text(stmt, 0);
        const unsigned char *pak_dir = sqlite3_column_text(stmt, 1);
        const unsigned char *icon = sqlite3_column_text(stmt, 2);
        int i = *out_count;
        if (name) snprintf(out[i].name, sizeof(out[i].name), "%s", name);
        if (pak_dir) snprintf(out[i].pak_dir, sizeof(out[i].pak_dir), "%s", pak_dir);
        if (icon) snprintf(out[i].icon, sizeof(out[i].icon), "%s", icon);
        (*out_count)++;
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}

int jw_db_list_games_for_system(const char *db_path, const char *system,
                                jw_game_entry *out, int max_count, int *out_count) {
    if (!db_path || !system || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    static const char *sql =
        "SELECT system, name, rom_path, COALESCE(image_path, '') "
        "FROM games WHERE system = ? ORDER BY name LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, system, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, max_count);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_count < max_count) {
        const unsigned char *system_text = sqlite3_column_text(stmt, 0);
        const unsigned char *name_text = sqlite3_column_text(stmt, 1);
        const unsigned char *rom_text = sqlite3_column_text(stmt, 2);
        const unsigned char *image_text = sqlite3_column_text(stmt, 3);
        int i = *out_count;
        if (system_text) snprintf(out[i].system, sizeof(out[i].system), "%s", system_text);
        if (name_text) snprintf(out[i].name, sizeof(out[i].name), "%s", name_text);
        if (rom_text) snprintf(out[i].rom_path, sizeof(out[i].rom_path), "%s", rom_text);
        if (image_text) snprintf(out[i].image_path, sizeof(out[i].image_path), "%s", image_text);
        (*out_count)++;
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}
