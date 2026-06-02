#include "internal/db/db.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *kSchemaSql =
    "PRAGMA foreign_keys = ON;\n"
    "PRAGMA user_version = 3;\n"
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
    "CREATE VIRTUAL TABLE IF NOT EXISTS apps_fts USING fts5(\n"
    "    name, pak_dir, content='apps', content_rowid='id'\n"
    ");\n"
    "\n"
    "CREATE TRIGGER IF NOT EXISTS apps_ai AFTER INSERT ON apps BEGIN\n"
    "    INSERT INTO apps_fts(rowid, name, pak_dir)\n"
    "        VALUES (new.id, new.name, new.pak_dir);\n"
    "END;\n"
    "\n"
    "CREATE TRIGGER IF NOT EXISTS apps_ad AFTER DELETE ON apps BEGIN\n"
    "    INSERT INTO apps_fts(apps_fts, rowid, name, pak_dir)\n"
    "        VALUES ('delete', old.id, old.name, old.pak_dir);\n"
    "END;\n"
    "\n"
    "CREATE TRIGGER IF NOT EXISTS apps_au AFTER UPDATE ON apps BEGIN\n"
    "    INSERT INTO apps_fts(apps_fts, rowid, name, pak_dir)\n"
    "        VALUES ('delete', old.id, old.name, old.pak_dir);\n"
    "    INSERT INTO apps_fts(rowid, name, pak_dir)\n"
    "        VALUES (new.id, new.name, new.pak_dir);\n"
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
    if (sqlite3_open(path, out) != SQLITE_OK) {
        return -1;
    }
    /* Launcher, menu, and daemon each open their own connection on the same
       SQLite file. Without a busy timeout a write that collides with another
       connection's lock (e.g. a favorite/recent write during a daemon scan)
       fails immediately with SQLITE_BUSY. Wait briefly instead. */
    sqlite3_busy_timeout(*out, 2000);
    return 0;
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
    return jw__exec(db,
        "INSERT INTO games_fts(games_fts) VALUES('rebuild');"
        "INSERT INTO apps_fts(apps_fts) VALUES('rebuild');"
        "DELETE FROM apps; DELETE FROM games;");
}

/* Records a path in the per-scan "seen" temp table so jw_db_scan_prune can
   later remove only the rows whose ROM/pak vanished from disk. The temp table
   is created by jw_db_scan_begin. */
static int jw__mark_seen(sqlite3 *db, const char *seen_sql, const char *path) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, seen_sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int jw_db_insert_game(sqlite3 *db, const char *system, const char *name, const char *rom_path, const char *image_path) {
    /* Upsert keyed on the UNIQUE rom_path so an existing game keeps its id
       across rescans (favorites/recents reference that id). Only the scanned
       fields are updated; last_played and playtime_s are deliberately left
       intact. */
    static const char *sql =
        "INSERT INTO games (system, name, rom_path, image_path) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(rom_path) DO UPDATE SET "
        "system = excluded.system, name = excluded.name, image_path = excluded.image_path;";
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
    if (rc != 0) {
        return -1;
    }

    return jw__mark_seen(db, "INSERT OR IGNORE INTO _seen_games (rom_path) VALUES (?);", rom_path);
}

int jw_db_insert_app(sqlite3 *db, const char *pak_dir, const char *name, const char *icon, const char *platform, const char *pak_version, const char *min_jawaka_version) {
    /* Upsert keyed on the UNIQUE pak_dir, mirroring jw_db_insert_game, so an
       app keeps its id across rescans. */
    static const char *sql =
        "INSERT INTO apps (pak_dir, name, icon, platform, pak_version, min_jawaka_version) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(pak_dir) DO UPDATE SET "
        "name = excluded.name, icon = excluded.icon, platform = excluded.platform, "
        "pak_version = excluded.pak_version, min_jawaka_version = excluded.min_jawaka_version;";
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
    if (rc != 0) {
        return -1;
    }

    return jw__mark_seen(db, "INSERT OR IGNORE INTO _seen_apps (pak_dir) VALUES (?);", pak_dir);
}

int jw_db_scan_begin(sqlite3 *db) {
    if (!db) {
        return -1;
    }
    /* Per-connection temp tables track which ROMs/paks the current scan saw.
       Recreated-then-cleared each scan; dropped implicitly when the daemon
       connection closes. */
    return jw__exec(db,
        "CREATE TEMP TABLE IF NOT EXISTS _seen_games (rom_path TEXT PRIMARY KEY);"
        "CREATE TEMP TABLE IF NOT EXISTS _seen_apps (pak_dir TEXT PRIMARY KEY);"
        "DELETE FROM _seen_games;"
        "DELETE FROM _seen_apps;");
}

int jw_db_scan_prune(sqlite3 *db) {
    if (!db) {
        return -1;
    }
    /* Remove games/apps whose path was not seen this scan (deleted from disk);
       the FTS delete triggers keep the search index in sync. Then drop any
       favorites/recents that pointed at a now-removed row so they can never
       resolve to the wrong entry after an id is later reused. */
    return jw__exec(db,
        "DELETE FROM games WHERE rom_path NOT IN (SELECT rom_path FROM _seen_games);"
        "DELETE FROM apps WHERE pak_dir NOT IN (SELECT pak_dir FROM _seen_apps);"
        "DELETE FROM favorites WHERE kind = 'game' AND target_id NOT IN (SELECT id FROM games);"
        "DELETE FROM favorites WHERE kind = 'app'  AND target_id NOT IN (SELECT id FROM apps);"
        "DELETE FROM recents   WHERE kind = 'game' AND target_id NOT IN (SELECT id FROM games);"
        "DELETE FROM recents   WHERE kind = 'app'  AND target_id NOT IN (SELECT id FROM apps);");
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
    rc |= jw__query_int(db, "SELECT COUNT(DISTINCT system) FROM games;", &out->system_count);
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

int jw_db_set_settings(const char *db_path, const char *const *keys,
                       const char *const *values, int count) {
    if (!db_path || !keys || !values || count <= 0) return -1;

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

    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    int rc = 0;
    for (int i = 0; i < count; i++) {
        if (!keys[i] || !values[i]) { rc = -1; break; }
        sqlite3_bind_text(stmt, 1, keys[i],   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, values[i], -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) rc = -1;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        if (rc != 0) break;
    }
    sqlite3_exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);

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
        "SELECT g.id, g.system, g.name, g.rom_path, COALESCE(g.image_path, ''), "
        "EXISTS(SELECT 1 FROM favorites f WHERE f.kind = 'game' AND f.target_id = g.id) "
        "FROM games g WHERE g.system = ? ORDER BY g.name LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, system, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, max_count);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_count < max_count) {
        const unsigned char *system_text = sqlite3_column_text(stmt, 1);
        const unsigned char *name_text = sqlite3_column_text(stmt, 2);
        const unsigned char *rom_text = sqlite3_column_text(stmt, 3);
        const unsigned char *image_text = sqlite3_column_text(stmt, 4);
        int i = *out_count;
        out[i].id = sqlite3_column_int(stmt, 0);
        if (system_text) snprintf(out[i].system, sizeof(out[i].system), "%s", system_text);
        if (name_text) snprintf(out[i].name, sizeof(out[i].name), "%s", name_text);
        if (rom_text) snprintf(out[i].rom_path, sizeof(out[i].rom_path), "%s", rom_text);
        if (image_text) snprintf(out[i].image_path, sizeof(out[i].image_path), "%s", image_text);
        out[i].favorite = sqlite3_column_int(stmt, 5);
        (*out_count)++;
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}

int jw_db_set_favorite(const char *db_path, const char *kind, int target_id, int on) {
    if (!db_path || !kind || target_id <= 0) {
        return -1;
    }

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    static const char *add_sql =
        "INSERT OR IGNORE INTO favorites (kind, target_id, added_at) "
        "VALUES (?, ?, strftime('%s','now'));";
    static const char *del_sql =
        "DELETE FROM favorites WHERE kind = ? AND target_id = ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, on ? add_sql : del_sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, target_id);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_list_favorite_games(const char *db_path, jw_game_entry *out,
                              int max_count, int *out_count) {
    if (!db_path || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    static const char *sql =
        "SELECT g.id, g.system, g.name, g.rom_path, COALESCE(g.image_path, '') "
        "FROM games g JOIN favorites f ON f.kind = 'game' AND f.target_id = g.id "
        "ORDER BY f.added_at DESC, g.name LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_int(stmt, 1, max_count);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_count < max_count) {
        const unsigned char *system_text = sqlite3_column_text(stmt, 1);
        const unsigned char *name_text = sqlite3_column_text(stmt, 2);
        const unsigned char *rom_text = sqlite3_column_text(stmt, 3);
        const unsigned char *image_text = sqlite3_column_text(stmt, 4);
        int i = *out_count;
        out[i].id = sqlite3_column_int(stmt, 0);
        if (system_text) snprintf(out[i].system, sizeof(out[i].system), "%s", system_text);
        if (name_text) snprintf(out[i].name, sizeof(out[i].name), "%s", name_text);
        if (rom_text) snprintf(out[i].rom_path, sizeof(out[i].rom_path), "%s", rom_text);
        if (image_text) snprintf(out[i].image_path, sizeof(out[i].image_path), "%s", image_text);
        out[i].favorite = 1;
        (*out_count)++;
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}

int jw_db_record_play(const char *db_path, const char *rom_path, int duration_s) {
    if (!db_path || !rom_path || !rom_path[0]) {
        return -1;
    }
    if (duration_s < 0) duration_s = 0;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    /* Resolve the game's stable id from its rom_path. */
    int id = -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id FROM games WHERE rom_path = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, rom_path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (id < 0) {
        jw_db_close(db);
        return -1;
    }

    /* Cumulative playtime + last-played timestamp on the game row. */
    if (sqlite3_prepare_v2(db,
            "UPDATE games SET playtime_s = playtime_s + ?, last_played = strftime('%s','now') "
            "WHERE id = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, duration_s);
        sqlite3_bind_int(stmt, 2, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* Recents row: newest open wins; duration_s holds the last session length. */
    if (sqlite3_prepare_v2(db,
            "INSERT INTO recents (kind, target_id, last_opened, duration_s) "
            "VALUES ('game', ?, strftime('%s','now'), ?) "
            "ON CONFLICT(kind, target_id) DO UPDATE SET "
            "last_opened = excluded.last_opened, duration_s = excluded.duration_s;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        sqlite3_bind_int(stmt, 2, duration_s);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    jw_db_close(db);
    return 0;
}

int jw_db_list_recent_games(const char *db_path, jw_game_entry *out,
                            int max_count, int *out_count) {
    if (!db_path || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    /* Most-recently-opened first; carry the favorite flag so recents rows show
       the star too. */
    static const char *sql =
        "SELECT g.id, g.system, g.name, g.rom_path, COALESCE(g.image_path, ''), "
        "EXISTS(SELECT 1 FROM favorites f WHERE f.kind = 'game' AND f.target_id = g.id) "
        "FROM games g JOIN recents r ON r.kind = 'game' AND r.target_id = g.id "
        "ORDER BY r.last_opened DESC LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_int(stmt, 1, max_count);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_count < max_count) {
        const unsigned char *system_text = sqlite3_column_text(stmt, 1);
        const unsigned char *name_text = sqlite3_column_text(stmt, 2);
        const unsigned char *rom_text = sqlite3_column_text(stmt, 3);
        const unsigned char *image_text = sqlite3_column_text(stmt, 4);
        int i = *out_count;
        out[i].id = sqlite3_column_int(stmt, 0);
        if (system_text) snprintf(out[i].system, sizeof(out[i].system), "%s", system_text);
        if (name_text) snprintf(out[i].name, sizeof(out[i].name), "%s", name_text);
        if (rom_text) snprintf(out[i].rom_path, sizeof(out[i].rom_path), "%s", rom_text);
        if (image_text) snprintf(out[i].image_path, sizeof(out[i].image_path), "%s", image_text);
        out[i].favorite = sqlite3_column_int(stmt, 5);
        (*out_count)++;
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}

static int jw__build_fts_query(const char *query, char *out, size_t out_size) {
    if (!query || !out || out_size == 0) {
        return -1;
    }

    out[0] = '\0';
    size_t used = 0;
    int token_count = 0;

    for (const unsigned char *p = (const unsigned char *)query; *p; ) {
        while (*p && !isalnum(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }

        char token[64];
        size_t token_len = 0;
        while (*p && isalnum(*p)) {
            if (token_len + 1 < sizeof(token)) {
                token[token_len++] = (char)tolower(*p);
            }
            p++;
        }
        token[token_len] = '\0';
        if (token_len == 0) {
            continue;
        }

        size_t needed = token_len + 1u + (token_count > 0 ? 1u : 0u);
        if (used + needed + 1u >= out_size) {
            break;
        }

        if (token_count > 0) {
            out[used++] = ' ';
        }
        memcpy(out + used, token, token_len);
        used += token_len;
        out[used++] = '*';
        out[used] = '\0';
        token_count++;
    }

    return token_count > 0 ? 0 : 1;
}

int jw_db_search_library(const char *db_path, const char *query,
                         jw_search_result *out, int max_count, int *out_count) {
    if (!db_path || !query || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    char fts_query[256];
    int query_rc = jw__build_fts_query(query, fts_query, sizeof(fts_query));
    if (query_rc > 0) {
        return 0;
    }
    if (query_rc < 0) {
        return -1;
    }

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    static const char *sql =
        "SELECT kind, name, system, rom_path, image_path, pak_dir, icon FROM ("
        "  SELECT 0 AS kind, games.name AS name, games.system AS system,"
        "         games.rom_path AS rom_path, COALESCE(games.image_path, '') AS image_path,"
        "         '' AS pak_dir, '' AS icon, bm25(games_fts) AS score"
        "    FROM games_fts JOIN games ON games_fts.rowid = games.id"
        "   WHERE games_fts MATCH ?"
        "  UNION ALL"
        "  SELECT 1 AS kind, apps.name AS name, '' AS system,"
        "         '' AS rom_path, '' AS image_path,"
        "         apps.pak_dir AS pak_dir, COALESCE(apps.icon, '') AS icon,"
        "         bm25(apps_fts) AS score"
        "    FROM apps_fts JOIN apps ON apps_fts.rowid = apps.id"
        "   WHERE apps_fts MATCH ?"
        ") ORDER BY score ASC, kind ASC, name ASC LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, fts_query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, fts_query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, max_count);

    int step_rc = SQLITE_ROW;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW && *out_count < max_count) {
        int i = *out_count;
        out[i].kind = sqlite3_column_int(stmt, 0) == 1 ? JW_SEARCH_APP : JW_SEARCH_GAME;

        const unsigned char *name = sqlite3_column_text(stmt, 1);
        const unsigned char *system = sqlite3_column_text(stmt, 2);
        const unsigned char *rom_path = sqlite3_column_text(stmt, 3);
        const unsigned char *image_path = sqlite3_column_text(stmt, 4);
        const unsigned char *pak_dir = sqlite3_column_text(stmt, 5);
        const unsigned char *icon = sqlite3_column_text(stmt, 6);

        if (name) snprintf(out[i].name, sizeof(out[i].name), "%s", name);
        if (system) snprintf(out[i].system, sizeof(out[i].system), "%s", system);
        if (rom_path) snprintf(out[i].rom_path, sizeof(out[i].rom_path), "%s", rom_path);
        if (image_path) snprintf(out[i].image_path, sizeof(out[i].image_path), "%s", image_path);
        if (pak_dir) snprintf(out[i].pak_dir, sizeof(out[i].pak_dir), "%s", pak_dir);
        if (icon) snprintf(out[i].icon, sizeof(out[i].icon), "%s", icon);
        (*out_count)++;
    }

    int rc = (step_rc == SQLITE_DONE || *out_count >= max_count) ? 0 : -1;
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}
