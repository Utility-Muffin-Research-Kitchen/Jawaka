#include "internal/retroarch/catalog.h"

#include <stdio.h>

#define JW_CATALOG_SMOKE_MAX_CHOICES 32

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr,
                "usage: %s <sdcard-root> <system-id> <cores-dir> <platform-dir>\n",
                argv[0]);
        return 2;
    }

    char error[256];
    jw_ra_catalog *catalog = jw_ra_catalog_load(argv[1], error, sizeof(error));
    if (!catalog) {
        fprintf(stderr, "catalog load failed: %s\n",
                error[0] ? error : "unknown");
        return 1;
    }

    jw_ra_core_choice choices[JW_CATALOG_SMOKE_MAX_CHOICES];
    size_t count = 0;
    if (jw_ra_catalog_list_system_cores(catalog, argv[2], argv[3], argv[4],
                                        choices, JW_CATALOG_SMOKE_MAX_CHOICES,
                                        &count) != 0) {
        fprintf(stderr, "core choice listing failed for %s\n", argv[2]);
        jw_ra_catalog_free(catalog);
        return 1;
    }

    printf("count\t%zu\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("choice\t%zu\t%s\t%s\t%s\t%s\t%s\n",
               i,
               choices[i].id,
               choices[i].type,
               choices[i].is_default ? "default" : "alternate",
               choices[i].display_name,
               choices[i].type[0] && choices[i].type[0] == 'p'
                   ? choices[i].path
                   : choices[i].file_name);
    }

    jw_ra_catalog_free(catalog);
    return 0;
}
