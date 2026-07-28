/* mkdtemp(), kill(), and PATH_MAX need broader-than-bare-C11 visibility on
 * glibc; see the matching comment in lease.c. Must precede every
 * #include. */
#define _GNU_SOURCE

#include "internal/services/lease.h"

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define JW_TEST_REASON_BUF 64

static void jw__test_mkdtemp(const char *suffix, char *out, size_t out_size) {
    int n = snprintf(out, out_size, "/tmp/jw-lease-test-%s.XXXXXX", suffix);
    assert(n > 0 && (size_t)n < out_size);
    assert(mkdtemp(out) != NULL);
}

static ino_t jw__test_fd_inode(int fd) {
    struct stat st;
    assert(fstat(fd, &st) == 0);
    return st.st_ino;
}

static void jw__test_basic_acquire_and_contention(void) {
    char runtime_dir[512]; jw__test_mkdtemp("basic", runtime_dir, sizeof(runtime_dir));

    char reason[JW_TEST_REASON_BUF] = {0};
    int fd1 = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd1 >= 0);

    /* Same-process double open: flock() treats two independently open()'d
     * descriptors on the same file as unrelated, even within one process,
     * so this genuinely exercises the conflict path without forking. */
    memset(reason, 0, sizeof(reason));
    int fd2 = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd2 == -1);
    assert(strcmp(reason, "stale-generation") == 0);

    ino_t inode_before = jw__test_fd_inode(fd1);
    assert(close(fd1) == 0); /* releases the flock: last (only) holder closed it */

    memset(reason, 0, sizeof(reason));
    int fd3 = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd3 >= 0);
    /* The lease file must never be recreated -- same inode across the
     * release/reacquire cycle, not a fresh file that happens to have the
     * same path. */
    assert(jw__test_fd_inode(fd3) == inode_before);
    assert(close(fd3) == 0);

    puts("PASS lease-test basic acquire, same-process contention, inode preserved");
}

static void jw__test_invalid_service_ids(void) {
    char runtime_dir[512]; jw__test_mkdtemp("invalid-id", runtime_dir, sizeof(runtime_dir));
    char reason[JW_TEST_REASON_BUF];

    static const char *bad_ids[] = {"", "a/b", "..", ".", "/etc"};
    for (size_t i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); i++) {
        memset(reason, 0, sizeof(reason));
        int fd = jw_svc_lease_acquire(runtime_dir, bad_ids[i], reason, sizeof(reason));
        if (fd != -1 || strcmp(reason, "invalid-service-id") != 0) {
            fprintf(stderr, "FAIL invalid service_id %s: fd=%d reason=%s\n",
                    bad_ids[i], fd, reason);
            assert(false);
        }
    }

    memset(reason, 0, sizeof(reason));
    int fd_null = jw_svc_lease_acquire(runtime_dir, NULL, reason, sizeof(reason));
    assert(fd_null == -1);
    assert(strcmp(reason, "invalid-service-id") == 0);

    puts("PASS lease-test rejects invalid service ids");
}

static void jw__test_runtime_dir_unavailable(void) {
    char reason[JW_TEST_REASON_BUF] = {0};
    int fd = jw_svc_lease_acquire("/nonexistent/jw-lease-test-root", "org.umrk.test",
                                  reason, sizeof(reason));
    assert(fd == -1);
    assert(strcmp(reason, "runtime-dir-unavailable") == 0);
    puts("PASS lease-test rejects an unavailable runtime_dir");
}

static void jw__test_directories_are_owner_only(void) {
    char runtime_dir[512]; jw__test_mkdtemp("perms", runtime_dir, sizeof(runtime_dir));
    char reason[JW_TEST_REASON_BUF] = {0};
    int fd = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd >= 0);

    char services_dir[600], service_dir[600];
    snprintf(services_dir, sizeof(services_dir), "%s/services", runtime_dir);
    snprintf(service_dir, sizeof(service_dir), "%s/services/org.umrk.test", runtime_dir);

    struct stat st;
    assert(stat(services_dir, &st) == 0);
    assert((st.st_mode & 0777) == 0700);
    assert(stat(service_dir, &st) == 0);
    assert((st.st_mode & 0777) == 0700);

    assert(close(fd) == 0);
    puts("PASS lease-test creates owner-only directories");
}

/* The scenario contracts.md's daemon-restart-overlap section exists for:
 * a real separate process (a stand-in for an old Jawaka generation, or a
 * cooperating service descendant) holds the lease; a new attempt must see
 * stale-generation while it lives, and must succeed only once it is
 * actually gone -- not merely once some fixed time has elapsed. */
static void jw__test_cross_process_contention(void) {
    char runtime_dir[512]; jw__test_mkdtemp("cross-proc", runtime_dir, sizeof(runtime_dir));

    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    pid_t holder = fork();
    assert(holder >= 0);
    if (holder == 0) {
        close(ready_pipe[0]);
        char reason[JW_TEST_REASON_BUF] = {0};
        int fd = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
        char result = (fd >= 0) ? 'Y' : 'N';
        assert(write(ready_pipe[1], &result, 1) == 1);
        close(ready_pipe[1]);
        if (fd < 0) {
            _exit(1);
        }
        pause(); /* hold the lease open until killed */
        _exit(0);
    }

    close(ready_pipe[1]);
    char result = '\0';
    assert(read(ready_pipe[0], &result, 1) == 1);
    close(ready_pipe[0]);
    assert(result == 'Y');

    char reason[JW_TEST_REASON_BUF] = {0};
    int fd_contended = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd_contended == -1);
    assert(strcmp(reason, "stale-generation") == 0);

    assert(kill(holder, SIGKILL) == 0);
    int status = 0;
    assert(waitpid(holder, &status, 0) == holder);
    /* The kernel releases flock()s held by a process automatically when
     * its last descriptor closes, including on process death -- no
     * separate cleanup step is needed or expected here. */

    memset(reason, 0, sizeof(reason));
    int fd_after = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd_after >= 0);
    assert(close(fd_after) == 0);

    puts("PASS lease-test cross-process contention: stale while held, "
         "acquirable once the holder is gone");
}

int main(void) {
    jw__test_basic_acquire_and_contention();
    jw__test_invalid_service_ids();
    jw__test_runtime_dir_unavailable();
    jw__test_directories_are_owner_only();
    jw__test_cross_process_contention();
    puts("PASS lease-test");
    return 0;
}
