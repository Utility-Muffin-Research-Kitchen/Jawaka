#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/store/pakrat_txn.h"

#include "internal/store/pakrat_recovery.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #condition);                                            \
            failures++;                                                     \
        }                                                                   \
    } while (0)

typedef struct {
    char root[PATH_MAX];
    char primary[PATH_MAX];
    char secondary[PATH_MAX];
    char absent[PATH_MAX];
    char state[PATH_MAX];
    char db[PATH_MAX];
    char runtime[PATH_MAX];
    char primary_userdata[PATH_MAX];
    char secondary_userdata[PATH_MAX];
    char primary_apps[PATH_MAX];
    char secondary_apps[PATH_MAX];
    jw_pakrat_context ctx;
} txn_fixture;

typedef struct {
    char target[PATH_MAX];
    char stage[PATH_MAX];
    char rollback[PATH_MAX];
    char origin[PATH_MAX];
    char trust[PATH_MAX];
    char history[PATH_MAX];
    char control[PATH_MAX];
} uninstall_fixture_paths;

static void join_path(char *out, size_t out_size, const char *a,
                      const char *b) {
    int n = snprintf(out, out_size, "%s/%s", a, b);
    if (n < 0 || (size_t)n >= out_size) {
        fprintf(stderr, "path overflow\n");
        exit(2);
    }
}

static void write_file(const char *path, const char *content, mode_t mode) {
    if (jw__pakrat_mkdir_parent(path) != 0) {
        fprintf(stderr, "mkdir parent failed: %s\n", path);
        exit(2);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0 || write(fd, content, strlen(content)) !=
                      (ssize_t)strlen(content) || close(fd) != 0) {
        fprintf(stderr, "write failed: %s\n", path);
        exit(2);
    }
}

static long long scalar_path(const char *path, const char *sql) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    long long result = -1;
    if (sqlite3_open(path, &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        result = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (db) sqlite3_close(db);
    return result;
}

static void fixture_env(txn_fixture *f) {
    char roots[PATH_MAX * 3];
    char userdata[PATH_MAX * 3];
    char apps[PATH_MAX * 3];
    snprintf(roots, sizeof(roots), "%s:%s:%s", f->primary, f->secondary,
             f->absent);
    snprintf(userdata, sizeof(userdata), "%s:%s:%s/.userdata/mac",
             f->primary_userdata, f->secondary_userdata, f->absent);
    snprintf(apps, sizeof(apps), "%s:%s:%s/Apps", f->primary_apps,
             f->secondary_apps, f->absent);
    setenv("PLATFORM", "mac", 1);
    setenv("SDCARD_PATH", f->primary, 1);
    setenv("SDCARD_PATHS", roots, 1);
    setenv("USERDATA_PATH", f->primary_userdata, 1);
    setenv("USERDATA_PATHS", userdata, 1);
    setenv("APPS_PATH", f->primary_apps, 1);
    setenv("APPS_PATHS", apps, 1);
}

static void fixture_setup(txn_fixture *f) {
    memset(f, 0, sizeof(*f));
    snprintf(f->root, sizeof(f->root), "/tmp/jw-pakrat-txn.XXXXXX");
    if (!mkdtemp(f->root)) {
        fprintf(stderr, "mkdtemp: %s\n", strerror(errno));
        exit(2);
    }
    join_path(f->primary, sizeof(f->primary), f->root, "primary");
    join_path(f->secondary, sizeof(f->secondary), f->root, "secondary");
    join_path(f->absent, sizeof(f->absent), f->root, "absent");
    join_path(f->state, sizeof(f->state), f->primary, ".umrk/mac");
    join_path(f->db, sizeof(f->db), f->state, "library.db");
    join_path(f->runtime, sizeof(f->runtime), f->root, "runtime");
    join_path(f->primary_userdata, sizeof(f->primary_userdata), f->primary,
              ".userdata/mac");
    join_path(f->secondary_userdata, sizeof(f->secondary_userdata),
              f->secondary, ".userdata/mac");
    join_path(f->primary_apps, sizeof(f->primary_apps), f->primary, "Apps");
    join_path(f->secondary_apps, sizeof(f->secondary_apps), f->secondary,
              "Apps");
    if (jw__pakrat_mkdir_p(f->primary_userdata, 0700) != 0 ||
        jw__pakrat_mkdir_p(f->secondary_userdata, 0700) != 0 ||
        jw__pakrat_mkdir_p(f->primary_apps, 0700) != 0 ||
        jw__pakrat_mkdir_p(f->secondary_apps, 0700) != 0 ||
        jw__pakrat_mkdir_p(f->state, 0700) != 0 ||
        jw__pakrat_mkdir_p(f->runtime, 0700) != 0) {
        fprintf(stderr, "fixture mkdir failed\n");
        exit(2);
    }
    fixture_env(f);
    snprintf(f->ctx.platform, sizeof(f->ctx.platform), "mac");
    snprintf(f->ctx.sdcard_root, sizeof(f->ctx.sdcard_root), "%s",
             f->primary);
    snprintf(f->ctx.state_dir, sizeof(f->ctx.state_dir), "%s", f->state);
    snprintf(f->ctx.db_path, sizeof(f->ctx.db_path), "%s", f->db);
    snprintf(f->ctx.runtime_dir, sizeof(f->ctx.runtime_dir), "%s",
             f->runtime);
}

static void fixture_teardown(txn_fixture *f) {
    (void)jw__pakrat_remove_tree(f->root);
}

static void write_real_pak(txn_fixture *f, const char *pak_root) {
    char run[PATH_MAX];
    char manifest[PATH_MAX];
    join_path(run, sizeof(run), pak_root, "bin/run.sh");
    join_path(manifest, sizeof(manifest), pak_root, "pak.json");
    write_file(run, "#!/bin/sh\nwhile :; do sleep 1; done\n", 0700);
    write_file(
        manifest,
        "{\"id\":\"org.umrk.test.txn\",\"name\":\"TXN Fixture\","
        "\"platform\":\"mac\",\"pak_version\":\"1.0.0\","
        "\"service\":{\"schema\":1,\"id\":\"org.umrk.test.txn\","
        "\"run\":{\"path\":\"bin/run.sh\",\"args\":[]},"
        "\"restart\":\"no\",\"default_enabled\":false,"
        "\"stop_grace_ms\":100},"
        "\"state\":{\"root\":\"Syncthing\","
        "\"revoke_on_uninstall\":[\"leaf/trusted.json\"],"
        "\"retained_roots\":[\"Syncthing\"]}}\n",
        0600);
}

static void write_floor_pak(const char *pak_root) {
    char launch[PATH_MAX];
    char manifest[PATH_MAX];
    join_path(launch, sizeof(launch), pak_root, "launch.sh");
    join_path(manifest, sizeof(manifest), pak_root, "pak.json");
    write_file(launch, "#!/bin/sh\nexit 0\n", 0700);
    write_file(manifest,
               "{\"id\":\"org.umrk.test.txn\",\"name\":\"TXN Floor\","
               "\"platform\":\"mac\",\"pak_version\":\"0.0.1\"}\n",
               0600);
}

static void test_manifest_and_cache(txn_fixture *f,
                                    jw_pakrat_txn_metadata *real_out) {
    char real[PATH_MAX];
    char floor[PATH_MAX];
    join_path(real, sizeof(real), f->root, "real.pak");
    join_path(floor, sizeof(floor), f->root, "floor.pak");
    write_real_pak(f, real);
    write_floor_pak(floor);
    char reason[JW_SVC_REASON_BUF] = {0};
    CHECK(jw_pakrat_txn_inspect_manifest(
              real, "pak.json", f->primary_userdata,
              "org.umrk.test.txn", "mac/TXN.pak", real_out,
              reason, sizeof(reason)) == 0);
    CHECK(real_out->has_service);
    CHECK(strcmp(real_out->service_id, "org.umrk.test.txn") == 0);
    CHECK(real_out->revoke_count == 1 && real_out->retained_count == 1);
    CHECK(strcmp(real_out->state_root, "Syncthing") == 0);

    jw_pakrat_txn_metadata floor_metadata;
    CHECK(jw_pakrat_txn_inspect_manifest(
              floor, "pak.json", f->primary_userdata,
              "org.umrk.test.txn", "mac/TXN.pak", &floor_metadata,
              reason, sizeof(reason)) == 0);
    CHECK(!floor_metadata.has_service && floor_metadata.service_id[0] == '\0');
    jw_pakrat_txn_metadata_destroy(&floor_metadata);

    sqlite3 *db = NULL;
    CHECK(jw_db_open(f->db, &db) == 0);
    CHECK(jw_db_apply_schema(db) == 0);
    CHECK(jw_pakrat_txn_metadata_upsert_db(db, real_out) == 0);
    jw_db_close(db);
    jw_pakrat_txn_metadata loaded;
    CHECK(jw_pakrat_txn_metadata_get(
              f->db, "org.umrk.test.txn", &loaded) == 0);
    CHECK(loaded.has_service && loaded.revoke_count == 1 &&
          loaded.retained_count == 1);
    CHECK(strcmp(loaded.revoke_on_uninstall[0], "leaf/trusted.json") == 0);
    jw_pakrat_txn_metadata_destroy(&loaded);
}

static void test_lock_death(txn_fixture *f) {
    char unused_runtime[PATH_MAX];
    join_path(unused_runtime, sizeof(unused_runtime), f->root,
              "unused-runtime");
    CHECK(!jw_pakrat_mutation_lock_is_held(
        unused_runtime, "missing", "org.umrk.test.txn", "mac/TXN.pak"));
    CHECK(!jw__pakrat_path_exists(unused_runtime));
    CHECK(!jw_pakrat_mutation_lock_is_held(
        f->runtime, NULL, "org.umrk.test.txn", "mac/TXN.pak"));
    CHECK(!jw_pakrat_mutation_lock_is_held(
        f->runtime, "missing", NULL, "mac/TXN.pak"));
    CHECK(!jw_pakrat_mutation_lock_is_held(
        f->runtime, "missing", "org.umrk.test.txn", NULL));
    int ready[2];
    CHECK(pipe(ready) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(ready[0]);
        jw_pakrat_mutation_lock lock;
        char reason[JW_SVC_REASON_BUF];
        int rc = jw_pakrat_mutation_lock_acquire(
            f->runtime, "pakrat-lock-test", "org.umrk.test.txn",
            "mac/TXN.pak", &lock, reason, sizeof(reason));
        char value = rc == 0 ? '1' : '0';
        (void)write(ready[1], &value, 1);
        pause();
        _exit(2);
    }
    close(ready[1]);
    char value = '0';
    CHECK(read(ready[0], &value, 1) == 1 && value == '1');
    close(ready[0]);
    CHECK(jw_pakrat_mutation_lock_is_held(
        f->runtime, "pakrat-lock-test", "org.umrk.test.txn",
        "mac/TXN.pak"));
    CHECK(!jw_pakrat_mutation_lock_is_held(
        f->runtime, "wrong-operation", "org.umrk.test.txn",
        "mac/TXN.pak"));
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    CHECK(!jw_pakrat_mutation_lock_is_held(
        f->runtime, "pakrat-lock-test", "org.umrk.test.txn",
        "mac/TXN.pak"));
}

static void test_inventory_and_uninstall(
    txn_fixture *f, const jw_pakrat_txn_metadata *metadata) {
    char history[PATH_MAX];
    char trust[PATH_MAX];
    char secondary_root[PATH_MAX];
    join_path(history, sizeof(history), f->primary_userdata,
              "Syncthing/history/index.db");
    join_path(trust, sizeof(trust), f->primary_userdata,
              "Syncthing/leaf/trusted.json");
    join_path(secondary_root, sizeof(secondary_root), f->secondary_userdata,
              "Syncthing");
    write_file(history, "history", 0600);
    write_file(trust, "secret", 0600);
    CHECK(symlink("missing-target", secondary_root) == 0);

    jw_pakrat_uninstall_info info;
    CHECK(jw_pakrat_txn_inventory_retained(&f->ctx, metadata, &info) == 0);
    CHECK(info.item_count == 3);
    CHECK(info.items[0].source_present && info.items[0].size_known &&
          info.items[0].size_bytes > 0);
    CHECK(info.items[1].source_present && !info.items[1].size_known);
    CHECK(!info.items[2].source_present && !info.items[2].size_known);
    jw_pakrat_uninstall_info_destroy(&info);
    unlink(secondary_root);

    char target[PATH_MAX];
    join_path(target, sizeof(target), f->primary, "Apps/mac/TXN.pak");
    write_real_pak(f, target);
    CHECK(jw_db_pakrat_upsert_install(
              f->db, "org.umrk.test.txn", "1.0.0", "mac",
              "mac/TXN.pak",
              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
              NULL, NULL) == 0);
    CHECK(jw_pakrat_txn_pending_persist(
              f->db, "primary", metadata) == 0);

    char control_path[PATH_MAX];
    join_path(control_path, sizeof(control_path), f->state,
              "services-control.db");
    sqlite3 *control = NULL;
    CHECK(sqlite3_open(control_path, &control) == SQLITE_OK);
    CHECK(sqlite3_exec(
              control,
              "CREATE TABLE service_control_state("
              "service_id TEXT PRIMARY KEY,start_with_leaf INTEGER,"
              "session_run INTEGER);"
              "INSERT INTO service_control_state VALUES("
              "'org.umrk.test.txn',1,1);",
              NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(control);

    jw_pakrat_pending_uninstall pending;
    CHECK(jw_pakrat_txn_pending_get(
              f->db, "org.umrk.test.txn", &pending) == 0);
    CHECK(jw_pakrat_txn_complete_uninstall(&f->ctx, &pending) == 0);
    jw_pakrat_pending_uninstall_destroy(&pending);
    CHECK(!jw__pakrat_path_exists(target));
    CHECK(!jw__pakrat_path_exists(trust));
    CHECK(jw__pakrat_path_exists(history));
    CHECK(scalar_path(f->db,
                      "SELECT COUNT(*) FROM pakrat_installs;") == 0);
    CHECK(scalar_path(f->db,
                      "SELECT COUNT(*) FROM pakrat_pending_uninstalls;") == 0);
    CHECK(scalar_path(control_path,
                      "SELECT COUNT(*) FROM service_control_state;") == 0);
}

static void seed_uninstall_fixture(txn_fixture *f,
                                   jw_pakrat_txn_metadata *metadata,
                                   uninstall_fixture_paths *paths,
                                   bool persist_intent) {
    memset(metadata, 0, sizeof(*metadata));
    memset(paths, 0, sizeof(*paths));
    join_path(paths->target, sizeof(paths->target), f->primary,
              "Apps/mac/TXN.pak");
    write_real_pak(f, paths->target);
    char reason[JW_SVC_REASON_BUF] = {0};
    if (jw_pakrat_txn_inspect_manifest(
            paths->target, "pak.json", f->primary_userdata,
            "org.umrk.test.txn", "mac/TXN.pak", metadata,
            reason, sizeof(reason)) != 0) {
        fprintf(stderr, "fixture manifest invalid: %s\n", reason);
        exit(2);
    }
    if (jw__pakrat_target_sibling_path(
            paths->target, metadata->store_id, "stage", paths->stage,
            sizeof(paths->stage)) != 0 ||
        jw__pakrat_target_sibling_path(
            paths->target, metadata->store_id, "rollback", paths->rollback,
            sizeof(paths->rollback)) != 0 ||
        jw__pakrat_target_sibling_path(
            paths->target, metadata->store_id, "origin", paths->origin,
            sizeof(paths->origin)) != 0) {
        fprintf(stderr, "fixture sibling path failed\n");
        exit(2);
    }
    write_floor_pak(paths->stage);
    write_floor_pak(paths->rollback);
    if (jw__pakrat_write_origin_marker(
            paths->target, metadata->store_id, metadata->install_path) != 0) {
        fprintf(stderr, "fixture origin marker failed\n");
        exit(2);
    }
    join_path(paths->history, sizeof(paths->history), f->primary_userdata,
              "Syncthing/history/index.db");
    join_path(paths->trust, sizeof(paths->trust), f->primary_userdata,
              "Syncthing/leaf/trusted.json");
    write_file(paths->history, "history", 0600);
    write_file(paths->trust, "secret", 0600);

    sqlite3 *db = NULL;
    if (jw_db_open(f->db, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        jw_pakrat_txn_metadata_upsert_db(db, metadata) != 0) {
        fprintf(stderr, "fixture metadata database failed\n");
        jw_db_close(db);
        exit(2);
    }
    jw_db_close(db);
    if (jw_db_pakrat_upsert_install(
            f->db, metadata->store_id, "1.0.0", "mac",
            metadata->install_path,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            NULL, NULL) != 0 ||
        (persist_intent &&
         jw_pakrat_txn_pending_persist(f->db, "primary", metadata) != 0)) {
        fprintf(stderr, "fixture install database failed\n");
        exit(2);
    }

    join_path(paths->control, sizeof(paths->control), f->state,
              "services-control.db");
    sqlite3 *control = NULL;
    if (sqlite3_open(paths->control, &control) != SQLITE_OK ||
        sqlite3_exec(
            control,
            "CREATE TABLE service_control_state("
            "service_id TEXT PRIMARY KEY,start_with_leaf INTEGER,"
            "session_run INTEGER);"
            "INSERT INTO service_control_state VALUES("
            "'org.umrk.test.txn',1,1);",
            NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "fixture control database failed\n");
        sqlite3_close(control);
        exit(2);
    }
    sqlite3_close(control);
}

static void expect_fault_exit(pid_t child, const char *point) {
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) {
        fprintf(stderr, "FAIL fault point %s exited status=%d\n", point,
                status);
        failures++;
    }
}

static void assert_uninstall_records(const txn_fixture *f,
                                     const uninstall_fixture_paths *paths,
                                     long long expected) {
    CHECK(scalar_path(f->db,
                      "SELECT COUNT(*) FROM pakrat_installs;") == expected);
    CHECK(scalar_path(
              f->db,
              "SELECT COUNT(*) FROM pakrat_pending_uninstalls;") == expected);
    CHECK(scalar_path(paths->control,
                      "SELECT COUNT(*) FROM service_control_state;") ==
          expected);
}

static void finish_pending_uninstall(txn_fixture *f) {
    jw_pakrat_pending_uninstall pending;
    CHECK(jw_pakrat_txn_pending_get(
              f->db, "org.umrk.test.txn", &pending) == 0);
    CHECK(jw_pakrat_txn_complete_uninstall(&f->ctx, &pending) == 0);
    jw_pakrat_pending_uninstall_destroy(&pending);
}

static void test_pending_intent_faults(void) {
    static const struct {
        const char *point;
        bool committed;
    } cases[] = {
        {"uninstall-before-intent", false},
        {"uninstall-during-intent", false},
        {"uninstall-after-intent", true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        txn_fixture f;
        jw_pakrat_txn_metadata metadata;
        uninstall_fixture_paths paths;
        fixture_setup(&f);
        seed_uninstall_fixture(&f, &metadata, &paths, false);
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            setenv("JW_PAKRAT_FAULT_AT", cases[i].point, 1);
            int rc = jw_pakrat_txn_pending_persist(
                f.db, "primary", &metadata);
            _exit(rc == 0 ? 90 : 91);
        }
        expect_fault_exit(child, cases[i].point);
        CHECK(jw__pakrat_path_exists(paths.target));
        CHECK(jw__pakrat_path_exists(paths.trust));
        CHECK(jw__pakrat_path_exists(paths.history));
        CHECK(scalar_path(
                  f.db,
                  "SELECT COUNT(*) FROM pakrat_pending_uninstalls;") ==
              (cases[i].committed ? 1 : 0));
        if (cases[i].committed) {
            finish_pending_uninstall(&f);
            CHECK(!jw__pakrat_path_exists(paths.target));
            CHECK(!jw__pakrat_path_exists(paths.trust));
            CHECK(jw__pakrat_path_exists(paths.history));
            assert_uninstall_records(&f, &paths, 0);
        } else {
            CHECK(scalar_path(
                      f.db,
                      "SELECT COUNT(*) FROM pakrat_installs;") == 1);
            CHECK(scalar_path(
                      paths.control,
                      "SELECT COUNT(*) FROM service_control_state;") == 1);
        }
        jw_pakrat_txn_metadata_destroy(&metadata);
        fixture_teardown(&f);
    }
}

static void test_pending_intent_idempotence(void) {
    txn_fixture f;
    jw_pakrat_txn_metadata metadata;
    uninstall_fixture_paths paths;
    fixture_setup(&f);
    seed_uninstall_fixture(&f, &metadata, &paths, true);
    CHECK(jw_pakrat_txn_pending_persist(f.db, "primary", &metadata) == 0);
    char original = metadata.display_name[0];
    metadata.display_name[0] = original == 'X' ? 'Y' : 'X';
    CHECK(jw_pakrat_txn_pending_persist(f.db, "primary", &metadata) == -1);
    metadata.display_name[0] = original;
    CHECK(scalar_path(
              f.db,
              "SELECT COUNT(*) FROM pakrat_pending_uninstalls;") == 1);
    finish_pending_uninstall(&f);
    assert_uninstall_records(&f, &paths, 0);
    jw_pakrat_txn_metadata_destroy(&metadata);
    fixture_teardown(&f);
}

static void test_uninstall_completion_faults(void) {
    static const struct {
        const char *point;
        bool trust_present;
        bool package_present;
        bool records_present;
    } cases[] = {
        {"uninstall-before-revoke", true, true, true},
        {"uninstall-after-revoke", false, true, true},
        {"uninstall-before-package-remove", false, true, true},
        {"uninstall-after-package-remove", false, false, true},
        {"uninstall-before-syncfs", false, false, true},
        {"uninstall-after-syncfs", false, false, true},
        {"uninstall-before-final-db", false, false, true},
        {"uninstall-during-final-db", false, false, true},
        {"uninstall-after-final-db", false, false, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        txn_fixture f;
        jw_pakrat_txn_metadata metadata;
        uninstall_fixture_paths paths;
        fixture_setup(&f);
        seed_uninstall_fixture(&f, &metadata, &paths, true);
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            setenv("JW_PAKRAT_FAULT_AT", cases[i].point, 1);
            jw_pakrat_pending_uninstall pending;
            int get_rc = jw_pakrat_txn_pending_get(
                f.db, metadata.store_id, &pending);
            int complete_rc = get_rc == 0
                                  ? jw_pakrat_txn_complete_uninstall(
                                        &f.ctx, &pending)
                                  : -1;
            if (get_rc == 0) {
                jw_pakrat_pending_uninstall_destroy(&pending);
            }
            _exit(complete_rc == 0 ? 90 : 91);
        }
        expect_fault_exit(child, cases[i].point);
        CHECK(jw__pakrat_path_exists(paths.trust) == cases[i].trust_present);
        CHECK(jw__pakrat_path_exists(paths.target) ==
              cases[i].package_present);
        CHECK(jw__pakrat_path_exists(paths.stage) ==
              cases[i].package_present);
        CHECK(jw__pakrat_path_exists(paths.rollback) ==
              cases[i].package_present);
        CHECK(jw__pakrat_path_exists(paths.origin) ==
              cases[i].package_present);
        CHECK(jw__pakrat_path_exists(paths.history));
        assert_uninstall_records(&f, &paths,
                                 cases[i].records_present ? 1 : 0);
        if (cases[i].records_present) {
            finish_pending_uninstall(&f);
        } else {
            jw_pakrat_pending_uninstall pending;
            CHECK(jw_pakrat_txn_pending_get(
                      f.db, metadata.store_id, &pending) == 1);
        }
        CHECK(!jw__pakrat_path_exists(paths.trust));
        CHECK(!jw__pakrat_path_exists(paths.target));
        CHECK(!jw__pakrat_path_exists(paths.stage));
        CHECK(!jw__pakrat_path_exists(paths.rollback));
        CHECK(!jw__pakrat_path_exists(paths.origin));
        CHECK(jw__pakrat_path_exists(paths.history));
        assert_uninstall_records(&f, &paths, 0);
        jw_pakrat_txn_metadata_destroy(&metadata);
        fixture_teardown(&f);
    }
}

int main(void) {
    txn_fixture fixture;
    fixture_setup(&fixture);
    jw_pakrat_txn_metadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    test_manifest_and_cache(&fixture, &metadata);
    test_lock_death(&fixture);
    test_inventory_and_uninstall(&fixture, &metadata);
    jw_pakrat_txn_metadata_destroy(&metadata);
    fixture_teardown(&fixture);
    test_pending_intent_faults();
    test_pending_intent_idempotence();
    test_uninstall_completion_faults();
    if (failures == 0) {
        puts("PASS pakrat-txn-test");
        return 0;
    }
    fprintf(stderr, "FAIL pakrat-txn-test (%d failures)\n", failures);
    return 1;
}
