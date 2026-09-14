#ifndef JW_LAUNCH_NOTICE_H
#define JW_LAUNCH_NOTICE_H

#include "internal/launcher/system_activity.h"

#include <stdbool.h>
#include <stdint.h>

/* A launch the user asked for did not happen. Unlike system-activity feedback,
   navigation never clears the notice: only a new launch attempt does, and the
   lifetime expires on its own after JW_SYSTEM_NOTICE_MS. Pure so it can be
   tested natively; the launcher owns the rendering and the frame requests. */

static inline void jw_launch_notice_clear(jw_system_notice *notice) {
    notice->text[0] = '\0';
    notice->started_ms = 0;
}

/* Start of a launch attempt: a retry replaces the previous refusal instead of
   stacking on it or inheriting its remaining lifetime. */
static inline void jw_launch_notice_begin(jw_system_notice *notice) {
    jw_launch_notice_clear(notice);
}

static inline void jw_launch_notice_show(jw_system_notice *notice,
                                         const char *text, uint32_t now) {
    jw_system_notice_set(notice, text, now);
}

static inline uint32_t jw_launch_notice_remaining(const jw_system_notice *notice,
                                                  uint32_t now) {
    return jw_system_notice_remaining(notice, now);
}

static inline const char *jw_launch_notice_text(const jw_system_notice *notice,
                                                uint32_t now) {
    return jw_launch_notice_remaining(notice, now) ? notice->text : "";
}

/* Expiry tick. Returns true on the one tick that removed an expired notice, so
   the caller can request the frame that erases the pill. Reads never clear, so
   a view that draws late cannot shorten the lifetime. */
static inline bool jw_launch_notice_expire(jw_system_notice *notice, uint32_t now) {
    if (jw_system_notice_remaining(notice, now)) return false;
    if (!notice->text[0]) return false;
    jw_launch_notice_clear(notice);
    return true;
}

#endif /* JW_LAUNCH_NOTICE_H */
