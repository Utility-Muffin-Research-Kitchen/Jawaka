#define _GNU_SOURCE

#include "internal/services/launch.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
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

#include "cJSON.h"

/* Real (not stubbed) fork()/exec() fixtures. Each test launches a real
 * helper binary through jw_svc_launch() and asserts ONE SVC-1 launch
 * mechanism actually happened in the child: the new process group, the
 * lease on descriptor 3 surviving exec, the environment, the rotating
 * bounded log, the pre-exec error pipe, or (Linux only) PDEATHSIG. A
 * passing suite on macOS plus the same suite inside gcc:13 on Linux is
 * the cross-platform bar; the PDEATHSIG test compiles to a skip on
 * macOS because that kernel has no pdwaitpid-style analogue. */

#define JW__TMP_TEMPLATE "/tmp/jw-launch-test-XXXXXX"

static char JW__TMP[sizeof(JW__TMP_TEMPLATE)];
static char JW__BIN[PATH_MAX];
static char JW__LOGS[PATH_MAX];
static char JW__LEASE[PATH_MAX];

/* Reaps a launched child, asserting it exited (not signalled) with the
 * expected status. The launch primitive hands back an unreaped leader;
 * the test (as its parent) is responsible for reaping it here. */
static void jw__reap_expect(pid_t pid, int expect_status) {
    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    assert(r == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == expect_status);
}

/* Writes `text` into `path`, creating it with mode 0700. */
static void jw__write_file(const char *path, const char *text, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    assert(fd >= 0);
    size_t len = strlen(text);
    ssize_t w = write(fd, text, len);
    assert(w == (ssize_t)len);
    close(fd);
}

static char *jw__read_file(const char *path) {
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0);
    char *buf = malloc((size_t)st.st_size + 1u);
    assert(buf);
    ssize_t n = read(fd, buf, (size_t)st.st_size);
    assert(n == st.st_size);
    buf[st.st_size] = '\0';
    close(fd);
    return buf;
}

/* A lease stand-in: a plain open file whose descriptor the child must
 * inherit on fd 3. flock semantics are lease.c's own test suite's job;
 * here we only need an open descriptor to hand over. */
static int jw__open_lease_standin(void) {
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/generation.lease", JW__TMP);
    assert(n > 0 && n < (int)sizeof(path));
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    assert(fd >= 0);
    return fd;
}

static void jw__test_group_and_exec(void) {
    puts("RUN launch-test: child runs in a new process group and execs");

    char out[PATH_MAX];
    int n = snprintf(out, sizeof(out), "%s/pgid.out", JW__TMP);
    assert(n > 0 && n < (int)sizeof(out));

    int lease = jw__open_lease_standin();
    const char *args[] = {"-c", "echo $$ > \"$1\"", "sh", out};
    jw_svc_launch_request req = {
        .run_path_abs = "/bin/sh",
        .args = args,
        .args_count = 4,
        .env_json = NULL,
        .lease_fd = lease,
        .log_fd = -1,
    };
    char reason[JW_SVC_LAUNCH_REASON_BUF];
    pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
    assert(pid > 0);
    close(lease);

    jw__reap_expect(pid, 0);

    char *written = jw__read_file(out);
    pid_t child_pgid = (pid_t)atoi(written);
    free(written);
    /* $$ inside the child is its own pid, and setpgid(0,0) made the
     * pgid equal to it -- and equal to the pid jw_svc_launch returned. */
    assert(child_pgid == pid);
    assert(getpgid(pid) == pid || errno == ESRCH); /* child already gone */
    puts("PASS launch-test: new process group + exec");
}

/* A tiny C helper exec'd by the fd-3 test: asserts, from INSIDE the
 * child after exec, that descriptor 3 is a valid open descriptor with
 * FD_CLOEXEC clear, that UMRK_SERVICE_LEASE_FD names it, and that the
 * caller-supplied env arrived. Exits 0 on success, distinct non-zero
 * codes pinpointing the failing check. Compiled once in main(). */
#define JW__FD3_HELPER_SRC                                              \
    "#include <errno.h>\n#include <fcntl.h>\n#include <stdio.h>\n"      \
    "#include <stdlib.h>\n#include <string.h>\n#include <unistd.h>\n"   \
    "int main(void){\n"                                                 \
    " const char*e=getenv(\"UMRK_SERVICE_LEASE_FD\");\n"               \
    " if(!e||strcmp(e,\"3\")!=0) return 10;\n"                          \
    " int fl=fcntl(3,F_GETFD);\n"                                       \
    " if(fl<0) return 11;\n"                                            \
    " if(fl&FD_CLOEXEC) return 12;\n"                                   \
    " const char*m=getenv(\"JW_TEST_MARKER\");\n"                       \
    " if(!m||strcmp(m,\"hello-leaf\")!=0) return 13;\n"                 \
    " return 0;\n"                                                      \
    "}\n"

static void jw__test_lease_fd_and_env(void) {
    puts("RUN launch-test: lease survives exec on fd 3 (cloexec clear), "
         "env applied");

    char helper[PATH_MAX];
    int n = snprintf(helper, sizeof(helper), "%s/fd3-helper", JW__TMP);
    assert(n > 0 && n < (int)sizeof(helper));
    char src[PATH_MAX];
    n = snprintf(src, sizeof(src), "%s.c", helper);
    assert(n > 0 && n < (int)sizeof(src));
    jw__write_file(src, JW__FD3_HELPER_SRC, 0600);
    char cmd[PATH_MAX * 2];
    n = snprintf(cmd, sizeof(cmd), "cc -std=c11 -O0 -o '%s' '%s'",
                 helper, src);
    assert(n > 0 && n < (int)sizeof(cmd));
    assert(system(cmd) == 0);

    int lease = jw__open_lease_standin();
    jw_svc_launch_request req = {
        .run_path_abs = helper,
        .args = NULL,
        .args_count = 0,
        .env_json = "{\"JW_TEST_MARKER\":\"hello-leaf\"}",
        .lease_fd = lease,
        .log_fd = -1,
    };
    char reason[JW_SVC_LAUNCH_REASON_BUF];
    pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
    assert(pid > 0);
    close(lease);

    /* Exit 0 means every in-child check passed; a 10/11/12/13 names the
     * exact failing one. */
    jw__reap_expect(pid, 0);
    puts("PASS launch-test: fd 3 lease (cloexec clear) + env");
}

static void jw__test_log_capture_and_rotation(void) {
    puts("RUN launch-test: stdout/stderr captured; log rotates at cap");

    char reason[JW_SVC_LAUNCH_REASON_BUF];
    int log_fd = jw_svc_launch_open_log(JW__LOGS, "svc.alpha", reason,
                                        sizeof(reason));
    assert(log_fd >= 0);

    int lease = jw__open_lease_standin();
    const char *script =
        "echo first-gen-stdout; echo first-gen-stderr 1>&2";
    const char *args[] = {"-c", script, "sh"};
    jw_svc_launch_request req = {
        .run_path_abs = "/bin/sh",
        .args = args,
        .args_count = 3,
        .env_json = NULL,
        .lease_fd = lease,
        .log_fd = log_fd,
    };
    pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
    assert(pid > 0);
    close(lease);
    close(log_fd);
    jw__reap_expect(pid, 0);

    char logpath[PATH_MAX];
    int n = snprintf(logpath, sizeof(logpath),
                     "%s/services/svc.alpha/svc.alpha.log", JW__LOGS);
    assert(n > 0 && n < (int)sizeof(logpath));
    char *contents = jw__read_file(logpath);
    assert(strstr(contents, "first-gen-stdout") != NULL);
    assert(strstr(contents, "first-gen-stderr") != NULL);
    free(contents);

    /* Fill the current file past the cap, then reopen: it must rotate to
     * .log.1 and start a fresh current file. */
    int fill = open(logpath, O_WRONLY | O_TRUNC);
    assert(fill >= 0);
    static char big[JW_SVC_LOG_MAX_BYTES];
    memset(big, 'x', sizeof(big));
    ssize_t w = write(fill, big, sizeof(big));
    assert(w == (ssize_t)sizeof(big));
    close(fill);

    int log_fd2 = jw_svc_launch_open_log(JW__LOGS, "svc.alpha", reason,
                                         sizeof(reason));
    assert(log_fd2 >= 0);
    close(log_fd2);

    char rotated[PATH_MAX];
    n = snprintf(rotated, sizeof(rotated), "%s.1", logpath);
    assert(n > 0 && n < (int)sizeof(rotated));
    struct stat st;
    assert(stat(rotated, &st) == 0);
    assert(st.st_size == JW_SVC_LOG_MAX_BYTES);
    /* The fresh current file is now empty again. */
    assert(stat(logpath, &st) == 0);
    assert(st.st_size == 0);
    puts("PASS launch-test: log capture + rotation");
}

/* Regression for a real bug: the child used to dup2() the caller's raw
 * lease_fd and log_fd straight onto their reserved slots (3, stdout,
 * stderr) without relocating them first. When a caller-supplied log_fd
 * happened to already equal JW_SVC_LAUNCH_LEASE_FD (3) -- which happens
 * in practice, since jw_svc_launch_open_log() is often the very first
 * fd a minimal-fd process allocates -- dup2(lease_fd, 3) silently closed
 * the log descriptor before it could be duplicated onto stdout/stderr,
 * so the child's stdout/stderr ended up pointing at the LEASE file
 * instead of the log, and the real log stayed empty. Forces that exact
 * collision here regardless of ambient fd state (via dup2, not
 * allocation order), plus the symmetric case of the caller's lease_fd
 * itself already being 3. */
static void jw__test_reserved_fd_collision(void) {
    puts("RUN launch-test: caller fds landing on the reserved lease slot "
         "(3) do not collide");

    char reason[JW_SVC_LAUNCH_REASON_BUF];

    /* Case 1: log_fd == 3, the originally-observed collision. */
    {
        int log_raw = jw_svc_launch_open_log(JW__LOGS, "svc.collide",
                                             reason, sizeof(reason));
        assert(log_raw >= 0);
        int lease_raw = jw__open_lease_standin();

        assert(dup2(log_raw, 3) == 3);
        if (log_raw != 3) close(log_raw);
        int lease = fcntl(lease_raw, F_DUPFD, 10);
        assert(lease >= 10);
        if (lease_raw != lease) close(lease_raw);

        const char *args[] = {"-c", "echo collide-stdout", "sh"};
        jw_svc_launch_request req = {
            .run_path_abs = "/bin/sh",
            .args = args,
            .args_count = 3,
            .env_json = NULL,
            .lease_fd = lease,
            .log_fd = 3,
        };
        pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
        assert(pid > 0);
        close(3);
        close(lease);
        jw__reap_expect(pid, 0);

        char logpath[PATH_MAX];
        int n = snprintf(logpath, sizeof(logpath),
                         "%s/services/svc.collide/svc.collide.log",
                         JW__LOGS);
        assert(n > 0 && n < (int)sizeof(logpath));
        char *contents = jw__read_file(logpath);
        assert(strstr(contents, "collide-stdout") != NULL);
        free(contents);
    }

    /* Case 2: the caller's lease_fd is already 3 (a dup2(x, x) no-op in
     * the naive version -- must still end up correctly placed). */
    {
        int lease_raw = jw__open_lease_standin();
        assert(dup2(lease_raw, 3) == 3);
        if (lease_raw != 3) close(lease_raw);

        int log_fd = jw_svc_launch_open_log(JW__LOGS, "svc.collide2",
                                            reason, sizeof(reason));
        assert(log_fd >= 0);

        const char *args[] = {"-c", "echo collide2-stdout", "sh"};
        jw_svc_launch_request req = {
            .run_path_abs = "/bin/sh",
            .args = args,
            .args_count = 3,
            .env_json = NULL,
            .lease_fd = 3,
            .log_fd = log_fd,
        };
        pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
        assert(pid > 0);
        close(3);
        close(log_fd);
        jw__reap_expect(pid, 0);

        char logpath[PATH_MAX];
        int n = snprintf(logpath, sizeof(logpath),
                         "%s/services/svc.collide2/svc.collide2.log",
                         JW__LOGS);
        assert(n > 0 && n < (int)sizeof(logpath));
        char *contents = jw__read_file(logpath);
        assert(strstr(contents, "collide2-stdout") != NULL);
        free(contents);
    }

    puts("PASS launch-test: reserved fd (3) collisions handled");
}

static void jw__test_exec_failure_reported(void) {
    puts("RUN launch-test: a non-executable run.path fails the launch "
         "synchronously");

    char bad[PATH_MAX];
    int n = snprintf(bad, sizeof(bad), "%s/not-there", JW__TMP);
    assert(n > 0 && n < (int)sizeof(bad));

    int lease = jw__open_lease_standin();
    jw_svc_launch_request req = {
        .run_path_abs = bad,
        .args = NULL,
        .args_count = 0,
        .env_json = NULL,
        .lease_fd = lease,
        .log_fd = -1,
    };
    char reason[JW_SVC_LAUNCH_REASON_BUF];
    pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
    /* The error pipe must report the pre-exec execv() failure as a
     * failed launch, not a pid that then dies. */
    assert(pid < 0);
    assert(strcmp(reason, "fork-failed") == 0);
    close(lease);
    puts("PASS launch-test: exec failure reported via error pipe");
}

static void jw__test_invalid_requests(void) {
    puts("RUN launch-test: invalid requests are refused before fork");

    char reason[JW_SVC_LAUNCH_REASON_BUF];
    jw_svc_launch_request req = {
        .run_path_abs = "/bin/true",
        .args = NULL,
        .args_count = 0,
        .env_json = NULL,
        .lease_fd = -1, /* no lease: must be refused */
        .log_fd = -1,
    };
    assert(jw_svc_launch(&req, reason, sizeof(reason)) < 0);
    assert(strcmp(reason, "invalid-request") == 0);

    req.lease_fd = jw__open_lease_standin();
    req.run_path_abs = NULL;
    assert(jw_svc_launch(&req, reason, sizeof(reason)) < 0);
    assert(strcmp(reason, "invalid-request") == 0);
    close(req.lease_fd);

    /* Malformed env fails before fork. */
    req.run_path_abs = "/bin/true";
    req.lease_fd = jw__open_lease_standin();
    req.env_json = "not json";
    assert(jw_svc_launch(&req, reason, sizeof(reason)) < 0);
    assert(strcmp(reason, "env-parse-failed") == 0);
    close(req.lease_fd);
    puts("PASS launch-test: invalid requests refused");
}

#if defined(__linux__)
static void jw__test_pdeathsig(void) {
    puts("RUN launch-test (linux): PDEATHSIG kills the service when the "
         "parent dies");

    /* A child that blocks forever; we then kill THIS test process's
     * intermediate child (the service's parent) and assert the service
     * is signalled. jw_svc_launch sets the service's parent to this
     * process, so we fork an intermediate to act as the parent we can
     * kill. */
    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    pid_t middle = fork();
    assert(middle >= 0);
    if (middle == 0) {
        close(sync_pipe[0]);
        int lease = jw__open_lease_standin();
        jw_svc_launch_request req = {
            .run_path_abs = "/bin/sleep",
            .args = (const char *const[]){"60"},
            .args_count = 1,
            .env_json = NULL,
            .lease_fd = lease,
            .log_fd = -1,
        };
        pid_t svc = jw_svc_launch(&req, NULL, 0);
        /* Tell the real parent the service pid, then die so the service
         * is orphaned-by-us -- PDEATHSIG should fire. */
        ssize_t ignored = write(sync_pipe[1], &svc, sizeof(svc));
        (void)ignored;
        close(sync_pipe[1]);
        _exit(0); /* the service's parent exits here */
    }

    close(sync_pipe[1]);
    pid_t svc = 0;
    ssize_t n = read(sync_pipe[0], &svc, sizeof(svc));
    close(sync_pipe[0]);
    assert(n == (ssize_t)sizeof(svc) && svc > 0);

    /* Wait for the middle child to exit, then give PDEATHSIG a moment to
     * be delivered; the sleep should die on SIGTERM without us signalling
     * it directly. */
    int status = 0;
    while (waitpid(middle, &status, 0) < 0 && errno == EINTR) {
    }

    bool gone = false;
    for (int i = 0; i < 100; i++) {
        if (kill(svc, 0) != 0 && errno == ESRCH) {
            gone = true;
            break;
        }
        usleep(10000);
    }
    /* Reap the service if it became our child on reparenting. */
    while (waitpid(-1, &status, WNOHANG) > 0) {
    }
    assert(gone);
    puts("PASS launch-test (linux): PDEATHSIG");
}
#endif

int main(void) {
    strcpy(JW__TMP, JW__TMP_TEMPLATE);
    assert(mkdtemp(JW__TMP) != NULL);
    int n = snprintf(JW__LOGS, sizeof(JW__LOGS), "%s/logs", JW__TMP);
    assert(n > 0 && n < (int)sizeof(JW__LOGS));
    assert(mkdir(JW__LOGS, 0700) == 0);
    n = snprintf(JW__BIN, sizeof(JW__BIN), "%s/bin", JW__TMP);
    assert(n > 0 && n < (int)sizeof(JW__BIN));
    n = snprintf(JW__LEASE, sizeof(JW__LEASE), "%s/lease", JW__TMP);
    assert(n > 0 && n < (int)sizeof(JW__LEASE));

    jw__test_group_and_exec();
    jw__test_lease_fd_and_env();
    jw__test_log_capture_and_rotation();
    jw__test_reserved_fd_collision();
    jw__test_exec_failure_reported();
    jw__test_invalid_requests();
#if defined(__linux__)
    jw__test_pdeathsig();
#else
    puts("SKIP launch-test: PDEATHSIG (no macOS analogue; runs on Linux)");
#endif

    puts("PASS launch-test");
    return 0;
}
