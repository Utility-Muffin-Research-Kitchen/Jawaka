#ifndef JW_CORE_LOG_H
#define JW_CORE_LOG_H

/* The format attribute is load-bearing: a mismatched argument list here reads
   a garbage vararg and segfaults the daemon at the log call, so let the
   compiler reject it instead. */
void jw_log_impl(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* When the filesystem behind stderr has gone read-only, point stdout and
   stderr at <fallback_dir>/<name>.log instead. Children started afterwards
   inherit the new descriptors; children already running keep the old ones.
   The fallback is capped at 2 MB with one rotation, checked when it is opened.
   Returns 1 when it redirected, 0 when nothing was needed, -1 on failure.
   Never logs through jw_log_impl, so it cannot recurse. */
int jw_log_redirect_if_read_only(const char *fallback_dir, const char *name);

#define jw_log_info(...)  jw_log_impl("INFO", __VA_ARGS__)
#define jw_log_warn(...)  jw_log_impl("WARN", __VA_ARGS__)
#define jw_log_error(...) jw_log_impl("ERROR", __VA_ARGS__)

#endif
