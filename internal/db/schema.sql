-- internal/db/schema.sql
-- Non-authoritative reviewer-facing copy. The runtime schema is the
-- embedded string in internal/db/db.c. Keep these in sync by hand until
-- later codegen replaces this duplication.

PRAGMA foreign_keys = ON;
PRAGMA user_version = 4;

CREATE TABLE IF NOT EXISTS games (
    id          INTEGER PRIMARY KEY,
    system      TEXT    NOT NULL,
    name        TEXT    NOT NULL,
    rom_path    TEXT    NOT NULL UNIQUE,
    image_path  TEXT,
    last_played INTEGER,
    playtime_s  INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS apps (
    id                  INTEGER PRIMARY KEY,
    pak_dir             TEXT NOT NULL UNIQUE,
    name                TEXT NOT NULL,
    icon                TEXT,
    platform            TEXT,
    pak_version         TEXT,
    min_jawaka_version  TEXT
);

CREATE TABLE IF NOT EXISTS favorites (
    kind       TEXT NOT NULL CHECK (kind IN ('game','app')),
    target_id  INTEGER NOT NULL,
    added_at   INTEGER NOT NULL,
    PRIMARY KEY (kind, target_id)
);

CREATE TABLE IF NOT EXISTS recents (
    kind        TEXT NOT NULL CHECK (kind IN ('game','app')),
    target_id   INTEGER NOT NULL,
    last_opened INTEGER NOT NULL,
    duration_s  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (kind, target_id)
);

CREATE VIRTUAL TABLE IF NOT EXISTS games_fts USING fts5(
    name, system, content='games', content_rowid='id'
);

CREATE TRIGGER IF NOT EXISTS games_ai AFTER INSERT ON games BEGIN
    INSERT INTO games_fts(rowid, name, system)
        VALUES (new.id, new.name, new.system);
END;

CREATE TRIGGER IF NOT EXISTS games_ad AFTER DELETE ON games BEGIN
    INSERT INTO games_fts(games_fts, rowid, name, system)
        VALUES ('delete', old.id, old.name, old.system);
END;

CREATE TRIGGER IF NOT EXISTS games_au AFTER UPDATE ON games BEGIN
    INSERT INTO games_fts(games_fts, rowid, name, system)
        VALUES ('delete', old.id, old.name, old.system);
    INSERT INTO games_fts(rowid, name, system)
        VALUES (new.id, new.name, new.system);
END;

CREATE VIRTUAL TABLE IF NOT EXISTS apps_fts USING fts5(
    name, pak_dir, content='apps', content_rowid='id'
);

CREATE TRIGGER IF NOT EXISTS apps_ai AFTER INSERT ON apps BEGIN
    INSERT INTO apps_fts(rowid, name, pak_dir)
        VALUES (new.id, new.name, new.pak_dir);
END;

CREATE TRIGGER IF NOT EXISTS apps_ad AFTER DELETE ON apps BEGIN
    INSERT INTO apps_fts(apps_fts, rowid, name, pak_dir)
        VALUES ('delete', old.id, old.name, old.pak_dir);
END;

CREATE TRIGGER IF NOT EXISTS apps_au AFTER UPDATE ON apps BEGIN
    INSERT INTO apps_fts(apps_fts, rowid, name, pak_dir)
        VALUES ('delete', old.id, old.name, old.pak_dir);
    INSERT INTO apps_fts(rowid, name, pak_dir)
        VALUES (new.id, new.name, new.pak_dir);
END;

CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS system_settings (
    system     TEXT NOT NULL,
    key        TEXT NOT NULL,
    value      TEXT NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (system, key)
);

CREATE TABLE IF NOT EXISTS game_settings (
    game_id    INTEGER NOT NULL,
    key        TEXT NOT NULL,
    value      TEXT NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (game_id, key),
    FOREIGN KEY (game_id) REFERENCES games(id) ON DELETE CASCADE
);
