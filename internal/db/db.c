#include "internal/db/db.h"

#include <stdio.h>
#include <string.h>

static const char *kSchemaSql =
    "PRAGMA foreign_keys = ON;\n"
    "PRAGMA user_version = 1;\n"
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
    "END;\n";

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
