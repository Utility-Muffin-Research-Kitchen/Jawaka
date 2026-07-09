#include "internal/update/update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage: jawaka-update-smoke --state-dir PATH --platform ID --manifest PATH\n");
}

int main(int argc, char **argv)
{
    const char *state_dir = NULL;
    const char *platform = NULL;
    const char *manifest = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) {
            state_dir = argv[++i];
        } else if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            platform = argv[++i];
        } else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
            manifest = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }

    if (!state_dir || !platform || !manifest) {
        usage(stderr);
        return 2;
    }

    jw_update_status status;
    jw_update_status_init(&status, platform, state_dir);
    int rc = jw_update_check_local_manifest(&status, state_dir, platform, manifest);
    cJSON *json = jw_update_status_to_json(&status);
    char *printed = cJSON_Print(json);
    cJSON_Delete(json);
    if (!printed) {
        return 1;
    }
    puts(printed);
    free(printed);
    return rc == 0 ? 0 : 1;
}
