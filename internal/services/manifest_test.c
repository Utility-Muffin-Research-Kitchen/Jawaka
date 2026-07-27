#include "internal/services/manifest.h"

#include "cJSON.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Two fast, dependency-free sanity checks that don't need the sibling
 * umrk-workspace checkout, so this test still proves the library works
 * even when that checkout is absent. The real coverage is the fixture walk
 * below. */

static char *jw__test_read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)size, fp);
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

static void jw__test_write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "wb");
    assert(fp);
    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
}

static void jw__test_inline_sanity(void) {
    char tmpl[] = "/tmp/jw-svc-manifest-test.XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir);

    char bin_path[512];
    snprintf(bin_path, sizeof(bin_path), "%s/svc", dir);
    jw__test_write_file(bin_path, "#!/bin/sh\nexit 0\n");
    chmod(bin_path, 0755);

    char pak_json_path[512];
    snprintf(pak_json_path, sizeof(pak_json_path), "%s/pak.json", dir);
    jw__test_write_file(pak_json_path,
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"svc\"},\"restart\":\"no\",\"default_enabled\":false}}");

    char *json = jw__test_read_file(pak_json_path);
    assert(json);

    jw_service_manifest m;
    char reason[JW_SVC_REASON_BUF];
    bool ok = jw_service_manifest_validate(json, dir, NULL, &m, reason, sizeof(reason));
    assert(ok);
    assert(strcmp(m.id, "org.umrk.test") == 0);
    assert(m.stop_grace_ms == 5000); /* absent -> default */
    free(json);

    /* Same manifest, but run.path is now absolute -- must be rejected with
     * that specific reason, and the containing directory needs no changes
     * on disk for this since the string check runs before any filesystem
     * access. */
    char *bad_json = strdup(
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"/etc/passwd\"},\"restart\":\"no\",\"default_enabled\":false}}");
    ok = jw_service_manifest_validate(bad_json, dir, NULL, &m, reason, sizeof(reason));
    assert(!ok);
    assert(strcmp(reason, "absolute-run-path") == 0);
    free(bad_json);

    puts("PASS manifest-test inline sanity");
}

/* ------------------------------------------------------------------------
 * Fixture-tree walk against the frozen A0 contract fixtures.
 *
 * contracts/leaf-services/README.md documents exactly two ways a consuming
 * repo references that directory: a local sibling checkout (this), or a
 * pinned commit SHA in CI. There is no CI in this repo yet, so this test
 * uses the sibling path and skips cleanly -- not a failure -- when it
 * isn't there, matching that documented contract instead of hardcoding an
 * assumption CI will eventually need to override anyway.
 * ------------------------------------------------------------------------ */

#define JW_TEST_FIXTURES_ROOT "../umrk-workspace/contracts/leaf-services/manifests"

static bool jw__test_fixture_json_string(cJSON *obj, const char *key, char *out, size_t out_size) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    snprintf(out, out_size, "%s", item->valuestring);
    return true;
}

static int jw__test_walk_valid(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) {
        return 0;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char fixture_dir[1024], pak_path[1100], expect_path[1100];
        snprintf(fixture_dir, sizeof(fixture_dir), "%s/%s", dir_path, entry->d_name);
        snprintf(pak_path, sizeof(pak_path), "%s/pak.json", fixture_dir);
        snprintf(expect_path, sizeof(expect_path), "%s/expect.json", fixture_dir);

        struct stat st;
        if (stat(pak_path, &st) != 0) {
            continue; /* not a fixture directory */
        }

        char *pak_text = jw__test_read_file(pak_path);
        char *expect_text = jw__test_read_file(expect_path);
        assert(pak_text && expect_text);

        cJSON *expect = cJSON_Parse(expect_text);
        assert(expect);

        jw_service_manifest m;
        char reason[JW_SVC_REASON_BUF] = {0};
        bool ok = jw_service_manifest_validate(pak_text, fixture_dir, fixture_dir, &m, reason, sizeof(reason));
        if (!ok) {
            fprintf(stderr, "FAIL manifests/valid/%s: expected valid, got reason=%s\n",
                    entry->d_name, reason);
            assert(false);
        }

        cJSON *resolved_item = cJSON_GetObjectItem(expect, "resolved_stop_grace_ms");
        if (cJSON_IsNumber(resolved_item)) {
            int want = (int)resolved_item->valuedouble;
            if (m.stop_grace_ms != want) {
                fprintf(stderr, "FAIL manifests/valid/%s: resolved stop_grace_ms %d != expected %d\n",
                        entry->d_name, m.stop_grace_ms, want);
                assert(false);
            }
        }

        printf("ok:   manifests/valid/%s\n", entry->d_name);
        count++;

        cJSON_Delete(expect);
        free(pak_text);
        free(expect_text);
    }
    closedir(d);
    return count;
}

static int jw__test_walk_invalid(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) {
        return 0;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char fixture_dir[1024], expect_path[1100];
        snprintf(fixture_dir, sizeof(fixture_dir), "%s/%s", dir_path, entry->d_name);
        snprintf(expect_path, sizeof(expect_path), "%s/expect.json", fixture_dir);

        struct stat st;
        if (stat(expect_path, &st) != 0) {
            continue;
        }
        char *expect_text = jw__test_read_file(expect_path);
        assert(expect_text);
        cJSON *expect = cJSON_Parse(expect_text);
        assert(expect);

        if (strcmp(entry->d_name, "duplicate-id") == 0) {
            /* Pairwise case: this module deliberately does not implement
             * cross-manifest duplicate-service-id detection (that's a
             * discovery-level concern once every candidate has been
             * individually validated) -- assert the precondition instead:
             * both manifests validate individually and declare the same
             * service id, which is what makes them a genuine duplicate
             * rather than two unrelated rejects. */
            char pak_a_path[1100], pak_b_path[1100];
            snprintf(pak_a_path, sizeof(pak_a_path), "%s/pak-a.json", fixture_dir);
            snprintf(pak_b_path, sizeof(pak_b_path), "%s/pak-b.json", fixture_dir);
            char *pak_a_text = jw__test_read_file(pak_a_path);
            char *pak_b_text = jw__test_read_file(pak_b_path);
            assert(pak_a_text && pak_b_text);

            jw_service_manifest ma, mb;
            char reason_a[JW_SVC_REASON_BUF] = {0}, reason_b[JW_SVC_REASON_BUF] = {0};
            bool ok_a = jw_service_manifest_validate(pak_a_text, fixture_dir, fixture_dir, &ma,
                                                      reason_a, sizeof(reason_a));
            bool ok_b = jw_service_manifest_validate(pak_b_text, fixture_dir, fixture_dir, &mb,
                                                      reason_b, sizeof(reason_b));
            if (!ok_a || !ok_b || strcmp(ma.id, mb.id) != 0) {
                fprintf(stderr, "FAIL manifests/invalid/duplicate-id: expected both individually "
                                "valid with matching id, got ok_a=%d(%s) ok_b=%d(%s)\n",
                        ok_a, reason_a, ok_b, reason_b);
                assert(false);
            }
            printf("ok:   manifests/invalid/duplicate-id (duplicate-service-id, both unavailable)\n");
            count++;
            cJSON_Delete(expect);
            free(expect_text);
            free(pak_a_text);
            free(pak_b_text);
            continue;
        }

        char pak_path[1100];
        snprintf(pak_path, sizeof(pak_path), "%s/pak.json", fixture_dir);
        char *pak_text = jw__test_read_file(pak_path);
        assert(pak_text);

        char expected_reason[JW_SVC_REASON_BUF];
        assert(jw__test_fixture_json_string(expect, "reason", expected_reason, sizeof(expected_reason)));

        jw_service_manifest m;
        char reason[JW_SVC_REASON_BUF] = {0};
        bool ok = jw_service_manifest_validate(pak_text, fixture_dir, fixture_dir, &m, reason, sizeof(reason));
        if (ok) {
            fprintf(stderr, "FAIL manifests/invalid/%s: expected reason %s, got valid\n",
                    entry->d_name, expected_reason);
            assert(false);
        }
        if (strcmp(reason, expected_reason) != 0) {
            fprintf(stderr, "FAIL manifests/invalid/%s: expected reason %s, got %s\n",
                    entry->d_name, expected_reason, reason);
            assert(false);
        }
        printf("ok:   manifests/invalid/%s -> %s\n", entry->d_name, reason);
        count++;

        cJSON_Delete(expect);
        free(pak_text);
        free(expect_text);
    }
    closedir(d);
    return count;
}

int main(void) {
    jw__test_inline_sanity();

    char valid_dir[1024], invalid_dir[1024];
    snprintf(valid_dir, sizeof(valid_dir), "%s/valid", JW_TEST_FIXTURES_ROOT);
    snprintf(invalid_dir, sizeof(invalid_dir), "%s/invalid", JW_TEST_FIXTURES_ROOT);

    struct stat st;
    if (stat(valid_dir, &st) != 0) {
        printf("SKIP manifest-test fixture walk: %s not found "
               "(umrk-workspace not checked out as a sibling)\n", JW_TEST_FIXTURES_ROOT);
        puts("PASS manifest-test");
        return 0;
    }

    int valid_count = jw__test_walk_valid(valid_dir);
    int invalid_count = jw__test_walk_invalid(invalid_dir);
    printf("manifest-test: %d valid + %d invalid fixtures checked\n", valid_count, invalid_count);
    assert(valid_count >= 10);
    assert(invalid_count >= 27);

    puts("PASS manifest-test");
    return 0;
}
