#ifndef JW_CORE_LOG_H
#define JW_CORE_LOG_H

/* The format attribute is load-bearing: a mismatched argument list here reads
   a garbage vararg and segfaults the daemon at the log call, so let the
   compiler reject it instead. */
void jw_log_impl(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define jw_log_info(...)  jw_log_impl("INFO", __VA_ARGS__)
#define jw_log_warn(...)  jw_log_impl("WARN", __VA_ARGS__)
#define jw_log_error(...) jw_log_impl("ERROR", __VA_ARGS__)

#endif
