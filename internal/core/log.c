#include "internal/core/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define JW__LOG_FALLBACK_MAX_BYTES (2L * 1024L * 1024L)

static bool jw__log_fd_read_only(int fd) {
    struct statvfs vfs;
    return fstatvfs(fd, &vfs) == 0 && (vfs.f_flag & ST_RDONLY) != 0;
}

int jw_log_redirect_if_read_only(const char *fallback_dir, const char *name) {
    if (!fallback_dir || !fallback_dir[0] || !name || !name[0] ||
        !jw__log_fd_read_only(STDERR_FILENO)) {
        return 0;
    }
    char path[512];
    char rotated[520];
    if (snprintf(path, sizeof(path), "%s/%s.log", fallback_dir, name) >= (int)sizeof(path)) {
        return -1;
    }
    snprintf(rotated, sizeof(rotated), "%s.1", path);
    (void)mkdir(fallback_dir, 0755);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > JW__LOG_FALLBACK_MAX_BYTES) {
        (void)rename(path, rotated);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    if (jw__log_fd_read_only(fd)) {
        close(fd);
        return -1;
    }
    fflush(stdout);
    fflush(stderr);
    int rc = (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) ? -1 : 1;
    close(fd);
    clearerr(stdout);
    clearerr(stderr);
    return rc;
}

void jw_log_impl(const char *level, const char *fmt, ...) {
    char timestamp[32];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_now);

    fprintf(stderr, "%s %s ", timestamp, level);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);
    fflush(stderr);
}
