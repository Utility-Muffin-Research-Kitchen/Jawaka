#include "internal/platform/paths.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void jw__usage(FILE *stream) {
    fprintf(stream,
            "Usage: jawaka-retroarch-runner --menu|--reset-config\n"
            "\n"
            "  --menu          Launch RetroArch's native menu with shared Jawaka config\n"
            "  --reset-config  Restore shared RetroArch config from packaged defaults\n"
            "  --help          Show this help\n");
}

static int jw__path_executable(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

/* Fill the four RetroArch user joypad indices from the launch roster jawakad
   publishes in the environment: SDL_JOYSTICK_DEVICE lists exactly the roster
   event paths in player order (externals first, calibrated virtual Loong
   last). Unused players get -1. Never derive indices by scanning devices:
   the physical and virtual Loong pads share the name "Loong Gamepad", so
   name-based counting cannot distinguish them.
   Returns true when a roster was published; false = no roster (legacy
   single-player config). */
static int jw__roster_player_indices(int out[4]) {
    const char *sdl = getenv("SDL_JOYSTICK_DEVICE");
    if (!out || !sdl || !sdl[0]) {
        return 0;
    }

    int count = 1;
    for (const char *p = sdl; *p; p++) {
        if (*p == ':') {
            count++;
        }
    }
    if (count > 4) {
        count = 4;
    }
    for (int i = 0; i < 4; i++) {
        out[i] = i < count ? i : -1;
    }
    return 1;
}

static int jw__reset_config(void) {
    char *sdcard_root = jw_sdcard_root();
    char status[256];
    int rc;
    if (!sdcard_root) {
        fprintf(stderr, "could not resolve SD-card root\n");
        return 1;
    }

    rc = jw_reset_retroarch_shared_config(sdcard_root, status, sizeof(status));
    free(sdcard_root);
    if (rc != 0) {
        fprintf(stderr, "%s\n", status[0] ? status : "RetroArch config reset failed");
        return 1;
    }

    printf("%s\n", status[0] ? status : "RetroArch config reset");
    return 0;
}

static int jw__launch_menu(void) {
    char *runtime_dir = jw_runtime_dir();
    char *sdcard_root = jw_sdcard_root();
    char *retroarch = jw_retroarch_bin_path();
    char *runtime_config = NULL;
    char *state_dir = NULL;
    char error[256];
    int exit_code = 1;

    if (!runtime_dir || !sdcard_root || !retroarch) {
        fprintf(stderr, "could not resolve RetroArch launch paths\n");
        goto done;
    }
    if (!jw__path_executable(retroarch)) {
        fprintf(stderr, "RetroArch binary missing or not executable: %s\n", retroarch);
        goto done;
    }

    int player_indices[4];
    const int *player_indices_arg =
        jw__roster_player_indices(player_indices) ? player_indices : NULL;
    /* proxied_cheevos=false: this --menu runner launches no content, so there
       is nothing to proxy and neither cheevos key is ever injected here. */
    runtime_config = jw_prepare_retroarch_config(runtime_dir, sdcard_root, NULL,
                                                  player_indices_arg,
                                                  true,
                                                  false,
                                                  error, sizeof(error));
    if (!runtime_config) {
        fprintf(stderr, "could not prepare RetroArch config: %s\n",
                error[0] ? error : "unknown error");
        goto done;
    }

    state_dir = jw_retroarch_state_dir(sdcard_root);
    if (state_dir) {
        setenv("HOME", state_dir, 1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        goto done;
    }

    if (pid == 0) {
        char *const argv[] = {
            retroarch,
            "--menu",
            "--config", runtime_config,
            NULL
        };
        execv(retroarch, argv);
        perror("execv");
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        goto done;
    }

    /* No launch snapshot: this --menu runner never injects the RAOfflineProxy
       transient overrides, so there is nothing to restore. A NULL snapshot is
       ordinary backup behavior -- anything a game session already restored
       stays as it was. */
    if (jw_backup_retroarch_config(runtime_config, sdcard_root, NULL,
                                   error, sizeof(error)) != 0) {
        fprintf(stderr, "could not save RetroArch config: %s\n",
                error[0] ? error : "unknown error");
        goto done;
    }

    /* Drop the runtime config now that it is backed up; it holds the plaintext
       cheevos password. Ignore errors (the file may already be gone). */
    (void)unlink(runtime_config);

    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    } else {
        exit_code = 1;
    }

done:
    free(runtime_dir);
    free(sdcard_root);
    free(retroarch);
    free(runtime_config);
    free(state_dir);
    return exit_code;
}

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--help") == 0) {
        jw__usage(argc == 2 ? stdout : stderr);
        return argc == 2 ? 0 : 2;
    }

    if (strcmp(argv[1], "--menu") == 0) {
        return jw__launch_menu();
    }
    if (strcmp(argv[1], "--reset-config") == 0) {
        return jw__reset_config();
    }

    fprintf(stderr, "unknown option: %s\n", argv[1]);
    jw__usage(stderr);
    return 2;
}
