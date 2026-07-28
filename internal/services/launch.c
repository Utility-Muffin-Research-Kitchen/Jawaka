#define _GNU_SOURCE

#include "internal/services/launch.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include "cJSON.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* jw__log_write_fail / jw__child_fail use async-signal-safe operations
 * only: the child between fork() and exec() may call nothing that takes
 * a lock or allocates (no stdio, no malloc), because the parent may have
 * held one of those locks in another thread at the instant of fork. All
 * child-side failure reporting is a single write() into the CLOEXEC
 * error pipe or the inherited log fd. */

static void jw__set_slug(char *reason, size_t reason_size, const char *slug) {
    if (reason && reason_size > 0) {
        snprintf(reason, reason_size, "%s", slug);
    }
}

/* Builds logs_dir/services/<service_id>/ like the lease tree: each level
 * must end up a real directory (not a symlink), owner-owned, with no
 * group/other permission bits. Returns 0 on success, -1 with `reason`
 * set on any failure. */
static int jw__ensure_service_log_dir(const char *logs_dir,
                                      const char *service_id,
                                      char *reason, size_t reason_size) {
    if (!service_id || service_id[0] == '\0' ||
        strchr(service_id, '/') != NULL || strcmp(service_id, ".") == 0 ||
        strcmp(service_id, "..") == 0) {
        jw__set_slug(reason, reason_size, "invalid-service-id");
        return -1;
    }
    if (!logs_dir || logs_dir[0] == '\0') {
        jw__set_slug(reason, reason_size, "logs-dir-unavailable");
        return -1;
    }

    char services[PATH_MAX];
    char dir[PATH_MAX];
    if (snprintf(services, sizeof(services), "%s/services", logs_dir) >=
            (int)sizeof(services) ||
        snprintf(dir, sizeof(dir), "%s/%s", services, service_id) >=
            (int)sizeof(dir)) {
        jw__set_slug(reason, reason_size, "path-too-long");
        return -1;
    }

    const char *levels[2] = {services, dir};
    for (int i = 0; i < 2; i++) {
        struct stat st;
        if (lstat(levels[i], &st) != 0) {
            if (errno != ENOENT) {
                jw__set_slug(reason, reason_size, "mkdir-failed");
                return -1;
            }
            if (mkdir(levels[i], 0700) != 0) {
                jw__set_slug(reason, reason_size, "mkdir-failed");
                return -1;
            }
            continue;
        }
        /* Exists: must be a real directory we own, with no group/other
         * bits -- never a symlink redirecting the log tree elsewhere. */
        if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            jw__set_slug(reason, reason_size, "mkdir-failed");
            return -1;
        }
    }
    return 0;
}

/* Renames <id>.log -> .log.1 -> ... -> .log.4, dropping .log.4. Best
 * effort per generation: a missing intermediate file is fine (ENOTENT),
 * anything else fails the rotation. */
static int jw__log_rotate(const char *base, char *reason, size_t reason_size) {
    char from[PATH_MAX];
    char to[PATH_MAX];

    /* Drop the oldest generation outright. */
    if (snprintf(to, sizeof(to), "%s.%d", base, JW_SVC_LOG_MAX_FILES - 1) >=
        (int)sizeof(to)) {
        jw__set_slug(reason, reason_size, "path-too-long");
        return -1;
    }
    if (unlink(to) != 0 && errno != ENOENT) {
        jw__set_slug(reason, reason_size, "rotate-failed");
        return -1;
    }

    for (int i = JW_SVC_LOG_MAX_FILES - 1; i >= 1; i--) {
        if (snprintf(from, sizeof(from), "%s.%d", base, i - 1) >=
                (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s.%d", base, i) >= (int)sizeof(to)) {
            jw__set_slug(reason, reason_size, "path-too-long");
            return -1;
        }
        if (i == 1) {
            /* The current file has no numeric suffix. */
            if (snprintf(from, sizeof(from), "%s", base) >=
                (int)sizeof(from)) {
                jw__set_slug(reason, reason_size, "path-too-long");
                return -1;
            }
        }
        if (rename(from, to) != 0 && errno != ENOENT) {
            jw__set_slug(reason, reason_size, "rotate-failed");
            return -1;
        }
    }
    return 0;
}

int jw_svc_launch_open_log(const char *logs_dir, const char *service_id,
                           char *reason, size_t reason_size) {
    if (jw__ensure_service_log_dir(logs_dir, service_id, reason,
                                   reason_size) != 0) {
        return -1;
    }

    char base[PATH_MAX];
    if (snprintf(base, sizeof(base), "%s/services/%s/%s.log", logs_dir,
                 service_id, service_id) >= (int)sizeof(base)) {
        jw__set_slug(reason, reason_size, "path-too-long");
        return -1;
    }

    /* Rotate only when the current file has reached the cap; a file that
     * has not is appended to. Rotation happens at OPEN time (once per
     * service generation), so a generation never rewrites history -- it
     * starts a fresh current file exactly when the previous one filled. */
    struct stat st;
    bool have_current = (lstat(base, &st) == 0);
    if (have_current && !S_ISREG(st.st_mode)) {
        jw__set_slug(reason, reason_size, "open-failed");
        return -1;
    }
    if (have_current && st.st_size >= JW_SVC_LOG_MAX_BYTES) {
        if (jw__log_rotate(base, reason, reason_size) != 0) {
            return -1;
        }
    }

    int fd = open(base, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if (fd < 0) {
        jw__set_slug(reason, reason_size, "open-failed");
        return -1;
    }

    /* The child inherits a duplicate; the parent's copy must not leak
     * into an unrelated exec. CLOEXEC on the parent copy is the safe
     * default -- jw_svc_launch() dup2()s it explicitly in the child. */
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
    return fd;
}

/* Applies env_json (a flat {"NAME":"value"} object) to the caller's
 * environment via setenv(). Done in the PARENT before fork so a
 * malformed object fails the launch instead of the child. Returns 0 on
 * success, -1 on a parse/shape error. */
static int jw__apply_env_json(const char *env_json) {
    if (!env_json || env_json[0] == '\0') {
        return 0;
    }
    cJSON *root = cJSON_Parse(env_json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }
    int rc = 0;
    for (const cJSON *entry = root->child; entry; entry = entry->next) {
        if (!entry->string || !cJSON_IsString(entry) ||
            !entry->valuestring) {
            rc = -1;
            break;
        }
        if (setenv(entry->string, entry->valuestring, 1) != 0) {
            rc = -1;
            break;
        }
    }
    cJSON_Delete(root);
    return rc;
}

/* Child-side pre-exec failure: report a single byte tag into the error
 * pipe, then exit. Never returns. Async-signal-safe. */
static void jw__child_fail(int err_fd, char tag) {
    if (err_fd >= 0) {
        ssize_t ignored = write(err_fd, &tag, 1);
        (void)ignored;
    }
    _exit(127);
}

pid_t jw_svc_launch(const jw_svc_launch_request *req,
                    char *reason, size_t reason_size) {
    if (!req || !req->run_path_abs || req->run_path_abs[0] == '\0' ||
        req->args_count < 0 || req->args_count > JW_SVC_LAUNCH_MAX_ARGS ||
        (req->args_count > 0 && !req->args) || req->lease_fd < 0) {
        jw__set_slug(reason, reason_size, "invalid-request");
        return -1;
    }

    /* Apply the environment before forking so a bad object fails the
     * launch cleanly. */
    if (jw__apply_env_json(req->env_json) != 0) {
        jw__set_slug(reason, reason_size, "env-parse-failed");
        return -1;
    }

    /* The error pipe lets the child report a pre-exec setup failure
     * synchronously: CLOEXEC means a successful exec closes the write
     * end and the parent's read() returns 0 (EOF); any byte means the
     * child died before exec and reports which step failed. */
    int err_pipe[2] = {-1, -1};
    if (pipe(err_pipe) != 0) {
        jw__set_slug(reason, reason_size, "fork-failed");
        return -1;
    }
    int flags = fcntl(err_pipe[1], F_GETFD);
    if (flags >= 0) {
        fcntl(err_pipe[1], F_SETFD, flags | FD_CLOEXEC);
    }

    /* Build argv in the parent (it may allocate) so the child only
     * exec()s. argv[0] is run_path_abs; run.args follow. */
    int argc = 1 + req->args_count;
    char **argv = calloc((size_t)argc + 1u, sizeof(char *));
    if (!argv) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        jw__set_slug(reason, reason_size, "fork-failed");
        return -1;
    }
    argv[0] = (char *)req->run_path_abs;
    for (int i = 0; i < req->args_count; i++) {
        argv[1 + i] = req->args[i] ? (char *)req->args[i] : (char *)"";
    }
    argv[argc] = NULL;

#if defined(__linux__)
    pid_t parent_pid = getpid();
#endif
    pid_t pid = fork();
    if (pid < 0) {
        free(argv);
        close(err_pipe[0]);
        close(err_pipe[1]);
        jw__set_slug(reason, reason_size, "fork-failed");
        return -1;
    }

    if (pid == 0) {
        /* ---- child: async-signal-safe operations only until exec ---- */
        close(err_pipe[0]);

        /* Descriptor 3 is RESERVED for the lease, but the parent's own
         * lease_fd / log_fd may themselves numerically be 3 (a parent
         * with only stdio open hands out fd 3 for the first open). Move
         * each working descriptor to a private high number FIRST so the
         * later dup2(...,3) for the lease can never clobber one. */
        int lease_work =
            fcntl(req->lease_fd, F_DUPFD, JW_SVC_LAUNCH_LEASE_FD + 1);
        if (lease_work < 0) {
            jw__child_fail(err_pipe[1], 'l');
        }
        int log_work;
        if (req->log_fd >= 0) {
            log_work = fcntl(req->log_fd, F_DUPFD, JW_SVC_LAUNCH_LEASE_FD + 1);
            if (log_work < 0) {
                jw__child_fail(err_pipe[1], 'o');
            }
        } else {
            log_work = open("/dev/null", O_WRONLY);
            if (log_work < 0) {
                jw__child_fail(err_pipe[1], 'o');
            }
        }

        if (setpgid(0, 0) != 0) {
            jw__child_fail(err_pipe[1], 'g');
        }

        /* The inherited generation lease on reserved descriptor 3,
         * FD_CLOEXEC cleared so it rides across exec. */
        if (dup2(lease_work, JW_SVC_LAUNCH_LEASE_FD) < 0) {
            jw__child_fail(err_pipe[1], 'l');
        }
        int lflags = fcntl(JW_SVC_LAUNCH_LEASE_FD, F_GETFD);
        if (lflags < 0 ||
            fcntl(JW_SVC_LAUNCH_LEASE_FD, F_SETFD, lflags & ~FD_CLOEXEC) <
                0) {
            jw__child_fail(err_pipe[1], 'l');
        }
        if (setenv(JW_SVC_LAUNCH_LEASE_ENV, "3", 1) != 0) {
            jw__child_fail(err_pipe[1], 'l');
        }

#if defined(__linux__)
        /* SVC-1 "Descendant survival": die when Jawaka does. Set, then
         * close the set-then-recheck race: if our parent already
         * changed (died and we were reparented) between fork() and this
         * prctl(), PDEATHSIG was armed against the NEW parent and may
         * never fire the way SVC-1 requires, so we exit rather than run
         * unsupervised. This must compare against parent_pid (captured
         * before fork(), so both processes agree on the value) rather
         * than the constant 1: a bare "getppid() == 1" reparent check is
         * wrong whenever the real parent already IS pid 1 in its
         * namespace (e.g. this process is a container's init), which
         * misfires exit(127) on every launch despite a live parent. */
        if (prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0) != 0) {
            jw__child_fail(err_pipe[1], 'p');
        }
        if (getppid() != parent_pid) {
            _exit(127);
        }
#endif

        /* stdout/stderr into the rotating log (or /dev/null). */
        if (dup2(log_work, STDOUT_FILENO) < 0 ||
            dup2(log_work, STDERR_FILENO) < 0) {
            jw__child_fail(err_pipe[1], 'o');
        }

        execv(req->run_path_abs, argv);
        /* Only reached if execv fails. */
        jw__child_fail(err_pipe[1], 'x');
    }

    /* ---- parent ---- */
    free(argv);
    close(err_pipe[1]);

    /* Block until the child either execs (read returns 0) or reports a
     * pre-exec failure (read returns 1). This is bounded: the child
     * reaches exec or a fail path without waiting on anything external. */
    char tag = 0;
    ssize_t n = read(err_pipe[0], &tag, 1);
    close(err_pipe[0]);

    if (n > 0) {
        /* The child failed before exec and already _exit()ed. Reap it so
         * it does not linger as a zombie the caller never asked for --
         * the reservation rule applies to a LAUNCHED service, not to one
         * that never got past setup. */
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        const char *slug = "fork-failed";
        if (tag == 'o') {
            slug = "open-log-failed";
        }
        jw__set_slug(reason, reason_size, slug);
        return -1;
    }

    /* n == 0: exec succeeded (CLOEXEC closed the pipe). n < 0: an
     * interrupted/failed read we cannot distinguish from success
     * safely -- but the child exists either way and is now the caller's
     * reservation responsibility, so report the pid. */
    return pid;
}
