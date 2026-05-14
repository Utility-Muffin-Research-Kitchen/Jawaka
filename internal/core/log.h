#ifndef JW_CORE_LOG_H
#define JW_CORE_LOG_H

void jw_log_impl(const char *level, const char *fmt, ...);

#define jw_log_info(...)  jw_log_impl("INFO", __VA_ARGS__)
#define jw_log_warn(...)  jw_log_impl("WARN", __VA_ARGS__)
#define jw_log_error(...) jw_log_impl("ERROR", __VA_ARGS__)

#endif
