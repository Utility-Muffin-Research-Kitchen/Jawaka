#include "internal/db/db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(sqlite3 *db, const char *message) {
    fprintf(stderr, "schema-v6-test: %s%s%s\n", message,
            db ? ": " : "", db ? sqlite3_errmsg(db) : "");
    exit(1);
}

static void exec_ok(sqlite3 *db, const char *sql) {
    char *error = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
        fprintf(stderr, "schema-v6-test SQL: %s\n", error ? error : "unknown");
        sqlite3_free(error);
        exit(1);
    }
}

static long long scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW) {
        fail(db, sql);
    }
    long long value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

int main(void) {
    char fresh[] = "/tmp/jawaka-schema-fresh.XXXXXX";
    int fresh_fd = mkstemp(fresh);
    if (fresh_fd < 0) fail(NULL, "fresh mkstemp");
    close(fresh_fd);
    unlink(fresh);
    sqlite3 *db = NULL;
    if (jw_db_open(fresh, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        scalar(db, "PRAGMA user_version") != 6 ||
        scalar(db, "SELECT COUNT(*) FROM pragma_table_info('games') "
                   "WHERE name IN ('source_id','rom_relpath','image_root_kind',"
                   "'image_relpath')") != 4) {
        fail(db, "fresh v6 schema");
    }
    sqlite3_close(db);
    unlink(fresh);

    char path[] = "/tmp/jawaka-schema-v6.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) fail(NULL, "mkstemp");
    close(fd);
    unlink(path);

    if (sqlite3_open(path, &db) != SQLITE_OK) fail(db, "open");
    exec_ok(db,
        "PRAGMA user_version=5;"
        "CREATE TABLE games(id INTEGER PRIMARY KEY,system TEXT NOT NULL,"
        "name TEXT NOT NULL,rom_path TEXT NOT NULL UNIQUE,image_path TEXT,"
        "last_played INTEGER,playtime_s INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE favorites(kind TEXT,target_id INTEGER,added_at INTEGER,"
        "PRIMARY KEY(kind,target_id));"
        "CREATE TABLE recents(kind TEXT,target_id INTEGER,last_opened INTEGER,"
        "duration_s INTEGER,PRIMARY KEY(kind,target_id));"
        "CREATE TABLE game_settings(game_id INTEGER,key TEXT,value TEXT,"
        "updated_at INTEGER,PRIMARY KEY(game_id,key),"
        "FOREIGN KEY(game_id) REFERENCES games(id) ON DELETE CASCADE);"
        "INSERT INTO games VALUES"
        "(10,'NES','old-a','/mnt/sdcard/Roms/NES/game.zip',"
        "'/mnt/sdcard/Images/NES/game.png',1000,5),"
        "(20,'NES','old-b','/media/sdcard1/Roms/NES/game.zip',"
        "'/media/sdcard1/Images/NES/game.png',2000,7),"
        "(30,'PORTS','source-art','Roms/PORTS/source.sh',"
        "'.catalog/PORTS/source.png',NULL,0),"
        "(40,'PORTS','bad-art','Roms/PORTS/bad.sh',"
        "'/unknown/art/bad.png',NULL,0);"
        "INSERT INTO favorites VALUES('game',20,50);"
        "INSERT INTO recents VALUES('game',10,100,1),('game',20,200,2);"
        "INSERT INTO game_settings VALUES"
        "(10,'display_name','Older',10),(20,'display_name','Newer',20);");

    if (jw_db_apply_schema(db) != 0) fail(db, "migration");
    if (scalar(db, "PRAGMA user_version") != 6) fail(db, "wrong version");
    if (scalar(db, "SELECT COUNT(*) FROM games") != 3 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE id=10") != 1 ||
        scalar(db, "SELECT playtime_s FROM games WHERE id=10") != 12 ||
        scalar(db, "SELECT last_played FROM games WHERE id=10") != 2000 ||
        scalar(db, "SELECT COUNT(*) FROM favorites WHERE target_id=10") != 1 ||
        scalar(db, "SELECT last_opened FROM recents WHERE target_id=10") != 200 ||
        scalar(db, "SELECT COUNT(*) FROM game_settings "
                   "WHERE game_id=10 AND value='Newer'") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE source_id='secondary_sd' "
                   "AND rom_relpath='NES/game.zip'") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE id=30 "
                   "AND image_root_kind='source' "
                   "AND image_relpath='.catalog/PORTS/source.png'") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE id=40 "
                   "AND image_root_kind IS NULL AND image_relpath IS NULL") != 1) {
        fail(db, "state was not merged");
    }
    sqlite3_close(db);

    char backup[4096];
    snprintf(backup, sizeof(backup), "%s.v5-backup", path);
    if (sqlite3_open_v2(backup, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        scalar(db, "PRAGMA user_version") != 5 ||
        scalar(db, "SELECT COUNT(*) FROM games") != 4) {
        fail(db, "backup invalid");
    }
    sqlite3_close(db);

    if (jw_db_open(path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        jw_db_scan_begin(db) != 0 ||
        jw_db_insert_game_stable(db, "NES", "moved", "secondary_sd",
            "NES/game.zip", "/media/sdcard1/Roms/NES/game.zip",
            "images", "NES/game.png",
            "/media/sdcard1/Images/NES/game.png") != 0 ||
        jw_db_insert_game_stable(db, "NES", "primary", "primary",
            "NES/primary.zip", "Roms/NES/primary.zip", NULL, NULL, NULL) != 0 ||
        jw_db_scan_source_complete(db, "primary") != 0 ||
        jw_db_scan_prune(db) != 0) {
        fail(db, "source-scoped prune setup");
    }
    if (scalar(db, "SELECT COUNT(*) FROM games WHERE source_id='secondary_sd'") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE source_id='primary'") != 1 ||
        scalar(db, "SELECT id FROM games WHERE source_id='secondary_sd'") != 10 ||
        scalar(db, "SELECT playtime_s FROM games WHERE id=10") != 12 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE id=10 AND "
                   "rom_path='/media/sdcard1/Roms/NES/game.zip'") != 1) {
        fail(db, "unscanned source was pruned");
    }
    if (jw_db_record_play_by_id(path, 10, 3) != 0 ||
        scalar(db, "SELECT playtime_s FROM games WHERE id=10") != 15) {
        fail(db, "id-based play accounting");
    }
    if (jw_db_scan_begin(db) != 0 ||
        jw_db_scan_source_complete(db, "primary") != 0 ||
        jw_db_scan_prune(db) != 0 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE source_id='primary'") != 0 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE source_id='secondary_sd'") != 1) {
        fail(db, "completed source was not pruned");
    }
    sqlite3_close(db);

    char unsafe[] = "/tmp/jawaka-schema-unsafe.XXXXXX";
    fd = mkstemp(unsafe);
    if (fd < 0) fail(NULL, "unsafe mkstemp");
    close(fd);
    if (sqlite3_open(unsafe, &db) != SQLITE_OK) fail(db, "unsafe open");
    exec_ok(db,
        "PRAGMA user_version=5;"
        "CREATE TABLE games(id INTEGER PRIMARY KEY,system TEXT NOT NULL,"
        "name TEXT NOT NULL,rom_path TEXT NOT NULL UNIQUE,image_path TEXT,"
        "last_played INTEGER,playtime_s INTEGER NOT NULL DEFAULT 0);"
        "INSERT INTO games VALUES"
        "(1,'NES','unsafe','/unknown/card/Roms/NES/game.zip',NULL,NULL,0);");
    if (jw_db_apply_schema(db) == 0 ||
        scalar(db, "PRAGMA user_version") != 5 ||
        scalar(db, "SELECT COUNT(*) FROM games") != 1) {
        fail(db, "unsafe migration was not rolled back");
    }
    sqlite3_close(db);

    char future[] = "/tmp/jawaka-schema-future.XXXXXX";
    fd = mkstemp(future);
    if (fd < 0 || sqlite3_open(future, &db) != SQLITE_OK) fail(db, "future open");
    close(fd);
    exec_ok(db, "PRAGMA user_version=7;");
    if (jw_db_apply_schema(db) == 0) fail(db, "future schema accepted");
    sqlite3_close(db);

    unlink(future);
    unlink(unsafe);
    unlink(backup);
    unlink(path);
    puts("PASS schema-v6-test");
    return 0;
}
