#include "internal/core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

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
