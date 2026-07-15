#include "internal/retroarch/states.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char *message) {
    fprintf(stderr, "states-core-test: %s\n", message);
    exit(1);
}

static void make_file(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fail("could not create fixture file");
    }
    fputs("state", fp);
    fclose(fp);
}

static void expect_path(const char *label, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "states-core-test: %s got=%s want=%s\n",
                label, got, want);
        exit(1);
    }
}

int main(void) {
    char root[] = "/tmp/jawaka-states-core.XXXXXX";
    int fd = mkstemp(root);
    if (fd < 0) {
        fail("mkstemp failed");
    }
    close(fd);
    if (unlink(root) != 0 || mkdir(root, 0700) != 0) {
        fail("could not create fixture root");
    }

    char mgba_dir[PATH_MAX];
    char gpsp_dir[PATH_MAX];
    char flat_state[PATH_MAX];
    char mgba_state[PATH_MAX];
    char gpsp_state[PATH_MAX];
    char gpsp_other[PATH_MAX];
    snprintf(mgba_dir, sizeof(mgba_dir), "%s/mGBA", root);
    snprintf(gpsp_dir, sizeof(gpsp_dir), "%s/gpSP", root);
    snprintf(flat_state, sizeof(flat_state), "%s/smoke.state99", root);
    snprintf(mgba_state, sizeof(mgba_state), "%s/smoke.state99", mgba_dir);
    snprintf(gpsp_state, sizeof(gpsp_state), "%s/smoke.state99", gpsp_dir);
    snprintf(gpsp_other, sizeof(gpsp_other), "%s/smoke.state1", gpsp_dir);

    if (mkdir(mgba_dir, 0700) != 0 || mkdir(gpsp_dir, 0700) != 0) {
        fail("could not create core fixture directories");
    }
    make_file(flat_state);
    make_file(mgba_state);
    make_file(gpsp_other);

    int slot = 0;
    char resolved[PATH_MAX];
    if (!jw_ra_find_resume_state_for_core(root, "gpSP", "Roms/GBA/smoke.gba",
                                          JW_RA_GAME_SWITCHER_STATE_SLOT,
                                          &slot, resolved, sizeof(resolved))) {
        fail("gpSP fallback state was not found");
    }
    if (slot != 1) {
        fail("gpSP lookup crossed into a preferred slot from another namespace");
    }
    expect_path("gpSP fallback", resolved, gpsp_other);

    make_file(gpsp_state);
    if (!jw_ra_find_resume_state_for_core(root, "gpSP", "smoke.gba",
                                          JW_RA_GAME_SWITCHER_STATE_SLOT,
                                          &slot, resolved, sizeof(resolved)) ||
        slot != JW_RA_GAME_SWITCHER_STATE_SLOT) {
        fail("gpSP preferred state was not found");
    }
    expect_path("gpSP preferred", resolved, gpsp_state);

    if (!jw_ra_find_resume_state_for_core(root, "mGBA", "smoke.gba",
                                          JW_RA_GAME_SWITCHER_STATE_SLOT,
                                          &slot, resolved, sizeof(resolved))) {
        fail("mGBA state was not found after switching back");
    }
    expect_path("mGBA preferred", resolved, mgba_state);

    if (jw_ra_find_resume_state_for_core(root, "missing", "smoke.gba",
                                         JW_RA_GAME_SWITCHER_STATE_SLOT,
                                         &slot, resolved, sizeof(resolved)) ||
        jw_ra_find_resume_state_for_core(root, "../mGBA", "smoke.gba",
                                         JW_RA_GAME_SWITCHER_STATE_SLOT,
                                         &slot, resolved, sizeof(resolved)) ||
        jw_ra_find_resume_state_for_core(root, NULL, "smoke.gba",
                                         JW_RA_GAME_SWITCHER_STATE_SLOT,
                                         &slot, resolved, sizeof(resolved))) {
        fail("core-isolated lookup accepted an unsafe namespace");
    }

    unlink(gpsp_other);
    unlink(gpsp_state);
    unlink(mgba_state);
    unlink(flat_state);
    rmdir(gpsp_dir);
    rmdir(mgba_dir);
    rmdir(root);
    puts("PASS states-core-test");
    return 0;
}
