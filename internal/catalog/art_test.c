#include "internal/catalog/manifest.h"
#include "internal/catalog/merge.h"
#include "internal/retroarch/catalog.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef JW_ART_FIXTURE
#define JW_ART_FIXTURE "../leaf-contracts/contracts/leaf-content/art/fixtures.json"
#endif

static const cJSON *get(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
static const char *str(const cJSON *o, const char *key) {
    const cJSON *v = get(o, key); return cJSON_IsString(v) ? v->valuestring : "";
}
static void write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(data, 1, strlen(data), f) == strlen(data)); assert(!fclose(f));
}
static cJSON *read_json(const char *path) {
    FILE *f = fopen(path, "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long n = ftell(f); assert(n > 0); rewind(f);
    char *s = calloc((size_t)n + 1, 1); assert(s);
    assert(fread(s, 1, (size_t)n, f) == (size_t)n); fclose(f);
    cJSON *o = cJSON_Parse(s); free(s); assert(o); return o;
}
static void check(int yes, const char *name, const char *detail) {
    if (!yes) { fprintf(stderr, "content-art: %s: %s\n", name, detail); abort(); }
}
static cJSON *art_diagnostic(const char *provider, const char *reason, const char *detail) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "provider", provider);
    cJSON_AddStringToObject(d, "reason", reason);
    cJSON_AddStringToObject(d, "detail", detail);
    return d;
}

int main(void) {
    char temp[] = "/tmp/jw-content-art-XXXXXX"; assert(mkdtemp(temp));
    char pak_dir[512], path[1024]; snprintf(pak_dir, sizeof(pak_dir), "%s/pak", temp);
    assert(!mkdir(pak_dir, 0700));
    snprintf(path, sizeof(path), "%s/art", pak_dir); assert(!mkdir(path, 0700));
    snprintf(path, sizeof(path), "%s/art/mark.png", pak_dir); write_file(path, "\x89PNG\r\n\x1a\n");
    snprintf(path, sizeof(path), "%s/art/bad.png", pak_dir); write_file(path, "not PNG");
    snprintf(path, sizeof(path), "%s/run.sh", pak_dir); write_file(path, "#!/bin/sh\n"); assert(!chmod(path, 0755));
    snprintf(path, sizeof(path), "%s/escape", pak_dir); assert(!symlink(temp, path));
    cJSON *fixtures = read_json(JW_ART_FIXTURE);
    const cJSON *paks = get(fixtures, "paks");
    const cJSON *test = NULL;
    int validations = 0, merges = 0;
    cJSON_ArrayForEach(test, get(fixtures, "cases")) {
        cJSON *pak = cJSON_Duplicate(get(paks, "owner"), true);
        cJSON_DeleteItemFromObjectCaseSensitive(pak, "content_art");
        if (get(test, "content_art")) cJSON_AddItemToObject(pak, "content_art", cJSON_Duplicate(get(test, "content_art"), true));
        const cJSON *block = NULL;
        char reason[64];
        bool unreadable = !strcmp(str(test, "setup"), "unreadable");
        snprintf(path, sizeof(path), "%s/art/mark.png", pak_dir);
        if (unreadable) assert(!chmod(path, 0000));
        int result = jw_content_art_validate(pak, pak_dir, &block, reason, sizeof(reason));
        if (unreadable) assert(!chmod(path, 0600));
        bool valid = cJSON_IsTrue(get(test, "valid"));
        if (!unreadable || geteuid() != 0)
            check(valid ? result >= 0 : result == -1 && !strcmp(reason, str(test, "reason")), str(test, "name"), reason);
        char *raw = cJSON_PrintUnformatted(pak); jw_content_manifest manifest;
        check(jw_content_manifest_validate(raw, pak_dir, JW_CONTENT_PLATFORM_LANE, "primary", &manifest, reason, sizeof(reason)), str(test, "name"), "CONTENT-1 refused");
        jw_content_manifest_destroy(&manifest); free(raw); cJSON_Delete(pak); validations++;
    }
    /* A NUL inside optional metadata must not hide a core, while a NUL in
       provides still fails closed. Exercise the raw-text parser boundary. */
    cJSON *pak = cJSON_Duplicate(get(paks, "owner"), true);
    cJSON_DeleteItemFromObjectCaseSensitive(pak, "content_art");
    char *plain = cJSON_PrintUnformatted(pak); size_t n = strlen(plain);
    char raw[8192]; snprintf(raw, sizeof(raw), "%.*s,\"content_art\":{\"bad\":\"x\\u0000y\"}}", (int)n - 1, plain);
    jw_content_manifest manifest; char reason[64]; const cJSON *block;
    assert(jw_content_manifest_validate(raw, pak_dir, JW_CONTENT_PLATFORM_LANE, "primary", &manifest, reason, sizeof(reason)));
    assert(jw_content_art_validate(manifest.document, pak_dir, &block, reason, sizeof(reason)) == -1);
    jw_content_manifest_destroy(&manifest);
    snprintf(raw, sizeof(raw), "%.*s,\"other\":\"x\\u0000y\"}", (int)n - 1, plain);
    assert(!jw_content_manifest_validate(raw, pak_dir, JW_CONTENT_PLATFORM_LANE, "primary", &manifest, reason, sizeof(reason)));
    free(plain); cJSON_Delete(pak);

    cJSON_ArrayForEach(test, get(fixtures, "generation_cases")) {
        cJSON *contributors = cJSON_CreateArray();
        const cJSON *name = NULL;
        cJSON *art_errors = cJSON_CreateArray();
        cJSON_ArrayForEach(name, get(test, "contributors")) {
            const cJSON *pak = get(paks, name->valuestring);
            cJSON *row = cJSON_CreateObject(); char provider[128];
            snprintf(provider, sizeof(provider), "mlp1/%s.pak", name->valuestring);
            cJSON_AddStringToObject(row, "provider", provider);
            cJSON_AddItemToObject(row, "provides", cJSON_Duplicate(get(pak, "provides"), true));
            const cJSON *art = NULL;
            if (jw_content_art_validate(pak, pak_dir, &art, reason, sizeof(reason)) > 0)
                cJSON_AddItemToObject(row, "content_art", cJSON_Duplicate(art, true));
            else if (reason[0]) cJSON_AddItemToArray(art_errors, art_diagnostic(provider, reason, "content_art ignored"));
            cJSON_AddItemToArray(contributors, row);
        }
        cJSON *merged = NULL, *diagnostics = NULL;
        assert(!jw_catalog_merge(get(fixtures, "base"), contributors, &merged, &diagnostics));
        cJSON_Delete(diagnostics); diagnostics = art_errors;
        cJSON *before = cJSON_Duplicate(merged, true);
        assert(!jw_catalog_apply_content_art(cJSON_GetObjectItemCaseSensitive(merged, "systems"), get(merged, "cores"), contributors, diagnostics));
        const cJSON *system = NULL;
        cJSON_ArrayForEach(system, get(merged, "systems")) {
            const char *owner = str(get(test, "applied"), str(system, "id"));
            char provider[128]; snprintf(provider, sizeof(provider), "mlp1/%s.pak", owner);
            check(owner[0] ? !strcmp(str(system, "wordmark_provider"), provider) && !strcmp(str(system, "wordmark"), "art/mark.png") : !get(system, "wordmark"), str(test, "name"), "wrong artwork");
        }
        cJSON *stripped = cJSON_Duplicate(merged, true); cJSON *row = NULL;
        cJSON_ArrayForEach(row, get(stripped, "systems")) {
            cJSON_DeleteItemFromObjectCaseSensitive(row, "wordmark");
            cJSON_DeleteItemFromObjectCaseSensitive(row, "wordmark_provider");
        }
        check(cJSON_Compare(before, stripped, true), str(test, "name"), "ordinary merge changed");
        const cJSON *expected = get(test, "diagnostics");
        check(cJSON_GetArraySize(expected) == cJSON_GetArraySize(diagnostics), str(test, "name"), "diagnostic count");
        const cJSON *want = NULL;
        cJSON_ArrayForEach(want, expected) {
            char provider[128]; snprintf(provider, sizeof(provider), "mlp1/%s.pak", cJSON_GetArrayItem(want, 0)->valuestring);
            cJSON *needle = art_diagnostic(provider, cJSON_GetArrayItem(want, 1)->valuestring, cJSON_GetArrayItem(want, 2)->valuestring);
            bool found = false; const cJSON *d = NULL;
            cJSON_ArrayForEach(d, diagnostics) if (cJSON_Compare(d, needle, true)) found = true;
            check(found, str(test, "name"), "diagnostic mismatch"); cJSON_Delete(needle);
        }
        cJSON_Delete(stripped); cJSON_Delete(before); cJSON_Delete(merged); cJSON_Delete(diagnostics); cJSON_Delete(contributors); merges++;
    }
    cJSON_Delete(fixtures);
    snprintf(path, sizeof(path), "%s/escape", pak_dir); unlink(path);
    snprintf(path, sizeof(path), "%s/run.sh", pak_dir); unlink(path);
    snprintf(path, sizeof(path), "%s/art/mark.png", pak_dir); unlink(path);
    snprintf(path, sizeof(path), "%s/art/bad.png", pak_dir); unlink(path);
    snprintf(path, sizeof(path), "%s/art", pak_dir); rmdir(path); rmdir(pak_dir); rmdir(temp);
    printf("PASS content-art-test: %d validation, %d merge fixtures\n", validations, merges);
}
