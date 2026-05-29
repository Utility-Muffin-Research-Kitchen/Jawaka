#include "loong_lifecycle.h"

#if defined(PLATFORM_MLP1)

#include <dlfcn.h>
#include <stdbool.h>
#include "../core/log.h"

/* C entry points exported by /usr/lib/libloong_sdk.so. They forward to
 * loong::ServiceApi::eventOpend / eventClosing over the loong PComm pipe. */
static int (*s_event_opend)(const char *id);
static int (*s_event_closing)(const char *id, const char *param);
static bool s_loaded;
static bool s_home_ready_sent;

static void jw__loong_load(void) {
    if (s_loaded) return;
    s_loaded = true;

    void *h = dlopen("/usr/lib/libloong_sdk.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        jw_log_error("loong: dlopen libloong_sdk.so failed: %s", dlerror());
        return;
    }
    /* dlsym returns void*; assign through a void** to avoid the ISO C pedantic
     * object->function pointer cast warning (POSIX guarantees this works). */
    *(void **)(&s_event_opend)   = dlsym(h, "EventOpend");
    *(void **)(&s_event_closing) = dlsym(h, "EventClosing");
    if (!s_event_opend)
        jw_log_error("loong: EventOpend symbol not found: %s", dlerror());
}

void jw_loong_notify_home_ready(void) {
    if (s_home_ready_sent) return;
    jw__loong_load();
    if (!s_event_opend) return;
    int r = s_event_opend("HOME");
    jw_log_info("loong: EventOpend(\"HOME\") -> %d (dismissing boot transition)", r);
    s_home_ready_sent = true;
}

void jw_loong_notify_home_closing(void) {
    jw__loong_load();
    if (s_event_closing) s_event_closing("HOME", "");
}

#else /* non-MLP1: no stock loong lifecycle to integrate with */

void jw_loong_notify_home_ready(void) {}
void jw_loong_notify_home_closing(void) {}

#endif
