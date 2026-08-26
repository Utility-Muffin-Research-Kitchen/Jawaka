#define _POSIX_C_SOURCE 200809L

#include "internal/platform/paths.h"
#include "internal/retroarch/command.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Distinct from any RetroArch exit code: the app ran, but its settings could
   not be written back to the durable shared config. jawakad turns this into a
   visible error instead of a silent loss. */
#define JW_RUNNER_EXIT_SAVE_FAILED 90

/* How long RetroArch may take to answer QUIT, write its config, and exit
   before the runner stops waiting and escalates.
   PROVISIONAL: sized to comfortably cover the observed MLP1 clean-quit path
   (UDP QUIT, save-on-exit write, FAT32 flush) with margin, pending the p95
   device measurement the #48 plan calls for. jawakad allows slightly more
   than this so the runner, not the daemon, is the one that gives up first. */
#define JW_RUNNER_QUIT_GRACE_MS 4000

/* After the grace expires RetroArch is not going to save. These only bound how
   long the forced teardown itself may take. */
#define JW_RUNNER_TERM_GRACE_MS 1000
#define JW_RUNNER_KILL_GRACE_MS 1000

/* Set from a signal handler and read nowhere else. The handler does nothing
   but this store: the quit request, the wait, the copy-back, and every log
   line happen on the normal path once the interrupted waitpid() returns. */
static volatile sig_atomic_t g_stop_requested = 0;

static void jw__on_stop_signal(int signum) {
    (void)signum;
    g_stop_requested = 1;
}

/* No SA_RESTART: the point of these handlers is to break waitpid() out of its
   indefinite block so the save path can run. */
static void jw__install_stop_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = jw__on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGHUP, &sa, NULL);
}

static long long jw__now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000ll + ts.tv_nsec / 1000000ll;
}

static void jw__sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000l;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        /* A stop signal during the poll interval is exactly what we want to
           notice; loop on the remaining time. */
    }
}

/* Wait for `pid`, giving up at `deadline_ms` (0 = wait indefinitely).
   Returns 1 when the child was reaped, 0 on timeout, -1 on a wait error or on
   a stop signal (the caller distinguishes the two by g_stop_requested).

   A stop signal MUST break the caller out of here rather than being retried:
   restarting the wait is what leaves the save path unreachable and turns a
   Leaf poweroff into a lost session. */
static int jw__wait_child(pid_t pid, int *status, long long deadline_ms) {
    for (;;) {
        int flags = deadline_ms > 0 ? WNOHANG : 0;
        pid_t r = waitpid(pid, status, flags);
        if (r == pid) {
            return 1;
        }
        if (r < 0) {
            if (errno == EINTR && !g_stop_requested) {
                continue;
            }
            return -1;
        }
        /* r == 0: still running, and we are on the bounded path. */
        if (jw__now_ms() >= deadline_ms) {
            return 0;
        }
        jw__sleep_ms(25);
    }
}

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
    bool child_reaped = false;
    bool quit_requested = false;
    bool clean_exit = false;

    /* Phase 1: RetroArch owns the session. Block here until it exits on its
       own, or until a stop signal breaks the wait. */
    int waited = jw__wait_child(pid, &status, 0);
    if (waited == 1) {
        child_reaped = true;
        clean_exit = true;
    } else if (waited < 0 && !g_stop_requested) {
        perror("waitpid");
        goto done;
    }

    if (!child_reaped) {
        /* Phase 2: something asked this runner to stop -- a Leaf poweroff or
           reboot, a daemon shutdown, or the launcher switcher. RetroArch is
           still up and holding the only copy of this session's settings.

           Promote whatever is already on disk FIRST. If the user used Save
           Current Configuration at any point, the working file already holds
           those changes, and this preserves them even if the clean quit below
           never completes. Failure here is not fatal: the durable config is
           left exactly as it was and the real attempt follows. */
        quit_requested = true;
        if (jw_backup_retroarch_config(runtime_config, sdcard_root, NULL,
                                       error, sizeof(error)) != 0) {
            fprintf(stderr, "pre-quit RetroArch config promotion skipped: %s\n",
                    error[0] ? error : "unknown error");
        }

        /* SIGTERM is not a clean quit for this build: MLP1 runs the sdl_gl
           context, which never consults RetroArch's Unix signal flag, so the
           process would die with its config still in memory. The protected
           network command port is the mechanism that actually makes RetroArch
           save and exit. */
        jw_ra_client client = jw_ra_client_default();
        jw_ra_result quit = jw_ra_quit(&client);
        if (quit != JW_RA_OK) {
            fprintf(stderr, "RetroArch quit command failed: %s\n",
                    jw_ra_result_string(quit));
        }

        waited = jw__wait_child(pid, &status, jw__now_ms() + JW_RUNNER_QUIT_GRACE_MS);
        if (waited == 1) {
            child_reaped = true;
            clean_exit = true;
        }
    }

    if (!child_reaped) {
        /* The grace is spent: RetroArch is not going to save this session.
           Everything from here only bounds the teardown so a poweroff cannot
           hang on a wedged emulator. Signal the child directly rather than the
           process group -- jawakad made this runner the group leader, so a
           group signal would take the runner down with it and orphan nothing
           useful. */
        fprintf(stderr, "RetroArch did not exit within %d ms of QUIT; forcing\n",
                JW_RUNNER_QUIT_GRACE_MS);
        kill(pid, SIGTERM);
        if (jw__wait_child(pid, &status, jw__now_ms() + JW_RUNNER_TERM_GRACE_MS) == 1) {
            child_reaped = true;
        } else {
            kill(pid, SIGKILL);
            if (jw__wait_child(pid, &status, jw__now_ms() + JW_RUNNER_KILL_GRACE_MS) == 1) {
                child_reaped = true;
            }
        }
    }

    /* One last promotion from the last complete copy on disk. After a forced
       teardown this is whatever RetroArch had already written; after a clean
       quit it is the config it just saved.

       No launch snapshot: this --menu runner never injects the RAOfflineProxy
       transient overrides, so there is nothing to restore. A NULL snapshot is
       ordinary backup behavior -- anything a game session already restored
       stays as it was. */
    bool saved = jw_backup_retroarch_config(runtime_config, sdcard_root, NULL,
                                            error, sizeof(error)) == 0;
    if (!saved) {
        fprintf(stderr, "could not save RetroArch config: %s\n",
                error[0] ? error : "unknown error");
    }

    /* Drop the runtime config once RetroArch is confirmed dead. It holds the
       plaintext cheevos password, MLP1 apps run as root, and mode 0600 is
       therefore not an isolation boundary -- a later app session could read
       it. Keep it ONLY while the process that owns it is still alive; a
       failed save leaves a filtered, secret-free recovery candidate next to
       the durable config instead. Ignore errors (it may already be gone). */
    if (child_reaped) {
        (void)unlink(runtime_config);
    } else {
        fprintf(stderr,
                "RetroArch pid=%d could not be confirmed dead; leaving %s in place\n",
                (int)pid, runtime_config);
    }

    if (!saved) {
        exit_code = JW_RUNNER_EXIT_SAVE_FAILED;
    } else if (quit_requested && !clean_exit) {
        /* The app was stopped from outside and RetroArch never answered QUIT.
           Whatever was on disk was promoted, but this session's last in-memory
           changes are gone; say so rather than reporting a clean exit. */
        exit_code = JW_RUNNER_EXIT_SAVE_FAILED;
    } else if (WIFEXITED(status)) {
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
        jw__install_stop_handlers();
        return jw__launch_menu();
    }
    if (strcmp(argv[1], "--reset-config") == 0) {
        return jw__reset_config();
    }

    fprintf(stderr, "unknown option: %s\n", argv[1]);
    jw__usage(stderr);
    return 2;
}
