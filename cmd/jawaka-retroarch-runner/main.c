#include "internal/platform/paths.h"

#include <errno.h>
#ifdef __linux__
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#endif
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

static int jw__parse_joypad_index(void) {
    const char *env = getenv("JAWAKA_RETROARCH_JOYPAD_INDEX");
    char *end = NULL;
    long value;
    if (!env || !env[0]) {
        return -1;
    }
    errno = 0;
    value = strtol(env, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value < 0 || value > 99) {
        return -1;
    }
    return (int)value;
}

#ifdef __linux__
static int jw__event_name_matches(int fd, const char *expected) {
    if (!expected || !expected[0]) {
        return 0;
    }

    char name[128];
    memset(name, 0, sizeof(name));
    return ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
           strcmp(name, expected) == 0;
}

static int jw__same_rdev(const char *a, const char *b) {
    struct stat sa;
    struct stat sb;
    return a && b && stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_rdev == sb.st_rdev;
}

static int jw__same_event_path(const char *a, const char *b) {
    return a && b && a[0] && b[0] &&
           (strcmp(a, b) == 0 || jw__same_rdev(a, b));
}

static int jw__joypad_index_from_virtual_event(void) {
    const char *virtual_event = getenv("JAWAKA_RETROARCH_VIRTUAL_EVENT");
    const char *device_name = getenv("JAWAKA_RETROARCH_INPUT_DEVICE");
    int joypad_index = 0;

    if (!virtual_event || !virtual_event[0]) {
        return -1;
    }
    if (!device_name || !device_name[0]) {
        return -1;
    }

    for (int i = 0; i < 64; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);

        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        int match = jw__event_name_matches(fd, device_name);
        close(fd);
        if (!match) {
            continue;
        }

        if (jw__same_event_path(path, virtual_event)) {
            return joypad_index;
        }
        joypad_index++;
    }

    return -1;
}
#endif

static int jw__resolve_joypad_index(void) {
#ifdef __linux__
    int index = jw__joypad_index_from_virtual_event();
    if (index >= 0) {
        return index;
    }
#endif
    return jw__parse_joypad_index();
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

    runtime_config = jw_prepare_retroarch_config(runtime_dir, sdcard_root, NULL,
                                                 jw__resolve_joypad_index(),
                                                 true,
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

    if (jw_backup_retroarch_config(runtime_config, sdcard_root, error, sizeof(error)) != 0) {
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
