#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#define FIXTURE_LEASE_FD 3

static volatile sig_atomic_t g_term_requested = 0;

static void fixture_on_term(int signo) {
    (void)signo;
    g_term_requested = 1;
}

static int fixture_install_term_handler(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = fixture_on_term;
    if (sigemptyset(&action.sa_mask) != 0) {
        return -1;
    }
    return sigaction(SIGTERM, &action, NULL);
}

static void fixture_delay_ms(int delay_ms) {
    struct timespec remaining = {
        .tv_sec = delay_ms / 1000,
        .tv_nsec = (long)(delay_ms % 1000) * 1000000L,
    };
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
}

static void fixture_wait_forever(void) {
    for (;;) {
        pause();
    }
}

static int fixture_path(char *out, size_t out_size, const char *leaf) {
    const char *runtime = getenv("UMRK_SERVICE_RUNTIME_DIR");
    if (!runtime || !runtime[0] || !leaf || strchr(leaf, '/') != NULL) {
        return -1;
    }
    int written = snprintf(out, out_size, "%s/%s", runtime, leaf);
    return written >= 0 && (size_t)written < out_size ? 0 : -1;
}

static int fixture_touch(const char *leaf) {
    char path[4096];
    if (fixture_path(path, sizeof(path), leaf) != 0) {
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }
    int rc = dprintf(fd, "%ld\n", (long)getpid()) < 0 ? -1 : 0;
    if (close(fd) != 0) {
        rc = -1;
    }
    return rc;
}

static int fixture_append_generation(void) {
    char path[4096];
    if (fixture_path(path, sizeof(path), "generations.log") != 0) {
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        return -1;
    }
    int rc = dprintf(fd, "leader=%ld\n", (long)getpid()) < 0 ? -1 : 0;
    if (close(fd) != 0) {
        rc = -1;
    }
    return rc;
}

static int fixture_wait_for_file(const char *leaf, int timeout_ms) {
    char path[4096];
    if (fixture_path(path, sizeof(path), leaf) != 0) {
        return -1;
    }
    for (int waited = 0; waited <= timeout_ms; waited += 10) {
        if (access(path, F_OK) == 0) {
            return 0;
        }
        fixture_delay_ms(10);
    }
    return -1;
}

static void fixture_wait_for_parent_death(pid_t original_parent) {
    while (!g_term_requested && getppid() == original_parent) {
        fixture_delay_ms(10);
    }
}

static int fixture_normal_exit(void) {
    return 0;
}

static int fixture_ignore_term(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        return 70;
    }
    fixture_wait_forever();
    return 0;
}

static int fixture_orphan_descendant(void) {
    pid_t child = fork();
    if (child < 0) {
        return 70;
    }
    if (child == 0) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_IGN;
        if (sigemptyset(&action.sa_mask) != 0 ||
            sigaction(SIGTERM, &action, NULL) != 0) {
            _exit(70);
        }
        fixture_wait_forever();
        _exit(0);
    }
    fixture_delay_ms(50);
    return 0;
}

static int fixture_daemonizes(void) {
    pid_t child = fork();
    if (child < 0) {
        return 70;
    }
    if (child == 0) {
        if (fixture_install_term_handler() != 0) {
            _exit(70);
        }
        while (!g_term_requested) {
            pause();
        }
        _exit(0);
    }
    fixture_delay_ms(50);
    return 0;
}

static int fixture_supervisor_death(void) {
    if (fixture_install_term_handler() != 0 ||
        fixture_append_generation() != 0) {
        return 70;
    }

    pid_t child = fork();
    if (child < 0) {
        return 70;
    }
    if (child == 0) {
        pid_t parent = getppid();
        /* Signal dispositions are inherited across fork(), but establish the
         * cooperative SIGTERM path explicitly in the descendant as well so
         * PDEATHSIG cannot silently become an immediate default-action exit
         * if the setup above is ever moved or narrowed. */
        if (fixture_install_term_handler() != 0) {
            _exit(70);
        }
#if defined(__linux__)
        if (prctl(PR_SET_PDEATHSIG, (unsigned long)SIGTERM, 0L, 0L, 0L) !=
            0 || getppid() != parent) {
            _exit(70);
        }
#endif
        if (fixture_touch("descendant-ready") != 0) {
            _exit(70);
        }
        fixture_wait_for_parent_death(parent);
        fixture_delay_ms(600);
        _exit(0);
    }

    if (fixture_wait_for_file("descendant-ready", 2000) != 0 ||
        fixture_touch("ready") != 0) {
        kill(child, SIGKILL);
        return 70;
    }
    pid_t parent = getppid();
    fixture_wait_for_parent_death(parent);
    fixture_delay_ms(600);
    return 0;
}

int main(int argc, char **argv) {
    const char *lease_text = getenv("UMRK_SERVICE_LEASE_FD");
    if (argc != 2 || !lease_text || strcmp(lease_text, "3") != 0 ||
        fcntl(FIXTURE_LEASE_FD, F_GETFD) < 0) {
        return 64;
    }
    if (strcmp(argv[1], "normal-exit") == 0) {
        return fixture_normal_exit();
    }
    if (strcmp(argv[1], "ignore-term") == 0) {
        return fixture_ignore_term();
    }
    if (strcmp(argv[1], "orphan-descendant") == 0) {
        return fixture_orphan_descendant();
    }
    if (strcmp(argv[1], "daemonizes") == 0) {
        return fixture_daemonizes();
    }
    if (strcmp(argv[1], "supervisor-death") == 0) {
        return fixture_supervisor_death();
    }
    return 64;
}
