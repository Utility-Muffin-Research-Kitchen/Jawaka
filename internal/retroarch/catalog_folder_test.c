#define _POSIX_C_SOURCE 200809L

#include "internal/retroarch/catalog.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int fail(const char *message) {
    fprintf(stderr, "catalog-folder-test: %s\n", message);
    return 1;
}

static int write_text(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t len = strlen(text);
    return fwrite(text, 1, len, fp) == len && fclose(fp) == 0 ? 0 : -1;
}

static int write_cores(const char *path, const char *rows) {
    char json[4096];
    int n = snprintf(json, sizeof(json),
                     "{\"platform\":\"mac\",\"cores\":[%s]}", rows);
    return n > 0 && n < (int)sizeof(json) ? write_text(path, json) : -1;
}

int main(void) {
    const char *safe[] = { "mGBA", "DOSBox-pure", "Game & Watch",
                           "MAME 2010", "PCSX-ReARMed" };
    for (size_t i = 0; i < sizeof(safe) / sizeof(safe[0]); i++) {
        if (!jw_ra_core_folder_is_safe(safe[i])) return fail("safe folder rejected");
    }
    const char *unsafe[] = { "", ".", "..", "../mGBA", "bad:name", "tail. ",
                             "CON", "con.txt", "COM1", "lpt9.states" };
    for (size_t i = 0; i < sizeof(unsafe) / sizeof(unsafe[0]); i++) {
        if (jw_ra_core_folder_is_safe(unsafe[i])) return fail("unsafe folder accepted");
    }
    char too_long[257];
    memset(too_long, 'a', sizeof(too_long) - 1u);
    too_long[sizeof(too_long) - 1u] = '\0';
    if (jw_ra_core_folder_is_safe(too_long)) return fail("overlong folder accepted");

    char root[] = "/tmp/jw-catalog-folder-XXXXXX";
    int fd = mkstemp(root);
    if (fd < 0) return fail("mkstemp failed");
    close(fd);
    if (unlink(root) != 0 || mkdir(root, 0700) != 0) return fail("root mkdir failed");
    char defaults[PATH_MAX];
    snprintf(defaults, sizeof(defaults), "%s/defaults", root);
    if (mkdir(defaults, 0755) != 0) return fail("defaults mkdir failed");
    setenv("UMRK_PLATFORM_PATH", root, 1);

    char cores[PATH_MAX], systems[PATH_MAX];
    snprintf(cores, sizeof(cores), "%s/cores.json", defaults);
    snprintf(systems, sizeof(systems), "%s/systems.json", defaults);
    if (write_text(systems,
                   "{\"platform\":\"mac\",\"systems\":["
                   "{\"id\":\"GBA\",\"default_core\":\"mgba\","
                   "\"legacy_flat_core\":\"mgba\"}]}") != 0) {
        return fail("systems fixture failed");
    }

    const char *safe_row =
        "{\"id\":\"mgba\",\"type\":\"retroarch\","
        "\"status\":\"packaged\",\"file_name\":\"mgba_libretro.so\","
        "\"config_folder\":\"Game & Watch\"}";
    if (write_cores(cores, safe_row) != 0) return fail("safe fixture failed");
    char error[256];
    jw_ra_catalog *catalog = jw_ra_catalog_load(root, error, sizeof(error));
    if (!catalog || !catalog->systems[0].legacy_flat_core ||
        strcmp(catalog->systems[0].legacy_flat_core, "mgba") != 0) {
        return fail(error[0] ? error : "safe catalog rejected");
    }
    jw_ra_catalog_free(catalog);

    const char *missing_row =
        "{\"id\":\"mgba\",\"type\":\"retroarch\","
        "\"status\":\"packaged\",\"file_name\":\"mgba_libretro.so\"}";
    if (write_cores(cores, missing_row) != 0 ||
        (catalog = jw_ra_catalog_load(root, error, sizeof(error))) != NULL) {
        jw_ra_catalog_free(catalog);
        return fail("missing folder catalog accepted");
    }

    const char *collision_rows =
        "{\"id\":\"a\",\"type\":\"retroarch\",\"status\":\"packaged\","
        "\"file_name\":\"a.so\",\"config_folder\":\"mGBA\"},"
        "{\"id\":\"b\",\"type\":\"retroarch\",\"status\":\"packaged\","
        "\"file_name\":\"b.so\",\"config_folder\":\"mgba\"}";
    if (write_cores(cores, collision_rows) != 0 ||
        (catalog = jw_ra_catalog_load(root, error, sizeof(error))) != NULL) {
        jw_ra_catalog_free(catalog);
        return fail("case-fold collision accepted");
    }

    printf("catalog-folder-test: ok\n");
    return 0;
}
