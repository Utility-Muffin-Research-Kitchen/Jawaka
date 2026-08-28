#include "internal/catalog/manifest.h"

#include "cJSON.h"

#include <assert.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef JW_CONTENT_FIXTURES_ROOT
#define JW_CONTENT_FIXTURES_ROOT "../leaf-contracts/contracts/leaf-content/manifests"
#endif

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file) fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *text = malloc((size_t)size + 1u);
    if (!text) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(text, 1u, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(text);
        return NULL;
    }
    text[got] = '\0';
    return text;
}

static const char *json_string(const cJSON *root, const char *key,
                               const char *fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : fallback;
}

static cJSON *load_optional_json(const char *path) {
    char *text = read_file(path);
    if (!text) {
        return NULL;
    }
    cJSON *json = cJSON_Parse(text);
    free(text);
    return json;
}

static int walk(const char *kind, bool expect_valid) {
    char root[1024];
    snprintf(root, sizeof(root), "%s/%s", JW_CONTENT_FIXTURES_ROOT, kind);
    DIR *dir = opendir(root);
    assert(dir);
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char fixture[1200], pak_path[1300], expect_path[1300], context_path[1300];
        snprintf(fixture, sizeof(fixture), "%s/%s", root, entry->d_name);
        snprintf(pak_path, sizeof(pak_path), "%s/pak.json", fixture);
        snprintf(expect_path, sizeof(expect_path), "%s/expect.json", fixture);
        snprintf(context_path, sizeof(context_path), "%s/context.json", fixture);
        char *pak = read_file(pak_path);
        cJSON *expect = load_optional_json(expect_path);
        cJSON *context = load_optional_json(context_path);
        assert(pak && expect);

        const char *lane_text = json_string(context, "install_lane", "platform");
        jw_content_install_lane lane = strcmp(lane_text, "shared") == 0
                                           ? JW_CONTENT_SHARED_LANE
                                           : JW_CONTENT_PLATFORM_LANE;
        const char *source_id = json_string(context, "source_id", "primary");
        jw_content_manifest manifest;
        char reason[JW_CONTENT_REASON_MAX] = {0};
        bool valid = jw_content_manifest_validate(pak, fixture, lane, source_id,
                                                  &manifest, reason, sizeof(reason));
        if (valid != expect_valid) {
            fprintf(stderr, "FAIL %s/%s: expected valid=%d, got valid=%d reason=%s\n",
                    kind, entry->d_name, expect_valid, valid, reason);
            assert(false);
        }
        if (!expect_valid) {
            const char *wanted = json_string(expect, "reason", "");
            if (strcmp(reason, wanted) != 0) {
                fprintf(stderr, "FAIL %s/%s: expected %s, got %s\n",
                        kind, entry->d_name, wanted, reason);
                assert(false);
            }
        } else {
            const cJSON *warnings = cJSON_GetObjectItemCaseSensitive(expect, "warnings");
            bool wants_warning = cJSON_IsArray(warnings) && cJSON_GetArraySize(warnings) > 0;
            if (manifest.redundant_case_variant != wants_warning) {
                fprintf(stderr, "FAIL %s/%s: warning mismatch\n", kind, entry->d_name);
                assert(false);
            }
            jw_content_manifest_destroy(&manifest);
        }
        printf("ok: manifests/%s/%s%s%s\n", kind, entry->d_name,
               expect_valid ? "" : " -> ", expect_valid ? "" : reason);
        count++;
        cJSON_Delete(context);
        cJSON_Delete(expect);
        free(pak);
    }
    closedir(dir);
    return count;
}

int main(void) {
    struct stat st;
    if (stat(JW_CONTENT_FIXTURES_ROOT, &st) != 0) {
        fprintf(stderr, "fixture root missing: %s\n", JW_CONTENT_FIXTURES_ROOT);
        return 1;
    }
    int valid = walk("valid", true);
    int invalid = walk("invalid", false);
    printf("content-manifest-test: %d valid + %d invalid fixtures checked\n",
           valid, invalid);
    assert(valid == 8);
    assert(invalid == 49);
    puts("PASS content-manifest-test");
    return 0;
}
